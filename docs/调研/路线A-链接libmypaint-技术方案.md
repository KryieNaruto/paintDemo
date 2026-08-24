# 路线 A · 链接 libmypaint 库 + Vulkan Compute — 详细技术方案

> **定位**：本方案是「三插拔接口」架构下 `IPaintKernel = libmypaint（链接 C 库，CPU 生成 dab）` + `IRenderBackend = Vulkan Compute（GPU 合成）` 的原始规划路线（即 DGCPaint_技术规划.md 的路线 A）。
> **状态**：评审中四维得分 **3.33**（最低）。本文档客观给出可执行技术细节，同时如实说明为何得分低、何时该放弃切换路线 E。
> **日期**：2026-08-20
> **编制依据**：DGCPaint_技术规划.md、笔刷渲染技术路线评审.md、绘画内核功能清单.md、mypaint_ffi（pub.dev）、Krita Android 移植、libmypaint 官方构建文档。

---

## 1. 技术路线概览

**一句话定位**：把成熟的 libmypaint 笔刷引擎作为 `IPaintKernel` 的「黑盒」实现直接链接进 app，由它在 CPU 端生成 dab（stamp），再由自研 Vulkan Compute 在 GPU 端把 stamp 批量合成到画布——保留 Procreate/Krita 验证过的手感，代价是背上 glib/json-c 交叉编译这座「黑洞」。

```
┌────────────────────────────────────────────────────────────────────┐
│                         UI 层（插拔）                                │
│     Android Pad：Jetpack Compose        PC：ImGui + GLFW             │
└──────────────────────────────┬─────────────────────────────────────┘
                               │ 引擎控制 API（设笔刷/颜色/undo）
┌──────────────────────────────▼─────────────────────────────────────┐
│                      engine（平台无关核心，3 线程）                   │
│        Input Thread ──▶ Brush Thread ──▶ Render Thread               │
│   (Ink Stroke Modeler)  (IPaintKernel)     (IRenderBackend)         │
└──────┬──────────────────────────┬───────────────────────────────────┘
       │ IPaintKernel（插拔点①）    │ IRenderBackend（插拔点②）
┌──────▼──────────────────────┐   ┌─▼───────────────────────────────┐
│  libmypaint（链接 .so/.a）    │   │  Vulkan Compute（自研）          │
│  · mypaint_brush_stroke_to   │   │  · canvas storage image         │
│  · MyPaintSurface 回调适配    │   │  · stamp 纹理池 + staging       │
│  · 依赖：glib shim + json-c   │   │  · brush_composite.comp         │
│  （CPU 生成 dab）             │   │  （GPU 合成 dab→画布）           │
└──────────────────────────────┘   └─────────────────────────────────┘
                ▲                            ▲
                └────── IPlatform（插拔点③：surface/input/lifecycle）──┐
                                                               ┌───────▼───────┐
                                                               │ Android / PC  │
                                                               └───────────────┘
```

**数据流向**：`MotionEvent → Ink Stroke Modeler 平滑预测点流 → libmypaint stroke_to → MyPaintSurface::draw_dab → StampData → Vulkan Compute over 合成 → present`。

**路线 A 与路线 E 的唯一区别**：路线 E 把 `mypaint_brush_stroke_to()` 算法抄成自研 C++；路线 A 直接链接官方 C 库。合成侧（Vulkan Compute）两者完全相同，因此本方案第 3.c、3.d、4 节对路线 E 同样适用。

---

## 2. 总体架构

### 2.1 libmypaint 作为 IPaintKernel 实现

```
core/interfaces/i_paint_kernel.h
        ▲
        │ implements
kernels/mypaint/mypaint_kernel.h/.cpp      ← 门面：BrushHandle 管理、stroke 生命周期
kernels/mypaint/mypaint_surface.h/.cpp     ← MyPaintSurface 虚函数适配 → StampData
        │ links
third_party/libmypaint/{arm64-v8a,host}/libmypaint.a  + include/
third_party/json-c/{arm64-v8a,host}/libjson-c.a       + include/
```

- `mypaint_kernel.cpp` 持有一个 `MyPaintBrush*` 句柄池（`BrushHandle = uint32_t` 索引），把 `IPaintKernel` 的四个方法映射到 libmypaint 的 `mypaint_brush_new/from_defaults`、`mypaint_brush_stroke_to`、`mypaint_brush_reset`。
- `mypaint_surface.cpp` 提供一个 `MyPaintSurface` 子类，把 `draw_dab` / `get_color` / `begin_atomic` / `end_atomic` 回调转成 `std::vector<StampData>` 输出。
- 关键点：**libmypaint 是纯 C、有内部全局状态、非线程安全**。因此所有 libmypaint 调用必须收敛到单一的 Brush Thread，绝不能在 Input/Render 线程直接调。

### 2.2 MyPaintSurface 适配到 StampData

libmypaint 不关心「画布怎么合成」，它只通过 `MyPaintSurface` 虚函数表回调上层。路线 A 的适配层就是把这个回调填成「收集 StampData」。这是链接路线相对白盒移植的**唯一省力点**：不需要理解 dab 数量计算 / 传感器滤波 / 响应曲线的内部实现，只消费结果。

### 2.3 3 线程模型

| 线程 | 职责 | 输入 | 输出 | 同步 |
|---|---|---|---|---|
| **Input Thread**（Main/Ink） | Ink Stroke Modeler 平滑预测 | MotionEvent | `StrokePoint`（含 `isPredicted`） | SPSC ring buffer → Brush |
| **Brush Thread** | `mypaint_brush_stroke_to` 生成 dab → 适配层收集 StampData | `StrokePoint` | `StampData` | SPSC ring buffer → Render |
| **Render Thread** | staging 上传 + compute 批量合成 + present | `StampData` | 屏幕帧 | fence + semaphore |

- 两条 ring buffer 都用 `core/ring_buffer.h`（无锁 SPSC），容量取 1024，写满丢新点（预测点优先丢，真实点不可丢）。
- Brush Thread 与 Render Thread 之间**不做帧同步**：Render Thread 每帧开始把当前累积的 stamp 全部消费（drain），Brush Thread 只负责塞。这是「低延迟优先」的取舍——stamp 迟到就下一帧合成，绝不让 Render Thread 等 Brush Thread。
- libmypaint 单例 + 无锁队列的约束：**一个 brush 句柄同一时刻只能被一个 stroke 使用**；多指绘制阶段不做（非目标）。

---

## 3. 核心模块设计

### 3.a libmypaint 交叉编译方案（核心难点，本路线的成败所在）

#### 3.a.1 总体策略：把「黑洞」收敛为「一次性产物」

交叉编译是 AI 执行最弱的环节（晦涩 C 库、config.h 生成、glib 宏）。路线 A 的对策是**把它压成一次性的、可脚本化、可 vendor 的步骤**，而不是让它在每次 app 构建里复发：

```
一次性（tools/build_mypaint_*.sh 脚本执行，AI 只需调一次并验证）：
  git clone libmypaint + json-c（锁定 commit）
  → 生成/手写 config.h + mypaint-config.h（vendor 进 third_party）
  → NDK standalone toolchain 交叉编译 json-c → libjson-c.a
  → NDK standalone toolchain 交叉编译 libmypaint（--without-glib）→ libmypaint.a
  → 拷贝产物到 third_party/{arm64-v8a,host}/
  → git commit 产物（静态库 + include 头），之后 CI 永不重编

每次 app 构建（CMake）：
  find_library / add_library(imported) 直接链接 third_party 里的预编译 .a
```

**为什么静态库 .a 而非动态 .so**：规划原文说「动态链接 .so 最省事」，但静态 .a 有一个决定性优势——**符号不依赖运行时加载，规避 glib/json-c 的运行时 .so 名冲突与 so 加载顺序问题**。ISC 许可两者都允许。静态链接需注意：链接顺序 `libmypaint.a` 在 `libjson-c.a` 之前，且用 `-Wl,--start-group ... --end-group`（或 CMake 的 `target_link_libraries(... libmypaint libjson-c)` 自动处理重复引用）解决循环/重排依赖。符号合并冲突（若 main 也用了 json-c）用 `-Wl,--allow-multiple-definition` 或改 json-c 符号前缀规避，原型阶段大概率不会撞。

#### 3.a.2 三个必踩坑 + 对策（已由 mypaint_ffi / Krita 验证）

| 坑 | 现象 | 对策（具体） |
|---|---|---|
| **config.h / mypaint-config.h 缺失** | `fatal error: 'config.h' file not found`（`fifo.c` 等源文件无条件 include） | 不删 include；在 host 上 `./autogen.sh && ./configure --without-glib --disable-introspection` 生成 config.h，再把 `mypaint-config.h` 一起 vendor 进 `third_party/libmypaint/include/`。产物作为头文件入仓 |
| **glib 头必须保留** | `mypaint-brush.c` 无条件 `#include "glib/mypaint-brush.h"`，删目录即编译失败 | 保留 libmypaint 源码树内自带的 `glib/` 兼容 shim 目录（这就是「glib 头保留在源码树」的由来），`MYPAINT_CONFIG_USE_GLIB=0` 时其内容基本编译为空，但文件必须存在 |
| **NDK 版本兼容** | 新 NDK（r27/r28）下 glib 的 `_FILE_OFFSET_BITS` / `_GNU_SOURCE` 宏与 64 位 off_t 冲突 | 见 3.a.3 版本选择 |

#### 3.a.3 NDK 版本选择：双 toolchain 隔离

- **主 app 构建**：NDK r27+（AGP 8.9+ 强制，规划 §1.1）。负责编译我们的 C++ 核心、Vulkan、JNI。
- **libmypaint 一次性构建**：用**独立的 standalone toolchain**，推荐 **NDK r18b**（Krita Android 移植的验证版本，glib shim + 老 autotools 兼容最好）。r18b 的产物是 `.a` + `.h`，与主 app 的 r27+ 完全 ABI 兼容（同为 arm64-v8a，`ANDROID_PLATFORM` 需 ≤ 主 app 的 minSdk 30）。
- **若坚持只用 r27+**：需在编译 libmypaint 时显式 `CFLAGS="-D_FILE_OFFSET_BITS=64"` 并统一 `-D_GNU_SOURCE`，且 json-c 的 `_LARGEFILE64_SOURCE` 要一致；风险高，不推荐。**结论：libmypaint/json-c 用 r18b standalone 编译，产预编译库入仓，主 app 用 r27+ 只链接。**

#### 3.a.4 build 脚本流程（`tools/build_mypaint_android.sh`，可执行伪码）

```bash
#!/usr/bin/env bash
set -euo pipefail
# 一次性交叉编译 libmypaint + json-c → 预编译 .a 入仓
LIBMYP_VER=1.6.1            # 锁定版本（避免上游漂移）
JSONC_VER=0.17
NDK=${NDK_R18B:?set NDK_R18B to standalone r18b path}
TOOL=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=30
ARCH=arm64-v8a
CC=$TOOL/bin/aarch64-linux-android30-clang

mkdir -p third_party/json-c/$ARCH third_party/libmypaint/$ARCH

# 1) json-c（小型 C 库，交叉编译简单）
curl -L https://github.com/json-c/json-c/archive/json-c-$JSONC_VER.tar.gz | tar xz
cd json-c-* && mkdir b && cd b
cmake .. -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
         -DANDROID_ABI=$ARCH -DANDROID_PLATFORM=android-$API \
         -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build . -j && cp libjson-c.a ../../third_party/json-c/$ARCH/

# 2) 生成 config.h + mypaint-config.h（host 上跑一次，产出后 vendor）
#    在 host 目录: ./autogen.sh && ./configure --without-glib --disable-introspection
#    把生成的两个头拷到 third_party/libmypaint/include/ 并 commit
#    关键：mypaint-config.h 里 MYPAINT_CONFIG_USE_GLIB 必须为 0

# 3) libmypaint（autotools + NDK 交叉编译）
git clone --depth 1 --branch v$LIBMYP_VER https://github.com/mypaint/libmypaint.git
cd libmypaint
./autogen.sh   # 或用 release tarball 自带 configure
CC=$CC AR=$TOOL/bin/llvm-ar RANLIB=$TOOL/bin/llvm-ranlib \
  ./configure --host=aarch64-linux-android \
              --without-glib --disable-introspection \
              --disable-shared --enable-static \
              CPPFLAGS="-I$PWD/../../third_party/json-c -I$PWD/../../third_party/libmypaint/include"
make -j
cp .libs/libmypaint.a ../../third_party/libmypaint/$ARCH/
cp -r libmypaint-config.h mypaint-config.h ../../third_party/libmypaint/include/ 2>/dev/null || true
```

#### 3.a.5 arm64 + host 双产物

| 产物 | 用途 | 编译方式 |
|---|---|---|
| `third_party/libmypaint/arm64-v8a/`（libmypaint.a + libjson-c.a + include） | Android 真机 `.so` | NDK r18b standalone 交叉编译 |
| `third_party/libmypaint/host/`（libmypaint.a + libjson-c.a + include） | PC 可执行 + host 单元测试（对照测试） | 系统 gcc/clang `./configure --without-glib --disable-introspection && make` |
| `third_party/mypaint-brushes/`（笔刷预设 `.myb` JSON 文件） | 运行时笔刷预设 | 直接 clone 官方 mypaint-brushes 仓库，作为 APK assets / PC 资源 |

- host 产物还承担一个重要职责：**路线 A 的正确性对照**——host 版 libmypaint 是官方的「黄金实现」，任何路径切换/性能优化（如第 6 节的 read() 回归修复）都以 host 输出为 diff 基准。
- mypaint-brushes 预设 `.myb` 是 JSON，由 libmypaint 内部用 json-c 解析；原型阶段只需加载 2~3 个基础预设（圆头笔/纹理笔），其余不进包。

---

### 3.b MyPaintSurface → StampData 适配层

libmypaint 通过虚函数表回调上层。适配层把 dab 回调翻译成渲染后端能吃的 `StampData`。

```cpp
// kernels/mypaint/mypaint_surface.h
// C++ 侧自定义 surface：内嵌 libmypaint 的 C 结构体头部，重写虚函数指针
struct MyPaintSurfaceAdapter {
    MyPaintSurface base;                 // 必须首成员，C 兼容
    std::vector<StampData>* out;         // 回调输出（单 stroke 累积）
    std::function<StampColor(float,float)> pickColor;  // get_color 实现来源
};

// kernels/mypaint/mypaint_surface.cpp
static void draw_dab(MyPaintSurface* self_,
                     float x, float y,
                     float radius,
                     float color_r, float color_g, float color_b,
                     float opaque, float hardness,
                     float alpha_eraser,
                     float aspect_ratio, float angle,
                     float lock_alpha, float colorize)
{
    auto* s = reinterpret_cast<MyPaintSurfaceAdapter*>(self_);
    StampData d;
    d.x = x; d.y = y;
    d.radius = radius;
    // libmypaint 输出的是 straight RGB（非预乘），合成 shader 里统一转预乘
    d.color = { color_r, color_g, color_b, opaque };   // opaque 已含压力映射
    d.hardness = hardness;
    d.aspect_ratio = aspect_ratio;
    d.angle = angle;
    d.eraser = alpha_eraser > 0.5f;
    d.lock_alpha = lock_alpha > 0.5f;
    d.colorize = colorize > 0.5f;
    s->out->push_back(d);
}

static void get_color(MyPaintSurface* self_,
                      float x, float y, float radius,
                      float* color_r, float* color_g, float* color_b, float* color_a)
{
    // 原型阶段：返回当前笔刷颜色即可（无拾色反馈）。后续接吸管工具时，从
    // Canvas storage image 读回一个像素（读回会 stall，需走「降采样读回」或异步）
    auto* s = reinterpret_cast<MyPaintSurfaceAdapter*>(self_);
    auto c = s->pickColor ? s->pickColor(x, y) : StampColor{*color_r,*color_g,*color_b,1.0f};
    *color_r = c.r; *color_g = c.g; *color_b = c.b; *color_a = c.a;
}

static int  begin_atomic(MyPaintSurface* self_) { (void)self_; return 0; }
static void end_atomic(MyPaintSurface* self_, MyPaintRectangle* roi) {
    // 原型单帧合成本身就是原子的，roi 可忽略（或用于包围盒 hint）
    (void)self_; (void)roi;
}
```

**关键点**：
- `draw_dab` 的 `opaque` 已经是 libmypaint 按「压力→不透明度响应曲线 + opaque_multiply + opaque_linearize」处理后的**最终** alpha，适配层**不要再二次乘压力**（否则手感变重）。
- `radius` 是「基本半径」（dab 中心半径），合成时 stamp 纹理需按 `radius * aspect_ratio` 生成形状，`angle` 控制旋转——原型先只做圆形 + hardness，aspect/angle/纹理后续补。
- `get_color` 的**读回陷阱**：若实现「吸管取色」会迫使 GPU 读回 Canvas，破坏低延迟。原型阶段 `get_color` 直接返回当前前景色，吸管工具降级为「取调色板色」，不做画布读回。
- 一次 `stroke_to` 可能回调**多个** draw_dab（高速移动时 libmypaint 自动补 dab，dabs_per_second / per_radius 逻辑），`out` 收集的 StampData 数量不固定——这是渲染侧「批量 dispatch」的输入来源。

---

### 3.c Vulkan 合成管线（与路线 E 完全共享，详述）

#### 3.c.1 资源与布局

| 资源 | 类型 | 用途 | 布局 |
|---|---|---|---|
| Canvas storage image | `rgba8`（原型）→ 后续 `rgba16f` | 画布像素，compute 直接读写 | `VK_IMAGE_LAYOUT_GENERAL` 常驻（避免每帧 transition） |
| Stamp 纹理池（N=64） | `rgba8` + `VK_IMAGE_USAGE_TRANSFER_DST|SAMPLED` | 每帧的 stamp 形状 alpha | `TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` |
| Staging buffer 池（M=8 环形） | `HOST_VISIBLE|HOST_COHERENT` | CPU 写 stamp 像素 → GPU 拷贝 | — |
| Descriptor set | compute：`image2D`(canvas) + `sampler2D`(stamp) + `push_constant` | 合成参数 | — |
| Command buffer | 每帧重录 compute + present | 合成 + 上屏 | — |
| 同步 | semaphore（acquire→compute→draw→present）+ fence | 帧序 | — |

- stamp 纹理是「笔刷形状 alpha 单通道」的上采样形式，可用 `rg8`/`r8` 存 alpha（原型 `r8` 即可，合成 shader 里 `stamp.a` 取自该通道）。
- 画布 `rgba8` 存**预乘 alpha（premultiplied）**（画世界PRO `blitType` 核心，规划 §4.5），合成走 `over`。

#### 3.c.2 stamp 纹理池 + staging buffer 池（上传 <1ms 的关键）

```
Brush Thread（CPU）                        Render Thread（GPU）
  StampData 向量 ──┐
                   │  memcpy 进 staging（HOST_COHERENT，无 map/unmap）
                   ▼
  staging buffer 池（环形，8×256KB） ── vkCmdCopyBufferToImage ──▶ stamp 纹理池
```

- **为什么要池**：每帧几十~几百个 stamp，逐个 `vkAllocateMemory` + `vkMapMemory` 会击穿上 <1ms 预算。池化后上传 = 一次 `memcpy` + 一次 `vkCmdCopyBufferToImage`，开销可控。
- **为什么 stamp 用 staging 而非 compute 直接写**：compute 生成 stamp 需额外管线，CPU 直接写像素 + 拷贝更简单，AI 友好；后续追求极致性能再评估把 libmypaint dab 形状烘焙成 `shapeTex`（规划 §8.5 启示 3）。
- 环形池写满时的策略：**丢弃最旧的预测 stamp**（`isPredicted` 优先丢），真实 stamp 不丢。

#### 3.c.3 compute shader（`brush_composite.comp`，与规划 §4.5 一致 + 细化）

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform image2D u_Canvas;
layout(set = 0, binding = 1) uniform sampler2D  u_Stamp;

layout(push_constant) uniform PC {
    vec2  stampPos;      // stamp 在画布上的像素坐标（左上）
    vec2  stampSize;     // stamp 纹理像素尺寸
    float opacity;       // 合成不透明度
    float alphaLock;     // 阿尔法锁定（只写已画像素）
    float colorize;      // colorize 模式（HSV 着色，原型可 0）
} pc;

void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_Canvas);
    if (c.x >= sz.x || c.y >= sz.y) return;
    vec2 uv = (vec2(c) - pc.stampPos) / pc.stampSize;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return;
    float a = texture(u_Stamp, uv).r * pc.opacity;
    if (a <= 0.001) return;
    vec4 canvas = imageLoad(u_Canvas, c);
    // alphaLock：只在已画像素区写入（原型预留，先实现）
    if (pc.alphaLock > 0.5 && canvas.a <= 0.001) return;
    // premultiplied over：dst = src + dst*(1-src.a)
    vec3 src = pc.color.rgb * a;
    vec4 outC = vec4(src + canvas.rgb * (1.0 - a), a + canvas.a * (1.0 - a));
    imageStore(u_Canvas, c, outC);
}
```

#### 3.c.4 批量 dispatch + 包围盒

- **单 stroke 一次 dispatch**：一个 stroke 的所有 stamp 放进**一个 storage 数组**（`StampMeta[]`：pos/size/opacity/color/angle），compute 内循环遍历数组内覆盖当前像素的 stamp 逐个 over 叠加——比「每 stamp 一次 dispatch」省掉 N 次 pipeline 提交 overhead。**这是 <2ms 的核心手段**。
- **包围盒**：计算 stroke 所有 stamp 的并集 AABB，只 `vkCmdDispatch(ceil(box/8))`，不 dispatch 全画布。快速一笔横扫 2048px 画布时，包围盒仍是线性长条，需配合 **tile 化**（把长条切成 8×8 tile 列表，逐 tile dispatch）控制单次 dispatch 尺寸。
- **多 stroke 累积**：一帧内多个 stroke 的 stamp 合并到一个批量，按 `(canvas 坐标排序 + tile 分桶)` 减少重复读画布。
- 三种合成扩展位（对应画世界PRO，规划 §4.5）：`layerMask`（蒙版）、`lockAlpha`（阿尔法锁定，原型已实现）、`selMask`（选区裁剪）——原型先只做 lockAlpha，其余留 uniform 位。

---

### 3.d 输入层（Ink Stroke Modeler 平滑预测）

- **依赖**：白盒移植 `ink-stroke-modeler` 进入 `core/stroke_predictor`（Apache-2.0，纯 C++，仅 stdlib+Abseil），在 SDK 内核内完成平滑预测，跨平台复用。
- **平滑预测**：`StrokeModeler::Update` 接收原始 `MotionEvent` 点流 → wobble smoothing + 重采样 + 弹簧质点模型；`StrokeModeler::Predict`（KalmanPredictor / StrokeEndPredictor）产出预测点，标 `isPredicted=true`。
- **确定性**：固定步长欧拉积分**无随机**，天然适配 `dgcSetRandomSeed`/`dgcSetFixedTime`。
- **预测点覆盖策略**（Procreate 同款，规划 §4.7）：预测 stamp 只用于降低观感延迟，**不作为最终像素保留**；真实点到达时以真实 stamp 重合成该段。实现上：Brush Thread 给预测点也生成 stamp 但标 `isPredicted`，Render Thread 的 staging 池满时优先丢预测 stamp；真实点 stamp 到达后覆盖同一画布区域的预测像素（over 运算天然覆盖，但会残留「预测过冲」的拖影——缓解见 §6）。
- **降延迟细节**：Compose 输入用 `PointerEventPass.Initial`；必要时绕过 Compose 直连 `MotionEvent`（规划 §6 风险表）。
- **PC 侧**：GLFW 鼠标/数位笔走同一 `StrokePoint` 结构，统一进 ring buffer；鼠标无压力时 pressure 恒 1.0、tilt 恒 0。

---

## 4. 数据流（触控→上屏完整生命周期，标注耗时预算）

```
触控笔按下
 │  MotionEvent（压力/倾斜/时间戳）
 ├─[1] Ink 点流捕获 + 预测外推 …………… 2ms
 │      StrokePoint{isPredicted} → SPSC ring buffer
 ├─[2] 线程切换 + Brush Thread 取出 …… 1~2ms
 ├─[3] mypaint_brush_stroke_to(x,y,p,tilt) → draw_dab 回调 … <3ms（P5）
 │      → 收集 StampData 向量
 ├─[4] StampData → staging memcpy ……… <1ms（P4）
 ├─[5] vkCmdCopyBufferToImage(staging→stamp tex)
 ├─[6] vkCmdDispatch(brush_composite.comp, 包围盒) … <2ms（P3）
 ├─[7] vkCmdDraw(present) → vkQueuePresent
 └─[8] swapchain vsync 等待 + 显示 ………… 8.3ms@120Hz（1帧）~16.6ms@60Hz
──────────────────────────────────────────────
 合计（未含预测）：~2+2+3+1+2+8.3 ≈ 18ms@120Hz（60Hz 屏 ~26ms）
 感知延迟（含预测）：预测点提前 1~2 帧渲染，观感可压到 <15ms
```

**预算说明**：
- `[3]` libmypaint 单 stamp <3ms（P5）——这是**CPU 侧瓶颈**。快速移动时一次 stroke_to 会出多个 dab，若 dab 过多会突破 3ms，需限制 `dabs_per_basic_radius` 或启用间距（spacing）控制。
- `[8]` swapchain vsync 是固定成本，无法消除；**预测 + 前缓冲**才是真正压低「感知延迟」的手段。路线 A 的预测是自研外推（同路线 E），因为 libmypaint 本身不提供前缓冲渲染。
- 若 `[8]` 用 TextureView 再多一次 SurfaceFlinger 合成拷贝（规划 §4.3），延迟 +1 帧，阶段 5 不达标则切 SurfaceView 独立 surface。

---

## 5. 关键接口设计

### 5.1 MyPaintSurface 回调签名（libmypaint 官方 C 虚函数表）

```c
// third_party/libmypaint/include/mypaint-surface.h（官方，不修改）
typedef struct {
    void (*draw_dab)(MyPaintSurface *self,
                     float x, float y, float radius,
                     float color_r, float color_g, float color_b,
                     float opaque, float hardness,
                     float alpha_eraser,
                     float aspect_ratio, float angle,
                     float lock_alpha, float colorize);
    void (*get_color)(MyPaintSurface *self,
                      float x, float y, float radius,
                      float *color_r, float *color_g, float *color_b, float *color_a);
    int  (*begin_atomic)(MyPaintSurface *self);
    void (*end_atomic)(MyPaintSurface *self, MyPaintRectangle *roi);
    int refcount;
} MyPaintSurface;
```

### 5.2 JNI 暴露（`platform/android/jni_bridge.cpp`）

```c
// 原生层 IPaintKernel 的 JNI 薄封装，Compose/Kotlin 侧直接调用
JNIEXPORT jlong JNICALL Java_com_dgcamp_paint_NativeBrush_createBrush(
    JNIEnv*, jclass, jfloat radius, jfloat hardness, jfloat opacity,
    jint colorR, jint colorG, jint colorB);
JNIEXPORT void JNICALL Java_com_dgcamp_paint_NativeBrush_beginStroke(
    JNIEnv*, jclass, jlong brush, jfloat x, jfloat y, jfloat pressure,
    jfloat tiltX, jfloat tiltY, jlong tUs);
JNIEXPORT void JNICALL Java_com_dgcamp_paint_NativeBrush_strokeTo(
    JNIEnv*, jclass, jlong brush, jfloat x, jfloat y, jfloat pressure,
    jfloat tiltX, jfloat tiltY, jlong tUs);
JNIEXPORT void JNICALL Java_com_dgcamp_paint_NativeBrush_endStroke(
    JNIEnv*, jclass, jlong brush);
```

- `jlong brush` 即 `BrushHandle`（`uint32_t` 打包），`createBrush` 内部 `mypaint_brush_new()` + `mypaint_brush_from_defaults()` + `mypaint_brush_set_base_value()` 设基础参数。
- `strokeTo` 内部 `mypaint_brush_stroke_to(brush, surface, x, y, pressure, 0, 0, dtime)`，其中 `dtime` 由相邻两点 `t_us` 差算出（libmypaint 用它算速度/间距）。
- JNI 只做**参数透传 + 线程切换**（把点 push 进 ring buffer 由 Brush Thread 消费），**绝不在 JNI 线程直接调 libmypaint**。

### 5.3 StampData 字段（`core/types.h` 扩展）

```cpp
struct StampColor { float r, g, b, a; };   // straight（非预乘），合成 shader 里转预乘
struct StampData {
    float x, y;              // dab 中心（画布坐标，像素）
    float radius;            // 基本半径（dab 中心半径）
    float hardness;          // 0~1 边缘软硬（libmypaint hardness 映射）
    StampColor color;        // straight RGBA，a 已含压力不透明度映射
    float aspect_ratio;      // 圆度（1=正圆），原型可忽略
    float angle;             // 旋转，原型可忽略
    bool eraser;             // 橡皮（alpha_eraser）
    bool lock_alpha;         // 阿尔法锁定
    bool colorize;           // HSV 着色（原型 false）
};
```

- `StampData` 不含 alpha 位图——路线 A 中 stamp 的**形状 alpha 在合成 shader 里程序化生成**（圆形距离场 + `hardness` 的 `smoothstep`），等价于画世界PRO 的「shapeTex + 距离场」（规划 §8.3）。若要纹理笔刷，需额外把纹理 alpha 上传成 stamp 纹理，`StampData` 加 `stamp_tex_id` 字段。

---

## 6. 关键技术难点与解决方案

### 6.1 交叉编译黑洞（glib / config.h / NDK）—— 路线 A 得分低的根因

| 难点 | 为什么难 | 解决方案 | 剩余风险 |
|---|---|---|---|
| glib 交叉编译 | glib 是大型 C 库，autotools + 平台宏极其繁琐，AI 调试极弱 | **不交叉编译 glib**：`--without-glib` 用 libmypaint 自带的 `glib/` 兼容 shim（随机数用 `g_random_double` 的 shim 实现，仅需几个 .c 文件），只保留头文件存在 | glib shim 的随机数序列与官方 glib 略有差异（手感影响极微） |
| config.h 生成 | 源文件无条件 include，删掉就编译失败 | host 上跑一次 `configure --without-glib` 生成，vendor 进 third_party 并 commit | 首次生成脚本需 AI 调通一次（一次性成本） |
| NDK 版本 | 新 NDK 宏与旧 autotools 冲突 | libmypaint/json-c 用 r18b standalone 预编译，主 app 用 r27+ 只链接 | r18b 工具链在 CI 上要额外下载/缓存（一次性） |
| json-c | 独立依赖，跨编译 | json-c 很小，CMake 交叉编译简单，一次脚本搞定 | 低 |
| 上游不活跃 | libmypaint 最后提交 2024-09，bug 无人修 | 锁定 commit + vendor 源码，自行打补丁（见 6.2） | 需自行维护 fork |

### 6.2 libmypaint 性能回归（MyPaintSensorPack::read() 每帧调用）

Krita 修复过 MR !1839 类问题：`MyPaintSensorPack::read()` 若在每次 `stroke_to` 内被重复构造/读取，会引入不必要的开销，导致单 stamp 超过 3ms 预算。对策：
- 交叉编译时**打一个小补丁**：缓存 sensor pack 的中间状态，避免每帧重建。
- 用 host 版 libmypaint 做**对照基准**：同一组输入点，分别跑「官方 host 版」和「打了补丁的 arm64 版」，diff dab 数量与位置，确认补丁不改算法只改开销。
- 若补丁后仍 >3ms，回退到「减少 dab 数量」（提高 spacing / 限制 dabs_per_second）——这是 CPU 内核的天然天花板。

### 6.3 黑盒限制：无法 GPU 化（路线 A 相对路线 E 的最大痛点）

- libmypaint 的 dab 生成逻辑（传感器滤波、响应曲线、dab 数量计算）封在 `.a` 黑盒里，**无法把 CPU dab 生成搬到 compute shader**。这意味着性能天花板锁死在「CPU 单 stamp <3ms」。
- 想对齐画世界PRO 的「shapeTex + 距离场全 GPU 化」，只能**另写一套 GPU 内核**（即路线 E 的演进终态 = 路线 B），而路线 A 的 libmypaint 代码无法复用——因为它不是「改自己代码」而是「换依赖」。
- **缓解**：MVP 阶段 CPU dab <3ms 足够（Procreate 早期也是 CPU dab + GPU 合成）；若未来确需 GPU 化，就触发路线 A → E 切换（见 §10）。

### 6.4 预测过冲拖影

预测 stamp 若过冲，真实点到达后 over 合成会在画布上残留「预测画过、真实没到」的像素。对策：
- 预测 lead_time 保守（≤2 帧），且预测 stamp 的 opacity 乘衰减系数（如 0.6）。
- 更稳方案：预测 stamp **不进 canvas**，只在「上屏帧」临时叠加（前缓冲思路），真实点到达重合成。原型先用「标 isPredicted + 真实点覆盖」的简单策略（规划 §4.7），拖影明显再上临时叠加。

### 6.5 get_color 读回 stall

吸管工具若读回 Canvas 会 stall GPU。原型 get_color 返回当前前景色，不做画布读回（§3.b 已述）。

---

## 7. 四维度评审（5 分制 + 一句理由）

| 维度（权重） | 得分 | 理由（客观） |
|---|---|---|
| **复杂度**（25%） | **2.5** | 核心 C++ 逻辑（Vulkan 合成、适配层）AI 很擅长，但 glib/config.h/NDK 交叉编译是 AI 最弱的「晦涩 C 库试错」，且要持续维护 vendored 二进制与补丁 |
| **性能**（30%） | **4.5** | dab 用 Krita/MyPaint 验证过的引擎、合成走 Vulkan compute（同路线 E），CPU dab <3ms 够 MVP；天花板被「CPU dab 无法 GPU 化」锁死 |
| **可控性**（25%） | **3** | 合成侧全自研可控，但 dab 生成是黑盒——改算法/加笔刷类型/调响应曲线只能靠 libmypaint 暴露的 setting 值，改不了内部逻辑 |
| **时间**（20%） | **3** | 理论上链接即用最快，但「交叉编译一次调通 + 长期维护」的隐性时间成本拉低到与移植相当的 3 分 |
| **加权总分** | **3.33** | 复杂度（2.5）与可控性（3）拖后腿，被路线 E（4.18）全面压制 |

**得分低的根因一句话**：性能不差、但「复杂度和可控性」双输——交叉编译黑洞落在 AI 最弱项，且黑盒挡住了 GPU 化演进路径。

---

## 8. 风险清单（重点交叉编译黑洞 + 兜底切换）

| # | 风险 | 概率 | 影响 | 缓解 | 兜底 |
|---|---|---|---|---|---|
| R1 | **交叉编译黑洞**：glib/config.h/NDK 调不通，AI 反复试错烧时间 | 高 | 阻塞 T3，拖垮时间线 | §3.a 的一次性脚本 + r18b 隔离 + vendor 产物；锁定 commit | **阶段 0 设 2 天硬 deadline，超时切路线 E** |
| R2 | libmypaint 上游不活跃，出现未修 bug | 中 | 手感/崩溃 | vendor 源码 + 自行打补丁（§6.2） | 同 R1，切 E |
| R3 | 单 stamp >3ms 性能回归 | 中 | 破 P5 指标 | §6.2 补丁 + 降 dab 数量 | 切 E（自研可 SIMD 优化） |
| R4 | 黑盒无法 GPU 化，性能天花板 | 中（长期） | 无法对齐画世界PRO | 接受 MVP 上限 | 演进到路线 E→B |
| R5 | 预测过冲拖影 | 中 | 观感劣化 | 保守 lead_time + 预测 stamp 降 opacity | 前缓冲临时叠加方案 |
| R6 | TextureView 合成拷贝致延迟超标 | 中 | 破 <30ms | 阶段 5 评估 | 切 SurfaceView 独立 surface |
| R7 | json-c 与 app 其他依赖符号冲突 | 低 | 链接错误 | 静态链接 + 符号前缀 | 换 header-only nlohmann/json（仅 .myb 解析用） |

**核心结论**：R1 是本路线的「黑洞」，所有兜底都指向「阶段 0 验证期设硬 deadline，失败即切路线 E」。路线 E 把 R1/R2/R3/R4 从「外部依赖风险」变成「自己的代码」，是可预期成本。

---

## 9. 分阶段实施计划（AI 执行视角）

> 与规划 §5 对应，但按路线 A 的实际依赖重排，并给每阶段「AI 视角的验收标准」。

### 阶段 0 · 技术风险 spike（最高优先，2 天硬 deadline）

- **目标**：(a) Ink Stroke Modeler 预测参数按绘画场景调优；(b) libmypaint arm64 交叉编译链跑通。
- **AI 执行**：跑 §3.a.4 脚本 → 交叉编译 json-c + libmypaint → host 版 `stroke_to` 出 dab。
- **验收（硬性）**：arm64 版 libmypaint.a 编出，且 host 版用同一组输入 `stroke_to` 输出 >0 个 dab；Ink Stroke Modeler 绘画场景调参结论落文档。
- **关键决策点**：**2 天内交叉编译未通 = 立即切路线 E**（不恋战）。

### 阶段 1 · 接口层 + 多平台骨架

- 定义 `IPaintKernel`/`IRenderBackend`/`IPlatform` + `StrokePoint`/`BrushParams`/`StampData`；CMake 多 toolchain 骨架 + 分层目录。
- **验收**：`host-windows`/`host-linux`/`android-arm64` 三 preset 配置通过；PC 可执行 + Android `.so` 空壳编出（此时不链 libmypaint）。

### 阶段 2 · 渲染后端（Vulkan，实现 IRenderBackend）

- `vk_backend`/`vk_canvas`/`vk_composite` + staging buffer 池 + `brush_composite.comp` + 批量 dispatch + 包围盒。host 可跑。
- **验收**：offscreen 用固定 stamp（无内核）合成出笔刷痕迹。

### 阶段 3 · 绘画内核（libmypaint，实现 IPaintKernel）

- 链接阶段 0 产物：`mypaint_surface` 适配层 + `mypaint_kernel` + JNI 暴露 `createBrush/beginStroke/strokeTo/endStroke`；打包 2~3 个 mypaint-brushes 预设。
- **验收**：JNI 调 libmypaint 生成 stamp → 送入 `vk_composite` 可画（offscreen）。

### 阶段 4 · 平台层 + UI（双平台）

- Android：TextureView + ANativeWindow + swapchain/present + JNI；Compose UI。PC：GLFW + Vulkan surface + ImGui UI。
- **验收**：双平台 Vulkan 画布上屏，UI 能切换笔刷/颜色。

### 阶段 5 · 输入集成

- Android Ink 点流（按阶段 0 结论）+ PC 鼠标/数位笔 → ring buffer；预测点覆盖策略。
- **验收**：双平台笔迹跟随良好，无明显可感知延迟。

### 阶段 6 · 全链路 + 性能测试

- 全链路压测（大 canvas/连续快速笔触）；AGI + RenderDoc + 高速摄影测 §3.3 全部指标。
- **验收**：满足 <30ms / 60fps（120fps）/ 合成<2ms / 上传<1ms / stamp<3ms；产出性能报告 + 路线结论。

---

## 10. 结论（何时选、何时弃）

**这条路线何时选**：
- 团队**已经有一份可用的 libmypaint arm64 预编译产物**（例如从 mypaint_ffi 直接拿现成的 `.so`/`.a` 二进制），交叉编译黑洞已被别人填平；
- 只求**快速验证「libmypaint 手感 + Vulkan 合成」能否达标**，且**明确不打算后续 GPU 化 dab 生成**（接受 CPU dab 天花板）；
- 希望 dab 算法**完全不看源码、零理解成本**，把它当纯黑盒调用。

**这条路线何时该放弃、切到路线 E（白盒移植）**：
1. **阶段 0 的 2 天交叉编译 deadline 没调通**（R1 触发）——立即切，这是最硬的切换信号；
2. 出现需要**改 dab 算法内部逻辑**的需求（加笔刷类型、调响应曲线、单 stamp >3ms 需 SIMD 优化）；
3. 需要**把 dab 生成 GPU 化**以逼近画世界PRO 全 GPU 形态——路线 A 的黑盒挡死这条路，只有路线 E（自研代码）才能平滑演进到路线 B。

**最终判断**：路线 A 的性能与路线 E 相同（同为 CPU dab + Vulkan 合成），但复杂度、可控性、时间全面落后，且**唯一省下的成本（不看源码）恰恰被「交叉编译维护」吃回去**。除非能直接复用现成预编译产物，否则本方案仅作为「兜底/对照基准」保留——**推荐主线走路线 E，路线 A 的 host 版 libmypaint 作为白盒移植的正确性对照（diff dab 输出）继续发挥作用**。

---

## 参考资源

- [mypaint_ffi（libmypaint Android 交叉编译参考实现，含 config.h vendor 细节）](https://pub.dev/packages/mypaint_ffi)
- [mypaint_ffi changelog（glib 目录必须保留 / mypaint-config.h 缺失修复）](https://pub.dev/packages/mypaint_ffi/changelog)
- [Building libmypaint（官方构建文档，--without-glib / config.h 生成）](https://www.mypaint.app/en/docs/contributing/building/libmypaint/)
- [How to make it for Android?（MyPaint 社区，config.h 生成讨论）](https://community.mypaint.app/t/how-to-make-it-for-android/3965/7)
- [Krita Android 构建（NDK r18b、androidbuild.sh）](https://phabricator.kde.org/source/krita/browse/ashwind%252FT13119-mypaint-brush-engine/README.android.md;88320134075eed51796c76bc01b30d709d885281)
