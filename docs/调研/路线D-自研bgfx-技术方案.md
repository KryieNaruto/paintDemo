# 路线 D 详细技术方案 · 自研笔刷 + bgfx 跨 API 渲染

> **路线定位**：CPU 端白盒移植 libmypaint dab 算法（自研笔刷内核）+ bgfx Compute Shader 做跨 API 的 GPU 合成。
> **文档阶段**：技术方案（原型初期设计）
> **编制依据**：`DGCPaint_技术规划.md`、`笔刷渲染技术路线评审.md`、`绘画内核功能清单.md`、bgfx 源码（`bgfx_compute.sh` / `shaderc.cpp` / `terrain.cpp`）
> **日期**：2026-08-20

---

## 1. 技术路线概览

**一句话定位**：dab 生成在 CPU 端自研（白盒移植 libmypaint 的 `stroke_to` 算法，去 glib），渲染合成用 bgfx 的 Compute Shader 抽象层——一次编写 `.sc`，跨 Vulkan / Metal / GLES 多后端编译，牺牲一点「贴近原生 API 的精细控制」，换取「未来 iOS/Metal 近乎免费」的可移植红利。

```
                     ┌──────────────────────────────────────────────┐
                     │            UI 层（插拔·编译期选）             │
                     │     Compose(Kotlin)      /    ImGui(C++)      │
                     └────────────────┬─────────────────────────────┘
                                      │ 稳定引擎 API
                     ┌────────────────▼─────────────────────────────┐
                     │        engine（3 线程 + ring buffer + 预测）   │
                     └───────┬──────────────────────┬───────────────┘
                             │ IPaintKernel          │ IRenderBackend
             ┌───────────────▼──────────────┐  ┌─────▼────────────────────────┐
             │  自研笔刷内核（CPU dab）       │  │  bgfx 渲染后端（render/bgfx/） │
             │  白盒移植 libmypaint stroke_to │  │  shaderc 编译 .sc → 多后端    │
             │  std::mt19937 替代 glib       │  │  Compute 合成 + blit 上屏     │
             │  （与路线 E dab 完全同源）      │  │  Vulkan(今) / Metal(未来免费)  │
             └──────────────────────────────┘  └─────┬────────────────────────┘
                                                     │ platformData.nwh
                             ┌───────────────────────▼────────────────────────┐
                             │  IPlatform（Android ANativeWindow / PC GLFW）   │
                             └────────────────────────────────────────────────┘
```

**与其它路线的关系（关键事实，先讲清楚）**：
- 路线 D 的 **dab 生成与路线 E 完全同源**（都是 CPU 白盒移植 libmypaint 算法），差异只在渲染后端：E 用原生 Vulkan Compute，D 用 bgfx 抽象。
- 路线 D 与路线 B（全 GPU）的差别在于 dab 在 CPU 而非 GPU，合成都在 GPU compute。
- 因此路线 D 的相对优劣，**几乎全部由「渲染后端换成 bgfx」这一件事决定**：多一层 shaderc 编译管线 + compute 抽象不如原生直接，换来 iOS/Metal 可移植性（当前用不上）。

---

## 2. 总体架构

### 2.1 bgfx 后端选择

| 平台 | 首选后端 | bgfx profile | 备选 / 回退 | 说明 |
|---|---|---|---|---|
| Android 平板 | **Vulkan** | `spirv` | GLES 3.1（`essl`）低端 Mali 回退 | minSdk 30，Vulkan 1.1+，对标 §3.3 指标 |
| PC (Windows) | **Vulkan** | `spirv` | D3D11/12（`hlsl`） | 与 Android 同后端，调试一致 |
| PC (Linux) | **Vulkan** | `spirv` | OpenGL 4.3+（`glsl`） | host 开发机 |
| iOS（未来） | **Metal** | `metal` | — | **本路线唯一未被当前需求覆盖的红利** |

当前阶段**锁定 Vulkan**（与原生路线 E 在同一起跑线对齐性能），但保留 `bgfx::RendererType::Count`（自动选择）与 profile 切换能力，使「未来 iOS」从「重写渲染层」降级为「加一个 Metal profile 编译产物」。

### 2.2 dab 生成 + 合成的 bgfx 实现分工

```
dab 生成（CPU，自研笔刷内核）
  stroke_to(x,y,pressure,tilt) ──► 1..N 个 StampData {x,y,radius,hardness,opacity,颜色}
          │                        （算法 = 白盒移植 libmypaint，同路线 E）
          ▼  ring buffer（无锁 SPSC）
合成（GPU，bgfx compute shader）
  staging buffer 池 ──► stamp texture（CPU→GPU 上传）
  bgfx::dispatch(view, cs_brush_composite, Nx, Ny, 1)
     IMAGE2D_RW(s_canvas)  ←── 逐个 stamp 做 premultiplied over，只覆盖包围盒
          ▼
上屏（GPU，bgfx fullscreen quad）
  present.sc 采样 s_canvas ──► 默认 backbuffer ──► bgfx::frame()（内部 swapchain present）
```

### 2.3 与三插拔接口的映射

| 插拔接口 | 实现位置 | 本路线的动作 |
|---|---|---|
| `IPaintKernel` | `kernels/selfmade/` | 白盒移植 libmypaint `stroke_to`（同路线 E 的 dab 部分），**不链接 glib/libmypaint** |
| `IRenderBackend` | `render/bgfx/` | `bgfx_backend.h/.cpp` + `bgfx_canvas` + `bgfx_composite` + `bgfx_shader_loader`，**唯一与路线 E 不同的目录** |
| `IPlatform` | `platform/android` / `platform/pc` | 不变，仅把「提供 surface 给 Vulkan」改为「提供 native window handle 给 bgfx `platformData.nwh`」 |

**核心结论**：换到 bgfx 只新增/替换 `render/bgfx/` 一个子目录；`core/`、`kernels/`、`platform/`、`ui/` 全部复用。这印证了三插拔架构的价值——路线 D 与 E 可以**并存**（`render/vulkan/` 与 `render/bgfx/` 同时编译，CMake option 切换），而非二选一。

---

## 3. 核心模块设计

### 3.a dab 生成方案：自研 CPU（白盒移植），不用 bgfx compute

**选择：CPU 端自研 dab（白盒移植 libmypaint `stroke_to` 算法）。MVP 阶段不把 dab 生成搬进 bgfx compute。**

理由（四条，逐条成立）：

1. **算法形态不适配 GPU 并行**。libmypaint `stroke_to` 是逐 stamp 的顺序状态机：`SensorPack` 滤波（speed gamma/direction filter）→ dab 数量计算 → 逐设置响应曲线（radius/hardness/opacity 对数映射）→ HSV 随机抖动 → 颜色调制。分支密集、状态强依赖（上一个 dab 的速度/方向影响下一个）、随机数序列依赖（每次 stamp 抖动不同）。这类标量串行代码在 SIMT GPU 上：并行度≈1、分支发散严重、随机数要靠额外 seed 序列，**改写成本高、收益几乎为零**。
2. **接口天然是 CPU 侧**。`IPaintKernel::strokeTo()` 返回 `std::vector<StampData>`（CPU 内存）。GPU dab 需把接口改成「返回 GPU buffer 句柄」，破坏接口正交性，且 GPU dab 的输入（压力/倾斜/速度）仍需 CPU 先处理点流，等于只搬了一半。
3. **手感 = libmypaint 现成算法，AI 无法从零调参**（评审附录结论）。自研的正确姿势是「白盒移植」（ISC 许可允许抄），移植产物是 CPU C++ 代码，与 GPU 无关。
4. **GPU dab 是路线 B 的终局形态，作为后续演进而非 MVP**。因为 dab 代码是自己写的 C++（非黑盒），未来把 `radius/hardness` 形状烘焙成 `shapeTex`、把形状生成搬进 compute（对齐画世界PRO shapeTex 模型）是「改自己的代码」，这正是评审 §6.2 的 E→B 演进路径，路线 D 同样适用（只是合成后端已是 bgfx）。

**与路线 B / E 的 dab 算法对比**：

| 维度 | 路线 B（自研 GPU dab） | 路线 E（CPU 白盒移植） | **路线 D（本方案）** |
|---|---|---|---|
| dab 生成位置 | GPU compute shader，从零写 | CPU C++，移植 libmypaint | **CPU C++，移植 libmypaint（同 E）** |
| 手感来源 | 美术人工调参（AI 弱项） | libmypaint 现成算法 | libmypaint 现成算法 |
| 移植规模 | 全量重写 ~3000+ 行 | ~1500–2500 行（有原码对照） | 同 E，~1500–2500 行 |
| 与合成解耦 | 否（同 shader 家族） | 是（IPaintKernel 边界） | 是 |
| 可演进到 GPU | 已是终局 | CPU→GPU 平滑演进 | CPU→GPU 平滑演进 |

**移植范围（MVP 最小集，同评审 §4.4）**：`stroke_to` 主流程 + 传感器包（pressure/speed/tilt 滤波）+ dab 数量计算 + 核心设置映射（radius_logarithmic / hardness / opacity + opaque_multiply/linearize / spacing）+ HSV 颜色抖动 + 圆形 dab 形状。glib 用 `std::mt19937`/`bool`/`std::vector`/`assert` 替代，`.myb` 预设解析用 header-only `nlohmann/json`。**不链接 libmypaint，无交叉编译黑洞。**

### 3.b bgfx Compute Shader 合成（`.sc` 写法 + premultiplied over + 批量 + 包围盒）

bgfx 的 compute shader 用 `.sc` 文件（`cs_` 前缀约定），`#include <bgfx_compute.sh>` 提供跨后端宏：`NUM_THREADS(x,y,z)`、`IMAGE2D_RW/RO/WR`、`BUFFER_RW/RO/WO`、`gl_GlobalInvocationID` 等。**与原生 GLSL compute 语法高度相似，但宏在 shaderc 阶段被翻译成各后端原生写法**（GLSL→ESSL/GLSL、SPIR-V、Metal MSL、HLSL/DXBC）。

**核心合成 shader `cs_brush_composite.sc`（伪代码，具体到可编译）**：

```glsl
// cs_brush_composite.sc  —— 笔刷 stamp 合成（Compute）
// 职责：把一个（或一批）stamp 以 premultiplied over 混合到 canvas 存储图像
$input none                    // compute 无 vertex 输入，shaderc 对 'c' 类型跳过 varyingdef

#include <bgfx_compute.sh>

// 存储图像：canvas（读改写）；格式 rgba8，register 0 对应 bgfx::setImage(0,...)
IMAGE2D_RW(s_canvas, rgba8, 0);

// 只读采样：stamp 形状纹理（可双线性过滤），register 1 对应 bgfx::setTexture(1,...)
SAMPLER2D(s_stamp, 1);

// bgfx uniform（vec4）：xy = stamp 左上像素坐标，zw = stamp 纹理像素尺寸
uniform vec4 u_stampRect;      // 单 stamp 用；批量时改用 BUFFER_RW 传 stamp 数组
// rgb = 颜色（已预乘），a = 总不透明度（opacity * 压力调制）
uniform vec4 u_color;
// xy = canvas 像素尺寸（避免跨后端 imageSize 差异，显式传入）
uniform vec4 u_canvasSize;

NUM_THREADS(8, 8, 1)           // local_size 8×8，对齐原方案 §4.5
void main()
{
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(c, ivec2(u_canvasSize.xy)))) return;   // 越界

    // 归一化到 stamp 局部坐标；包围盒外直接跳过（包围盒 dispatch 的兜底）
    vec2 uv = (vec2(c) - u_stampRect.xy) / u_stampRect.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return;

    vec4 stamp = texture2D(s_stamp, uv);           // 形状 alpha（双线性）
    float a = stamp.a * u_color.a;                 // 有效 alpha
    if (a <= 0.001) return;                        // 全透明早退，省带宽

    vec4 canvas = imageLoad(s_canvas, c);          // 读当前 canvas 像素（预乘存储）

    // premultiplied over：dst = src + dst * (1 - src.a)
    // canvas 与 stamp 均按预乘 alpha 存储 —— 与画世界PRO blit 模型一致（规划 §4.5/附录 A）
    vec4 outC = vec4(stamp.rgb * u_color.rgb * a + canvas.rgb * (1.0 - a),
                     a + canvas.a * (1.0 - a));
    imageStore(s_canvas, c, outC);                 // 写回
}
```

**批量 stamp 的两种形态**（对应功能清单 #110 批量合成）：

- **方案一（MVP，推荐）**：单 stamp 一次 dispatch。因为 compute 合成 <2ms 的预算下，包围盒 dispatch 的 overhead 很低；「批量」优化用「一帧内多个 stamp 累积到同一 canvas、按 dispatch 顺序串行 over」即可，无需一个 shader 处理多 stamp。简单、可控、AI 友好。
- **方案二（进阶，对齐原生路线的批量 dispatch）**：把一帧的 N 个 stamp 的 `{x,y,radius,color,opacity}` 打包进一个 `BUFFER_RW(s_stamps, Stamp, 2)`（structured buffer，register 2），shader 内 `gl_GlobalInvocationID` 的一个维度索引 stamp，另一维度索引像素，一次 dispatch 合成全部。**仅当单 stamp dispatch 成为瓶颈时启用**，因为 bgfx compute 里用 `BUFFER_RW` + 内层循环会增加分支与寄存器压力，边际收益需实测。

**包围盒优化（#111）**：CPU 端由 `StampData` 计算包围盒（`x±radius, y±radius`，含 hardness 外扩 1px），dispatch 尺寸 `ceil(box/8)`，只覆盖 stamp 区域而非全 canvas。大 canvas（≥2048×2048）时按 tile 分组 dispatch（#119）。

**批量合成的 bgfx 调用伪代码**：

```cpp
// 合成一帧内积累的所有 stamp（Render Thread）
void BgfxBackend::composite(const std::vector<StampData>& stamps)
{
    bgfx::setImage(0, m_canvasTex, 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::RGBA8);
    for (const StampData& s : stamps) {
        bgfx::setTexture(1, m_stampSampler, m_stampTex);   // 形状纹理（池内复用）
        float rect[4] = {s.x - s.radius, s.y - s.radius,
                         s.radius * 2.0f, s.radius * 2.0f};
        float col[4]  = {s.r, s.g, s.b, s.opacity};        // rgb 已预乘
        bgfx::setUniform(m_uStampRect, rect);
        bgfx::setUniform(m_uColor,     col);
        uint32_t nx = (uint32_t)ceilf(s.radius * 2.0f / 8.0f);
        uint32_t ny = nx;
        bgfx::dispatch(m_computeView, m_csComposite, nx, ny, 1);  // 只覆盖包围盒
    }
}
```

**清屏 shader `cs_clear_canvas.sc`**（#115）：

```glsl
$input none
#include <bgfx_compute.sh>
IMAGE2D_RW(s_canvas, rgba8, 0);
uniform vec4 u_clearColor;         // 预乘色（纯色背景 + 透明度）
NUM_THREADS(8, 8, 1)
void main()
{
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    imageStore(s_canvas, c, u_clearColor);   // 全 canvas 一次 dispatch
}
```

**上屏 shader `fs_present.sc`**（canvas → backbuffer，fullscreen）：

```glsl
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_canvas, 0);
void main()
{
    gl_FragColor = texture2D(s_canvas, v_texcoord0);
}
```
（配一个 fullscreen triangle 的 `vs_present.sc`，或复用 bgfx 内置屏幕三角形顶点，`bgfx::submit(0, m_presentProgram)` 到默认 view 0。）

### 3.c shaderc 编译管线（`.sc` → 各平台二进制）

**shaderc** 是 bgfx 自带的 shader 编译器（`tools/shaderc`），读 `.sc`，输出 bgfx 自定义二进制格式（版本 `BGFX_SHADER_BIN_VERSION`），compute 产物 chunk 为 `BGFX_CHUNK_MAGIC_CSH`。**每个平台后端一个 profile**，同一份 `.sc` 编译多次：

| 后端 | `--platform` | `--profile` | 产物 |
|---|---|---|---|
| Vulkan | `android` / `linux` / `windows` | `spirv` | SPIR-V |
| Metal | `osx` / `ios` | `metal` | MSL |
| GLES 3.1 | `android` | `essl` | ESSL |
| OpenGL 4.x | `linux` / `windows` | `glsl` | GLSL |
| D3D11/12 | `windows` | `hlsl` | DXBC/DXIL |
| WebGPU | 各平台 | `wgsl` | WGSL |

**关键 CLI**（来自 shaderc.cpp）：`--type` 取 `vertex|fragment|compute`；`-i` 指定 include 目录（指向 bgfx `src/`，以解析 `bgfx_shader.sh`/`bgfx_compute.sh`）；`--varyingdef` 只在 vertex/fragment 需要，**compute 类型自动跳过**（源码 `if ('c' != options.shaderType)`）；`--platform`/`--profile` 决定语言后端。

**CMake 集成方式（推荐）**：

```cmake
# render/bgfx/CMakeLists.txt
# ① bgfx/bx/bimg 作为 vendor 依赖（git submodule 或 FetchContent）
add_subdirectory(third_party/bgfx)            # 官方已提供 CMakeLists.txt（较新版本）
# ② 构建 shaderc 作为 host 工具（构建期跑在开发机，非目标设备）
#    bgfx 自带 shaderc target；若无则自建 add_executable 编译 tools/shaderc

# ③ 声明 .sc → 各平台二进制 的构建规则（每个 profile 一个产物）
set(BGFX_SHADER_INC ${BGFX_DIR}/src)          # 找 bgfx_shader.sh / bgfx_compute.sh
set(SHADER_SRC ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)

foreach(profile spirv essl)                    # Android 需 spirv + essl 两套；PC 需 spirv
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${profile}/cs_brush_composite.bin
        COMMAND shaderc
            -f ${SHADER_SRC}/cs_brush_composite.sc
            -o ${SHADER_OUT}/${profile}/cs_brush_composite.bin
            --type compute
            --platform android                 # 按 toolchain 传入（android/linux/windows）
            --profile ${profile}
            -i ${BGFX_SHADER_INC}
        DEPENDS ${SHADER_SRC}/cs_brush_composite.sc shaderc
        COMMENT "shaderc cs_brush_composite (${profile})")
endforeach()
# cs_clear_canvas / vs_present / fs_present 同理（fs 需 --varyingdef varying.def.sc）
```

产物（`.bin`）作为 assets 打进 APK（Android）/ 放到可执行文件旁（PC），运行时 `bgfx::createProgram(loadShader(mem))` 加载。**注意**：shaderc 是 host 工具，Android 交叉编译时在**开发机**上跑，产出的 `.bin` 是目标后端二进制，不打进 `.so` 的编译，只是构建产物。

### 3.d bgfx 纹理 / 资源管理

| 资源 | bgfx 对象 | 创建方式 | 用途 |
|---|---|---|---|
| Canvas 纹理 | `bgfx::TextureHandle` | `createTexture2D(w,h,false,1,RGBA8, BGFX_TEXTURE_COMPUTE_WRITE|SAMPLER_UVW)` | 存储图像，常驻，compute 读改写 |
| Stamp 纹理 | `bgfx::TextureHandle`（池） | `createTexture2D(radius,radius,false,1,RGBA8, SAMPLER_* | BGFX_TEXTURE_COMPUTE_WRITE)` | dab 形状 alpha，环形池复用 |
| Staging buffer | `bgfx::Memory*`（池） | `bgfx::alloc()` / `makeRef()` | CPU→GPU 上传中转，环形复用 |
| Stamp 批量参数 | `bgfx::DynamicVertexBufferHandle` | `createDynamicVertexBuffer`（方案二批量） | `BUFFER_RW` 的 stamp 数组 |
| 采样器 | `bgfx::UniformHandle` + 默认 sampler | `createUniform("s_stamp", Sampler)` | stamp 双线性采样 |
| 合成 uniform | `bgfx::UniformHandle` | `createUniform("u_color", Vec4)` | 每 stamp 颜色/opacity |

**上传路径（stamp 上传 <1ms，#113/#P4）**：Brush Thread 生成 `StampData` 的像素形状 → 拷贝进 staging `bgfx::Memory` → `bgfx::updateTexture2D(m_stampTex, 0,0,0,0,w,h, mem)` 上传。**bgfx 抽象掉了 Vulkan 的 staging buffer / `vkCmdCopyBufferToImage` / barrier**，上传由 bgfx 内部提交，CPU 侧只需一次 `updateTexture2D` 调用——代码量显著少于原生，代价是少了 staging 池与 barrier 的精细控制（上传带宽大 stamp 时可能微幅劣化）。

**Canvas 布局**：bgfx 内部管理 image layout（Vulkan 的 GENERAL vs SHADER_READ_ONLY 切换由 `Access::ReadWrite` 自动插入 barrier），无需像原生那样手动 transition。这是「少控制」的双面：省心，但也无法做原生级的布局优化。

### 3.e 输入层（Ink Stroke Modeler）

与原生路线**完全一致**（输入层不在本路线的差异范围）。要点沿用规划 §4.6：
- 白盒移植 `ink-stroke-modeler` 进 `core/stroke_predictor`（纯 C++，Apache-2.0），`StrokeModeler::Update` 取点流（含时间戳/压力/倾斜/方向），`StrokeModeler::Predict` 产出预测点，**不引入**任何 Android-only 输入管线（渲染由 bgfx 替代）。
- 预测点：`isPredicted` 标记；真实点到达后以真实 stamp 重合成。
- PC 用 GLFW 鼠标/数位笔，走同一 `StrokePoint` 点流，统一进 `core/ring_buffer.h`。

---

## 4. 数据流（触控 → 上屏完整生命周期 + 耗时预算）

```
触控笔按下（t=0）
  → Ink Stroke Modeler 平滑预测（压力/倾斜/时间戳）           ~1–2ms   [输入管线]
  → 预测外推（速度外推，标 isPredicted）                     ~0.1ms
  → push 无锁 SPSC ring_buffer                               ~0.01ms
  ── Brush Thread ─────────────────────────────────────────────
  → 自研内核 stroke_to(x,y,pressure,tilt)                    < 3ms    [P5 单 stamp]
      生成 1..N 个 StampData（dab 形状 alpha + 位置/半径/颜色）
  → push stamp ring_buffer                                   ~0.01ms
  ── Render Thread（bgfx） ────────────────────────────────────
  → staging 拷贝 + updateTexture2D（stamp 上传）             < 1ms    [P4]
  → bgfx::dispatch(cs_brush_composite, 包围盒 Nx×Ny)         < 2ms    [P3 合成]
  → bgfx::submit(present, fullscreen quad 采样 canvas)       ~0.5ms  [blit]
  → bgfx::frame()（内部 swapchain present）                  ~1–2ms  [vsync 等待为主]
  → SurfaceFlinger 合成 → 屏幕显示                           余量
──────────────────────────────────────────────────────────────────────
  端到端延迟合计                                            < 30ms   [P1]
```

**关键点**：3 线程流水线重叠执行，端到端延迟不是各段之和，而是**最长瓶颈段 + 同步等待**。CPU dab（<3ms）与 GPU 合成（<2ms）并行，帧率受 vsync（60/120fps）主导。bgfx 相比原生 Vulkan 多出的成本集中在「upload/barrier/present 被内部抽象」上——单次 dispatch 本身接近原生，但对「极致压榨每 0.1ms」的调优场景，少了直接抓手。

---

## 5. 关键接口设计

### 5.1 bgfx init / render 流程

```cpp
// bgfx_backend.cpp —— IRenderBackend 实现
void BgfxBackend::init(PlatformSurface surf, int w, int h)
{
    bgfx::Init init;
    init.type = bgfx::RendererType::Vulkan;   // 当前锁定；或 Count 自动
    init.platformData.nwh = surf.nativeWindow; // Android: ANativeWindow*; PC: HWND/X11 window
    init.platformData.ndt = nullptr;
    init.resolution.width  = w;
    init.resolution.height = h;
    bgfx::init(init);

    // 创建 canvas texture（compute 可写 + 可采样）
    m_canvasTex = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA8,
                    BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    // 编译 shader（.bin 资源加载）+ 创建 uniform/sampler
    m_csComposite = bgfx::createProgram(loadShader("cs_brush_composite.bin"), true);
    m_uStampRect  = bgfx::createUniform("u_stampRect", bgfx::UniformType::Vec4);
    m_uColor      = bgfx::createUniform("u_color",     bgfx::UniformType::Vec4);
    // view 0 = 默认 backbuffer（上屏）；view 1 = compute
    bgfx::setViewRect(0, 0, 0, w, h);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0xffffffff, 1.0f, 0);
}

void BgfxBackend::composite(const std::vector<StampData>& stamps)
{
    bgfx::setViewRect(1, 0, 0, w, h);
    bgfx::setImage(0, m_canvasTex, 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::RGBA8);
    for (auto& s : stamps) { /* 见 §3.b，setTexture + setUniform + dispatch 包围盒 */ }
}

void BgfxBackend::present()
{
    bgfx::setTexture(0, m_canvasSampler, m_canvasTex);   // view 0 采样 canvas
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(0, m_presentProgram);                   // fullscreen quad
    bgfx::frame();                                       // 内部 swapchain acquire/present
}

void BgfxBackend::resize(int w, int h)  { bgfx::reset(w, h, BGFX_RESET_VSYNC); /* 重建 canvas */ }
void BgfxBackend::clearCanvas()         { bgfx::dispatch(1, m_csClear, w/8, h/8, 1); }
void BgfxBackend::shutdown()            { bgfx::shutdown(); }
```

### 5.2 IRenderBackend 映射

`IRenderBackend` 接口（规划 §4.0）无需改动，bgfx 语义直接映射：

| 接口方法 | bgfx 实现 |
|---|---|
| `init(PlatformSurface, w, h)` | `bgfx::init` + 资源创建（§5.1） |
| `resize(w, h)` | `bgfx::reset` + 重建 canvas texture |
| `beginFrame()` | 空实现或 `bgfx::touch(0)`（bgfx 无显式 begin，靠 submit 排序） |
| `composite(stamps)` | `bgfx::dispatch` 批量合成（§3.b） |
| `clearCanvas()` | `bgfx::dispatch` 清屏 compute |
| `present()` | `bgfx::submit` fullscreen + `bgfx::frame()` |
| `shutdown()` | `bgfx::shutdown` |

### 5.3 swapchain 处理（与原生路线的差异）

- **bgfx 接管 swapchain**：`bgfx::init` 拿 `platformData.nwh` 内部创建 swapchain；`bgfx::reset` 重建；`bgfx::frame()` 完成 acquire→present。**不需要**手写 `VK_KHR_swapchain`/semaphore/fence（#116/#118 由 bgfx 内建）。
- **TextureView 集成**：bgfx 的 Android swapchain 同样走 `ANativeWindow`，与原生 Vulkan 的 TextureView 集成方式一致（`onSurfaceTextureAvailable` 时 `ANativeWindow_fromSurface` → 传给 `platformData.nwh`）。
- **offscreen 测试**：PC/host 上 `platformData.nwh = nullptr`，渲染到 `bgfx::createFrameBuffer`（render target）而非 swapchain，即可在无窗口 CI 环境验证合成正确性（对应 M2 offscreen 里程碑）。
- **代价**：swapchain 细节被隐藏，若 TextureView 多一次 SurfaceFlinger 拷贝导致延迟超标（规划 §6 中风险），bgfx 侧可调的旋钮（如 `BGFX_RESET_FLIP_AFTER_RENDER`、独立 surface）比原生少，SurfaceView 独立 surface 的评估需确认 bgfx 对 ANativeWindow 直连的支持（支持，但配置路径不同）。

---

## 6. 关键技术难点与解决方案

### 6.1 compute shader 在 bgfx 的灵活性受限（本路线最大扣分项）

**难点**：bgfx compute 抽象了 `Access::ReadWrite` → barrier、image layout 转换、push constant 映射、descriptor 绑定，你**无法**直接使用 Vulkan 特有特性：subgroup 操作、`VK_KHR_synchronization2`、精细 barrier、push constant 精确布局、storage image 的 format 扩展等。对「合成 shader」这种简单 over 运算影响不大，但对「需要极致性能/精细同步」的 GPU dab 或未来复杂滤镜会受限。

**解决方案**：
1. **承认边界**：本路线合成逻辑（premultiplied over）极简单，用不到 subgroup/精细 barrier，受限于「用不上」而非「做不到」。真需要时 bgfx 也提供 `bgfx::Access` 枚举控制读写意图，barrier 由驱动自动正确插入，正确性有保障。
2. **性能敏感点下沉到 shader 内部**：包围盒早退、透明早退、tile 分组，把「省 GPU 时间」做在算法层，而非依赖「原生 barrier 微调」。
3. **保留逃逸舱口**：三插拔架构下，`render/vulkan/` 与 `render/bgfx/` 可并存。若某 feature（如需要 subgroup 的高速模糊）在 bgfx 受限，可只对该 feature 换回原生 Vulkan 实现——接口不变。

### 6.2 shaderc 集成

**难点**：多一层编译管线（`.sc` → shaderc → 多后端 `.bin`）；shaderc 是 host 工具需先编译；CMake 里要为「每个后端 profile × 每个 shader」生成构建规则；include 路径与 varyingdef 配置易错。

**解决方案**：
1. **shaderc 作为 host target**，用 `add_custom_command` 统一规则（§3.c），一个函数 `bgfx_shader(target name sc profile platform)` 封装，避免重复样板。
2. **varyingdef 只配 vertex/fragment**：compute 类型 shaderc 自动跳过（源码确认），合成/清屏 shader 无需 varyingdef，降低出错面。
3. **构建期即验证**：shaderc 编译失败 = 构建失败（早于运行期），AI 可在 host 秒级迭代 shader 语法，反而比「运行时才发现 GLSL 编译错误」更友好。
4. **产物作为资源而非源码**：`.bin` 走 assets 打包，运行时 `loadShader` 一次加载，`bgfx::createProgram` 缓存。

### 6.3 跨平台调试成本（AI 执行的关键阻力）

**难点**：bug 出现时，面对的是 bgfx 抽象层 + 三后端翻译后的 shader，出问题的可能层次多（.sc 逻辑错 / shaderc 翻译错 / bgfx 绑定错 / 后端驱动错），定位成本高于「直接读自己写的原生 Vulkan」。

**解决方案**：
1. **锁定单后端调试**：当前只调 Vulkan（Android + PC 同后端），Metal/GLES 仅在切换后端时引入，避免「多后端矩阵」同时调试。
2. **用 `bgfx::setName` / `dbgText` / `setMarker` 给资源命名**，RenderDoc/AGI 抓帧时资源名可读，把「bgfx 内部句柄」映射回「逻辑 canvas/stamp」。
3. **RenderDoc/AGI 仍然可用**：bgfx 最终 emit 真 Vulkan，抓帧看的是真实 dispatch/barrier/带宽，只是资源名需手动标注。
4. **host offscreen 单测先行**：合成正确性用 host 端 `createFrameBuffer` offscreen 渲染 + CPU 读回比对（golden image），把「算法正确性」与「设备后端问题」分离。
5. **AI 友好度评估**：bgfx 文档 + 示例（compute 用 27-terrain 的 `cs_terrain` 模式）齐全，写 `.sc` 与 C++ 是 AI 强项；但「bgfx 内部行为与预期不符」这类需翻源码的黑盒调试是 AI 弱项——用「锁定单后端 + 资源命名 + offscreen golden 测试」把这类调试压缩到最小。

### 6.4 其它难点

| 难点 | 解决方案 |
|---|---|
| bgfx 上传/barrier 黑盒，stamp 上传 <1ms 难精确打点 | 用 `bgfx::updateTexture2D` 调用点做 CPU 打点；必要时 `bgfx::setMarker` + AGI 抓真耗时 |
| 低端 Mali 的 GLES 回退路径与 Vulkan 路径行为不一致 | MVP 锁定 Vulkan；GLES 回退仅作启动兜底，不承诺性能 |
| 未来 Metal 后端需真机验证（当前无 iOS 设备） | 只保证「同一 .sc 可编译出 MSL」，真机验证推迟到 iOS 需求出现时 |

---

## 7. 四维度评审（客观打分）

| 维度（权重） | 得分 | 理由 |
|---|---|---|
| **复杂度**（25%） | **3.0** | dab 部分同 E（白盒移植，AI 强项）；但多一层 shaderc 编译管线 + bgfx 抽象，比原生 Vulkan 多一套构建与心智负担 |
| **性能**（30%） | **4.0** | compute 是 compile-time 直通后端（SPIR-V），合成性能接近原生；扣分在少了 push constant/barrier/subgroup 精细控制，上传与 present 也黑盒化 |
| **可控性**（25%） | **4.0** | dab 全自研（可控），但渲染层受 bgfx 抽象约束，改底层混合/滤镜不如原生 Vulkan 直接 |
| **时间**（20%） | **3.0** | dab 移植与 E 同速；但 shaderc 集成 + bgfx 学习 + 跨 API 调试会让渲染层落地慢于原生 Vulkan |
| **加权总分** | **3.55** | 与评审一致：复杂度 3 × .25 + 性能 4 × .30 + 可控性 4 × .25 + 时间 3 × .20 |

**为何排在 E（4.18）与 B（4.03）之后（客观论证）**：
- 路线 D 与 E 的 dab 完全相同，**唯一差异是渲染后端换 bgfx**。而 bgfx 在「Android + PC 双平台」这个当前目标下，其核心价值（跨 API 可移植、未来 iOS/Metal 免费）**当前完全没有用武之地**——iOS 不在近期范围。于是 bgfx 只剩成本（shaderc 管线 + compute 抽象受限 + 调试黑盒 + 性能微降），没有收益，复杂度/性能/可控性/时间四项全面不优于 E。
- 对比 B：B 的「GPU dab」是真正的性能/可控天花板（4.03），代价是时间 2.0（从零写）。D 的 dab 在 CPU，性能天花板（4.0）低于 B（5.0），又不如 E 简单（4.18），卡在「既不更简单、也不更强」的中间位置。
- 一句话：**D 的短板不是 bgfx 不好，而是「现在用不上 bgfx 的好」。**

---

## 8. 风险清单

| # | 风险 | 影响 | 概率 | 缓解 |
|---|---|---|---|---|
| R1 | bgfx compute 灵活性受限，复杂滤镜/GPU dab 做不动 | 中 | 中 | 承认边界；逃逸舱口换回 `render/vulkan/` |
| R2 | shaderc 编译管线集成出错（include/varyingdef/profile 错配） | 中 | 中 | 封装 `bgfx_shader()` 函数；compute 跳过 varyingdef；构建期即报错 |
| R3 | bgfx 上传/barrier 黑盒，导致 stamp 上传或合成 <预算难调 | 中 | 低 | 单后端锁定 + AGI/RenderDoc 抓真实耗时；marker 打点 |
| R4 | 跨后端行为不一致（Vulkan vs GLES 回退） | 中 | 低 | MVP 锁 Vulkan，GLES 仅兜底不承诺性能 |
| R5 | 未来 iOS/Metal 红利当前无法真机验证 | 低 | 高 | 只承诺「.sc 可编 MSL」，真机验证推迟 |
| R6 | bgfx 学习/调试成本拖慢 AI 落地 | 中 | 中 | host offscreen golden 测试 + 资源命名 + 锁单后端 |
| R7 | TextureView + bgfx swapchain 延迟超标 | 中 | 低 | 阶段 5 评估 SurfaceView 独立 surface（bgfx 支持 ANativeWindow 直连） |

---

## 9. 分阶段实施计划（AI 执行视角）

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **D0 · spike** | (a) 白盒移植 libmypaint `stroke_to` 核心（去 glib，`std::mt19937`），host 跑通；对照原版 libmypaint diff dab 输出。(b) bgfx 编译 + shaderc 工具链在 host/android 两 toolchain 下跑通，编译出一个 `cs_*.bin` | 移植 dab 输出与原版误差可接受；`.sc → spirv .bin` 构建成功且 `createProgram` 加载 |
| **D1 · 接口桩** | 复用三插拔接口 + CMake 骨架；`render/bgfx/` 空壳实现 `IRenderBackend`，`bgfx::init/frame` 空跑 | host-windows / host-linux / android-arm64 三 preset 通过，bgfx 后端能 init/shutdown |
| **D2 · bgfx 合成** | `cs_brush_composite` + `cs_clear_canvas` + offscreen `createFrameBuffer`；固定 stamp 合成出痕迹 | host offscreen 用固定 stamp 合成出笔刷痕迹，CPU 读回 golden 比对通过 |
| **D3 · 自研内核** | 完成 dab 移植（MVP 核心设置）+ `nlohmann/json` 解析 `.myb`；实现 `IPaintKernel` | JNI 调内核生成 stamp，送入 bgfx compute 可画（offscreen） |
| **D4 · 平台 + UI** | Android TextureView + ANativeWindow → `platformData.nwh`；PC GLFW；Compose/ImGui UI | 双平台看到 bgfx 画布，UI 切笔刷/颜色 |
| **D5 · 输入集成** | Ink Stroke Modeler 平滑预测 + 预测覆盖 + PC 输入统一进 ring buffer | 双平台笔迹跟随良好，无感知延迟 |
| **D6 · 全链路 + 性能** | 压测 + AGI/RenderDoc/高速摄影测 §3.3 全部指标；产出性能报告 | 满足 §3.3；输出「D vs E」渲染后端实测对比 |

> 阶段 D0–D3 与路线 E 高度重叠（dab 移植共享），**若两线并行，dab 移植可复用同一份代码**——这再次说明 D 与 E 的边界只在渲染层。

---

## 10. 结论

**这条路线适合什么情况**：当你（1）未来明确要上 iOS/Metal 或 WebGPU，希望「一次编写多后端」省掉渲染层重写；或（2）团队没有原生图形 API 专家、希望由 bgfx 兜住 swapchain/barrier/驱动差异的复杂度；或（3）需要在一套代码里同时覆盖 Vulkan/GLES/D3D/Metal 多种设备矩阵。bgfx 的 BSD-2 许可 + compile-time 多后端 + 接近原生性能，使其在「多平台渲染」场景是正确选择。

**为何当前排在 E/B 之后**：当前目标锁定「Android 平板 + PC」，iOS 不在近期范围，bgfx 的核心红利（跨 API 可移植）**没有兑现场景**；而它的成本（shaderc 管线、compute 抽象受限、调试黑盒、性能微降）**是现在就要付的**。于是 D 在四项上全面不优于「更简单的 E（原生 Vulkan）」与「更强的 B（GPU dab）」，落得 3.55 的中间分。本质是「为未来 iOS 的期权，现在付了溢价」。

**何时值得选（尤其 iOS/Metal 需求何时冒出）**：一旦产品范围明确加入 iOS/iPad（Metal）甚至 WebGPU——即「双平台」升级为「三平台以上」或出现「Metal 后端」的硬需求——路线 D 的期权立刻行权：dab 内核与合成 shader 的 `.sc` 无需改动，只加一个 `--profile metal` 编译产物 + `RendererType::Metal` 初始化，iOS 渲染层几乎免费；而路线 E（原生 Vulkan）此时要把整个 `render/vulkan/` 重写成 Metal，路线 B 的 GPU compute 同样要面对 Metal 的 MSL 重写。那一刻 D 从「溢价期权」变成「最优解」。因此建议：**把 D 作为「未来 iOS 扩展」的储备方案记入架构，`render/` 目录按「可插拔渲染后端」设计（已满足），当且仅当 iOS/Metal 需求坐实时，再激活 `render/bgfx/` 分支——现在不必开发。**

> **一句话总结**：路线 D 技术可行、方案清晰、dab 与 E 同源，唯一短板是「bgfx 的好处在当前双平台目标下用不上」。它是被「目标范围」而非「技术能力」排到后面的路线。
