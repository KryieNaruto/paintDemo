# 路线 C · libmypaint + Skia 技术方案

> **定位**：libmypaint 生成 dab（CPU）→ Skia GPU 光栅化合成（SkSurface + drawImage + SkBlendMode）→ 上屏。
> **评审加权分**：3.65（当前排在 E「白盒移植+Vulkan」4.18 与 B「自研GPU+Vulkan」4.03 之后）。
> **本文目标**：给出可执行的技术方案，并客观论证——这条路线为何排在 E/B 之后，以及什么情况下它反而值得选。
> **日期**：2026-08-20 · 与《笔刷渲染技术路线评审》v2.0 对齐

---

## 1. 技术路线概览

**一句话定位**：用业界最成熟的 2D 引擎 Skia 替代「Vulkan Compute 手写合成」，把「画 dab」交给 libmypaint、把「合成 dab + 混合模式 + 滤镜」全部交给 Skia，换取最短的从零到「能画 + 有完整 2D 能力」的路径，代价是合成模型适配成本与终局性能天花板。

```
┌────────────────────────────────────────────────────────────────┐
│                  UI 层（插拔·编译期选）                          │
│     Android Compose（BrushPanel/ColorPicker/画布）    PC ImGui   │
└───────────────────────────────┬────────────────────────────────┘
                                │ TextureView(SurfaceTexture) / GLFW
┌───────────────────────────────▼────────────────────────────────┐
│               Native 层（3 线程模型，双平台共享）                │
│                                                                │
│  ┌──────────────┐   ┌──────────────────┐   ┌────────────────┐  │
│  │ Input Thread │   │  Brush Thread     │   │ Render Thread  │  │
│  │ Jetpack Ink  │──►│  libmypaint       │──►│  Skia GPU      │  │
│  │ 预测点流      │   │  stroke_to → dab  │   │  drawImage 合成 │  │
│  │ (ring buffer)│   │  dab → premul 位图 │   │  flush+present │  │
│  └──────────────┘   │ (ring buffer)      │   └────────────────┘  │
│                     └──────────────────┘                        │
│    ▲ 插拔③ IPlatform        ▲ 插拔① IPaintKernel    ▲ 插拔② IRenderBackend │
└────┴─────────────────────────┴──────────────────────┴──────────┘
```

与规划基准（路线 A/E 的 Vulkan Compute）**唯一替换点**是 `render/vulkan/` → `render/skia/`，其余（core 接口、engine 3 线程、libmypaint 内核、Jetpack Ink 输入、平台层）**全部复用，零改动**。这正是三插拔接口设计的价值：换渲染后端 = 换一个 `IRenderBackend` 实现。

---

## 2. 总体架构

### 2.1 libmypaint 与 Skia 的分工

| 职责 | 承担者 | 说明 |
|---|---|---|
| 笔迹 → dab 参数（位置/半径/压力/硬度/不透明度/颜色/倾斜） | **libmypaint（CPU）** | 实现 `IPaintKernel`，产出 `StampData` |
| dab 参数 → 像素级 stamp 位图（alpha 形状 + premul 颜色） | **Skia 侧 CPU 栅格化**（本路线的适配成本所在） | 见 §3.b |
| stamp 位图 → 画布合成（混合模式/不透明度） | **Skia GPU**：`SkCanvas::drawImage` + `SkBlendMode` | 实现 `IRenderBackend::composite` |
| 图层/蒙版/选区/滤镜 | **Skia**：`saveLayer` / `clipShader` / `SkImageFilter` | 现成能力，见 §3.d |
| 上屏 present | **Skia GPU** flush + swap（GL/Vulkan 后端） | 实现 `IRenderBackend::present` |

### 2.2 GPU 后端选型：GL（GLES3）为默认，Vulkan 为可选项

**推荐：GL（GLES3）**。理由：

1. **Skia GL 后端是最成熟、文档最全、坑最少的路径**（Chrome/Flutter 长期主力），Android NDK 直接内置 GLES，零渲染侧交叉编译。
2. **路线 C 的价值主张就是「不碰 GPU 细节、快速出图」**。EGL + ANativeWindow 的初始化远比 Vulkan surface/swapchain 简单；Vulkan 后端需要填 `GrVkBackendContext`、`GrVkExtensions`、自己管理 image layout/barrier，直接抵消了 Skia 的「省事」红利。
3. **若最终要上 Vulkan compute，Skia 反正会被整体换掉**（成为累赘，见 §10）。所以现在在 Skia 内部为 Vulkan 后端付出的复杂度是**沉没成本**，没有回报。

**可选：Skia Vulkan 后端**（`GrDirectContext::MakeVulkan(GrVkBackendContext)` + `GrBackendRenderTarget` 包 `VkImage`）。仅当团队要求「全线统一 Vulkan、避免 GLES 弃用、未来想在同一 VkDevice 上共存」时选。代价：要自管 instance/device/queue、`VK_KHR_swapchain` + `VK_KHR_android_surface`、图像布局与 barrier（Skia 要求 client 在导入 VkImage 前做同步）。这是「要 Vulkan 的复杂度，却拿不到 Vulkan compute 的性能」，与路线 C 定位相悖，故不默认。

> 结论：**本方案默认 Skia GLES3 后端**，在 §6 列出「若必须 Vulkan 后端」的要点，但不作为首选。

### 2.3 与三插拔接口的映射

```cpp
// 插拔① IPaintKernel —— 不变，仍由 libmypaint 实现（与路线 A/E 共用）
//     kernels/mypaint/mypaint_kernel.cpp 产出 StampData，不感知渲染后端

// 插拔② IRenderBackend —— 由 render/skia/sk_backend.cpp 实现
//     init → 建 offscreen SkSurface（持久画布）+ 窗口 SkSurface（present）
//     composite(stamps) → canvas->drawImage(每个 stamp 位图, blendMode)
//     present → flush + eglSwapBuffers

// 插拔③ IPlatform —— 不变，Android TextureView / PC GLFW
```

目录差异（相对规划 §2.2，只动 `render/`）：

```
render/
├── CMakeLists.txt
└── skia/                          # 替代 render/vulkan/
    ├── sk_backend.h/.cpp          # IRenderBackend 实现
    ├── sk_canvas.h/.cpp           # offscreen SkSurface 生命周期 + stamp 位图池
    └── sk_stamp.h/.cpp            # dab → SkBitmap（premultiply）栅格化
# shaders/ 目录删除（Skia 自带着色器，无需自写 GLSL）
# 顶层 CMake：DGCPAIN_USE_VULKAN 改 DGCPAIN_USE_SKIA；删除 glslc/SPIR-V 构建段
```

---

## 3. 核心模块设计

### 3.a libmypaint dab 生成（交叉编译方案简述）

与路线 A 完全相同，坑点见规划 §2.7，此处只列结论：

- **依赖**：libmypaint + json-c + glib（可 `MYPAINT_CONFIG_USE_GLIB=0` 裁剪）+ mypaint-brushes 预设。
- **三个必踩坑**：① `mypaint-config.h` 需生成或 vendor 进 `third_party/libmypaint/include/`；② `mypaint-brush.c` 无条件 include glib 头，glib 头文件必须保留在源码树；③ NDK 版本对 glib `_FILE_OFFSET_BITS` 等宏敏感，Krita 移植经验 NDK r18b 更稳。
- **产物**：`libmypaint.a` + `libjson-c.a`（arm64-v8a），预设目录打 APK assets。
- **产出数据**：`MyPaintSurface::draw_dab` 回调给出 `(x, y, radius, color_r/g/b[straight 0-1], opaque, hardness, alpha_eraser, aspect_ratio, angle, lock_alpha)`。

> 本路线**躲不开这个坑**——它是路线 A 的最大黑盒，也是路线 C 的「复杂度 4 分」里被拖后腿的那一项（若采用路线 E 白盒移植，这一整块被删除）。

### 3.b Skia 合成方案（核心）

**画布模型**：一张**持久 offscreen `SkSurface`**（RGBA8888 premultiplied，尺寸 = 画布分辨率），所有 stamp 累积画到它的 `SkCanvas` 上；每帧把 offscreen 的 `makeImageSnapshot()` 再 `drawImage` 到窗口 SkSurface 上屏（与规划中「Canvas storage image + present draw」同构）。

**合成调用**（每个 stamp 一次）：

```cpp
// offscreen 画布上叠一个已预乘的 stamp
SkPaint p;
p.setBlendMode(SkBlendMode::kSrcOver);   // 正常 / 或映射后的其它模式
p.setAlpha(255);                          // 预乘位图不再叠加额外不透明度
p.setColor(SK_ColorWHITE);                // 白 = 不调制位图颜色
canvas->drawImage(stampImage, sx, sy, SkSamplingOptions(), &p);
```

**dab → SkBitmap（RGBA 布局 + premultiplied 转换）**，两条路径：

**路径①（推荐，零手工预乘 bug）：alpha 蒙版 + SkPaint 染色**

生成一张「RGB=白、alpha=形状」的 stamp 位图（`kRGBA_8888` + `kUnpremul`），画的时候用 `p.setColor(brushColor)`（**straight/unpremultiplied**）让 Skia 在内部做预乘与 tint。这是 Skia 官方位图染色惯用法，**完全规避「SkPaint 吃 unpremultiplied、位图存 premultiplied」的转换陷阱**。

```cpp
SkBitmap mask; // RGB 全 255，alpha = shapeAlpha * opaque
SkPaint p;
p.setBlendMode(SkBlendMode::kSrcOver);
p.setColor(SkColorSetARGB(0xFF, r, g, b));  // straight，Skia 内部预乘
canvas->drawImage(mask.asImage(), x - rad, y - rad, SkSamplingOptions(), &p);
```

**路径②（预着色 stamp，贴合「drawImage + SkBlendMode」原始描述）**：在 CPU 端把 `color * shapeAlpha` 手工预乘进位图，再以白色 paint 原样贴出。

```cpp
// 每个 stamp 像素：straight alpha a = hardnessFalloff(d) * opaque
// premul RGBA = (color.r * a, color.g * a, color.b * a, a)，写入 uint8
SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                     kPremul_SkAlphaType);
// 用 SkPixmap 直写像素（绕过 SkBitmap::setPixel 的逐像素开销）
```

**关键结论（转换语义）**：Skia 的 `kN32/kRGBA_8888` 位图与 `SkSurface` 的存储是 **premultiplied**，但 `SkPaint::setColor` / `SkColor` 是 **unpremultiplied（straight）**。二者混用是 Skia 绘画最常见的 bug 源。**规则只有一条：颜色进 `SkPaint` 用 straight，进 `SkBitmap`/`SkPixmap` 用 premul**，二选一不要两头转。方案默认走路径①。

**性能注意（本路线真·软肋）**：Vulkan compute 路线里，CPU 只构造一个十几字节的 stamp 描述符（位置/半径/颜色），像素合成全在 GPU；**路线 C 里 CPU 要把每个 dab 栅格化成一张 `2r × 2r` 的位图**——256px 笔刷 = 65k 像素的 CPU 预乘/填充，大笔刷单 stamp 可能突破 §3.3 的 3ms。缓解：

1. **烘焙笔刷尖端纹理**（对齐画世界PRO `shapeTex`）：把 hardness 衰减曲线预烘焙成一张小灰度 stamp 纹理，CPU 只做 `drawImage(纹理, 缩放, tint)`，把像素填充交给 GPU（Skia 缩放采样）。这把「CPU 栅格化」降为「一次纹理缩放绘制」，是路线 C 性能达标的关键优化。
2. 小笔刷（<64px）可保留 CPU 直接栅格化（几十像素，纳秒级）。
3. stamp 位图/`SkImage` 从池复用，避免每 dab 一次 `malloc` + 纹理上传。

### 3.c 混合模式映射表（21 种 → SkBlendMode）

Skia 提供 29 种 `SkBlendMode`，其中 16 种是 Photoshop 风格混合模式（separable + non-separable），**直接覆盖需求清单 §3.3 的全部 15 种**。扩到「21 种」目标时，多出的 5 种 Skia 无原生枚举，需 `SkRuntimeEffect` 自定义（Skia 自带的 `SkBlender`/`SkRuntimeEffect` 机制可在 GPU 上跑任意 GLSL 混合公式）。

| # | 需求混合模式 | 英文 | SkBlendMode 枚举 | 原生 | 实现方式 |
|---|---|---|---|---|---|
| 1 | 正常 | Normal | `kSrcOver` | ✅ | premultiplied over |
| 2 | 变暗 | Darken | `kDarken` | ✅ | — |
| 3 | 正片叠底 | Multiply | `kMultiply` | ✅ | 注意：Skia 用 **unpremul** 计算 |
| 4 | 颜色加深 | Color Burn | `kColorBurn` | ✅ | — |
| 5 | 变亮 | Lighten | `kLighten` | ✅ | — |
| 6 | 滤色 | Screen | `kScreen` | ✅ | — |
| 7 | 颜色减淡 | Color Dodge | `kColorDodge` | ✅ | — |
| 8 | 叠加 | Overlay | `kOverlay` | ✅ | — |
| 9 | 柔光 | Soft Light | `kSoftLight` | ✅ | — |
| 10 | 强光 | Hard Light | `kHardLight` | ✅ | — |
| 11 | 色相 | Hue | `kHue` | ✅ | non-separable，W3C 语义 |
| 12 | 饱和度 | Saturation | `kSaturation` | ✅ | non-separable |
| 13 | 颜色 | Color | `kColor` | ✅ | non-separable |
| 14 | 发光度 | Luminosity | `kLuminosity` | ✅ | non-separable |
| 15 | 差值 | Difference | `kDifference` | ✅ | — |
| 16 | 排除 | Exclusion | `kExclusion` | ✅ | 需求清单外补充 |
| 17 | 线性加深 | Linear Burn | 无 | ❌ | `SkRuntimeEffect`：`1 - (1-src)/dst` |
| 18 | 线性减淡（相加） | Linear Dodge | `kPlus`（近似） | ⚠️ | `kPlus`=premul src+dst，与 straight 版略有偏差；精确用 `SkRuntimeEffect` |
| 19 | 亮光 | Vivid Light | 无 | ❌ | `SkRuntimeEffect`：Color Burn/Dodge 按 src 亮度切换 |
| 20 | 线性光 | Linear Light | 无 | ❌ | `SkRuntimeEffect`：Linear Burn/Dodge 组合 |
| 21 | 点光 | Pin Light | 无 | ❌ | `SkRuntimeEffect`：Darken/Lighten 按 src 亮度切换 |

> 图层透明度（需求 #75）不是混合模式，用 `SkPaint::setAlphaf` 或 `saveLayer` 的 paint alpha 实现。
> 附注：需求清单明确列出的 15 种（#1–15）**全部被 Skia 原生覆盖，零自写 shader**——这是路线 C 混合模式维度的最大红利；只有扩到 21 种时才需要 5 个 `SkRuntimeEffect`。

### 3.d 图层 / 蒙版用 Skia 能力

| 需求 | Skia 能力 | 说明 |
|---|---|---|
| 图层独立 | 每图层一张 offscreen `SkSurface` | 与画布同构，图 = 独立 surface |
| 图层合成（含非 normal 混合） | `SkCanvas::saveLayer(bounds, &paint_with_blendMode)` + `restore` | `saveLayer` 建临时离屏缓冲，restore 时按 paint 的 blendMode 合成到下层 |
| 图层透明度 | `saveLayer` 的 `SkPaint::setAlphaf` | — |
| 阿尔法锁定（Alpha Lock） | 合成时 `p.setBlendMode(kSrcIn)` | 只在已画像素（dst alpha>0）写入 |
| 图层蒙版 / 剪辑蒙版 | `SkCanvas::clipShader(maskShader)` / `clipRect` / `clipPath` | `clipShader` 用灰度 shader 做蒙版裁剪，最贴合黑白灰蒙版 |
| 选区裁剪 / 羽化 | `clipRect`/`clipPath` + `SkMaskFilter::MakeBlur` | 羽化用模糊 mask filter |
| 高斯模糊滤镜 | `SkImageFilter::MakeBlur`（`SkBlurImageFilter`） | 需求 #98 |
| 纸纹纹理 | `SkShader::MakeImageShader` | 需求 #42，tile 采样 |

**与 Vulkan compute 路线的差异**：这些能力在 Skia 里是**现成 API**（一行为一个滤镜/蒙版），而在 E/B 路线里需要**逐一手写 compute/fragment shader**。这正是路线 C 的「功能广度」红利——若目标是「画板 + 大量 2D 特效/矢量/文字」，Skia 省下的工作量巨大。

### 3.e 输入层（Jetpack Ink）

与规划 §4.6 完全一致，**零差异**：

- `androidx.ink:ink-strokes` 纯数据层取 `StrokeInputBatch` 点流（位置/时间戳/压力/倾斜/方向）。
- 不引入 `ink-rendering`（Skia 渲染替代它）。
- 预测点走自研速度外推（~30 行）或 `MotionEvent` 历史，标 `isPredicted`，真实点到达重合成覆盖。

---

## 4. 数据流（触控 → 上屏，含耗时预算）

```
触控笔按下 (MotionEvent)
  → Jetpack Ink 建模 StrokeInputBatch（压力/倾斜/时间戳）          [~1ms]
  → 预测点流 push 到 input ring_buffer（isPredicted 标记）
  → Brush Thread 取点 → libmypaint stroke_to(x,y,p,tilt)          [<3ms, §3.3 P5]
  → MyPaintSurface::draw_dab 回调 → StampData
  → dab → stamp 位图（CPU 栅格化 + premultiply 或 烘焙纹理缩放）    [~1-2ms, ★新成本]
  → stamp 位图 push 到 stamp ring_buffer
  → Render Thread：canvas->drawImage(stamp, blendMode)             [<2ms, §3.3 P3]
     （Skia 内部：位图 → GPU 纹理上传 + 纹理四边形绘制，批量入 GrOpsTask）
  → SkSurface::flush() + drawImage(offscreen 快照 → 窗口 surface)
  → eglSwapBuffers / present → 屏幕                                [VSync 对齐]
─────────────────────────────────────────────────────────────────
端到端合计目标 < 30ms（§3.3 P1）
```

**Skia 抽象层的潜在 overhead（本路线被扣分的技术根源）**：

1. **draw call 数量 vs 单次 dispatch**：Vulkan compute 路线把 N 个 stamp 折成**一次** `vkCmdDispatch`；Skia 里 N 个 stamp = **N 次 `drawImage` = N 个纹理四边形 draw**（Skia 会按 shader/state 尽力合批，但合批上限和状态切换开销仍在）。这是「合成 <2ms」能否达标的最大变量。
2. **隐式 flush 边界**：Skia 延迟 GPU 提交到 `flush()`，好处是能合批，坏处是「何时 flush」不在我们掌控、GC/纹理缓存淘汰可能引入不可预测的毛刺帧。
3. **非 srcOver 混合的 saveLayer 开销**：`saveLayer` 每次都是离屏 RT 分配 + resolve，多图层/多混合模式下开销远高于手写 shader 的逐像素混合。
4. **stamp 上传路径**：每 dab 一张小位图 → `SkImage` → GPU 纹理缓存。Skia 有纹理缓存去重，但「每 stamp 一张新图」模式天然不如「一张 stamp 纹理 + push_constant 变色」省（后者是 E/B 路线的做法）。
5. **CPU 栅格化成本**（§3.b 已述）：大笔刷预乘填充是 Vulkan 路线没有的额外 CPU 负担。

> 这些 overhead 不是「Skia 慢」（Skia GPU 光栅化本身很快），而是「**Skia 是 draw-call/纹理模型，不是 compute/blit 模型**」——与 dab stamp 的批量像素合成天然不匹配，适配成本无法归零。

---

## 5. 关键接口设计

### 5.1 SkSurface 生命周期

```cpp
class SkBackend : public IRenderBackend {
    sk_sp<GrDirectContext> ctx_;        // MakeGL()（或 MakeVulkan()）
    sk_sp<SkSurface> canvasSurf_;       // 持久画布（offscreen，premul RGBA8888）
    sk_sp<SkSurface> windowSurf_;       // 上屏 surface（EGL 窗口 / GrBackendRenderTarget 包 fb0）
    std::vector<sk_sp<SkImage>> stampPool_;  // stamp 位图池，复用

    void init(PlatformSurface s, int w, int h) override {
        ctx_ = GrDirectContext::MakeGL();   // 若 Vulkan：MakeVulkan(GrVkBackendContext)
        canvasSurf_ = SkSurface::MakeRenderTarget(ctx_.get(), SkBudgeted::kNo,
                          SkImageInfo::MakeN32Premul(w, h));
        // windowSurf_ = SkSurface::MakeFromBackendRenderTarget(ctx_, ...)  // 包窗口 fb
    }
    void resize(int w, int h) override {
        // 保留内容：旧 canvasSurf_ 快照 → 新 surface drawImage 回填
        auto snap = canvasSurf_->makeImageSnapshot();
        canvasSurf_ = SkSurface::MakeRenderTarget(... MakeN32Premul(w, h));
        canvasSurf_->getCanvas()->drawImage(snap, 0, 0);
        // 重建 windowSurf_（EGL swapchain 尺寸变化）
    }
    void composite(const std::vector<StampData>& stamps) override {
        SkCanvas* c = canvasSurf_->getCanvas();
        for (auto& s : stamps) {
            sk_sp<SkImage> img = stampFromPool(s);      // dab → SkImage（§3.b）
            SkPaint p; p.setBlendMode(map(s.blendMode)); // §3.c 映射
            c->drawImage(img, s.x - s.radius, s.y - s.radius,
                         SkSamplingOptions(), &p);
        }
    }
    void present() override {
        canvasSurf_->flush();                           // 提交画布 GPU 工作
        windowSurf_->getCanvas()->drawImage(
            canvasSurf_->makeImageSnapshot(), 0, 0);    // 画布 → 窗口
        ctx_->flushAndSubmit();                         // 提交全部
        eglSwapBuffers(...);                            // 或 Vulkan present
    }
};
```

### 5.2 stamp 上传

`stampFromPool`：从池里取 `SkBitmap`/`SkSurface`，按 dab 参数填充（路径①蒙版 或 路径②预乘位图），`SkImage::MakeFromBitmap`（或 `SkSurface::makeImageSnapshot`）。**关键优化**：烘焙笔刷尖端纹理后，同一笔刷的 stamp 共享一张 `SkImage`，每 dab 只传「位置 + 缩放 + 颜色」，彻底去掉「每 stamp 一次上传」。

### 5.3 接口映射表

| 规划接口 | 本路线实现 | 备注 |
|---|---|---|
| `IPaintKernel::strokeTo → StampData` | 不变（libmypaint） | `StampData` 增加 `blendMode` 字段（`core/types.h` 补一个枚举） |
| `IRenderBackend::init/resize` | 建/重建 `canvasSurf_` + `windowSurf_` | resize 用快照回填保内容 |
| `IRenderBackend::composite` | `drawImage` 批量 + `SkBlendMode` | 非 srcOver 用 `saveLayer` |
| `IRenderBackend::present` | `flush` + drawImage 快照 + swap | — |
| `IRenderBackend::clearCanvas` | `canvasSurf_->getCanvas()->clear(SK_ColorTRANSPARENT)` | — |
| `IPlatform` | 不变（TextureView / GLFW） | EGL 上下文由 `IPlatform` 提供句柄 |

---

## 6. 关键技术难点与解决方案

| 难点 | 影响 | 解决方案 |
|---|---|---|
| **① stamp 合成模型不匹配**（Skia「路径+paint」vs「dab stamp」） | 高 | 用 `drawImage(stamp位图) + SkBlendMode` 适配；烘焙尖端纹理，把「每 dab 栅格化」降为「纹理缩放 drawImage」 |
| **② premultiplied 转换陷阱**（SkPaint straight vs 位图 premul） | 中 | 铁律：颜色进 `SkPaint` 用 straight，进 `SkBitmap/SkPixmap` 用 premul；默认走「alpha 蒙版 + paint 染色」路径①，让 Skia 内部预乘 |
| **③ Skia 抽象层 overhead 不可控**（draw call 数、隐式 flush、saveLayer） | 高 | 测量三件事：`composite` 打点、AGI 抓 Skia GL 命令、大笔刷（256px）CPU 栅格化打点；不达标则烘焙纹理 + 减少 saveLayer + 显式 `flush` 策略 |
| **④ 与 Compose/TextureView 集成** | 中 | TextureView 多一次 GPU 合成拷贝（同规划 §4.3）；延迟超标则评估 SurfaceView 独立 surface；EGL 上下文在 `onSurfaceTextureAvailable` 建、`onSurfaceTextureDestroyed` 毁 |
| **⑤ libmypaint 交叉编译坑（glib/json-c/config.h）** | 高 | 走 mypaint_ffi 已验证路径（规划 §2.7）；或——**更优——直接换路线 E 白盒移植彻底删除此坑** |
| **⑥ 终局返工**（若日后转 Vulkan compute，Skia 合成层整体报废） | 中 | 接口隔离使返工**局限在 `render/skia/` 一个目录**，engine/core/kernel 不动；返工成本可控，但确实白写一套合成层 |

**针对 ③ 的测量方法（AI 执行可验证）**：用 Android GPU Inspector 抓 Skia GL 后端的 draw call 数与合成耗时；用 CPU 打点区分「libmypaint stroke_to / dab 栅格化 / drawImage / flush」四段各自耗时，找出是否因 CPU 栅格化（②）还是 draw call（①）超标，对症下药。

---

## 7. 四维度评审（5 分制 · 客观打分）

与《笔刷渲染技术路线评审》v2.0 评分一致（加权 3.65）：

| 维度（权重） | 得分 | 一句话理由 |
|---|---|---|
| **复杂度**（25%） | 4 | Skia 零渲染侧交叉编译、文档业界顶级、AI 友好；但 libmypaint 的 glib/json-c 交叉编译黑洞仍在，拖低了本该更高的分 |
| **性能**（30%） | 4 | Skia GPU 光栅化达 60fps 没问题，但 draw-call 模型 + CPU 栅格化 + 隐式 flush 让「合成 <2ms」达标有压力，且到不了 Vulkan compute 天花板 |
| **可控性**（25%） | 3 | 像素合成交给 Skia 黑盒，dab 又是 libmypaint 黑盒不可 GPU 化；改底层/加类型受限于两套 API，是全路线里可控性最低的一档 |
| **时间**（20%） | 3.5 | 「能画」出得最快（Skia 现成），但被交叉编译 + premultiply 适配 + 日后转 Vulkan 的返工抵消 |
| **加权总分** | **3.65** | 排在 E（4.18）、B（4.03）之后，A（3.33）、D（3.55）之上 |

---

## 8. 风险清单

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 合成 <2ms 不达标（draw call 过多） | 中 | 高 | 烘焙尖端纹理 + 合批 + 显式 flush；仍不达标则此路线证伪 |
| 大笔刷 CPU 栅格化突破 3ms | 中 | 中 | 烘焙纹理走 GPU 缩放；小笔刷才 CPU 直栅格 |
| premultiply 颜色错乱（画面发灰/描边黑边） | 中 | 中 | 路径①蒙版染色规避；单测对比 libmypaint host 输出 |
| libmypaint 交叉编译失败 | 中 | 高 | mypaint_ffi 路径；备选直接切路线 E（删此风险） |
| TextureView 额外拷贝致延迟超标 | 中 | 中 | 评估 SurfaceView |
| 5 个非原生混合模式（#17–21）需自写 SkRuntimeEffect | 中 | 低 | 仅高优才做；先交付 16 种原生 |
| Skia GL 后端在低端 Mali GPU 的合成性能不足 | 中 | 中 | baseline 先锁 Adreno；GLES3 最低要求 |
| 终局转 Vulkan compute 时 Skia 层返工 | 确定（若转） | 中 | 接口隔离，返工局限 `render/skia/` |

---

## 9. 分阶段实施计划（AI 执行视角）

> 复用规划 §5 的阶段骨架，仅替换渲染侧任务。

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **C0 · Skia spike** | host 上建 offscreen SkSurface + 固定 stamp `drawImage` + 全 16 种 SkBlendMode 合成出笔迹 | host 可执行程序渲染出多混合模式笔迹截图；确认 premultiply 路径①/②选型 |
| **C1 · 接口层**（同规划阶段 1） | 三接口 + CMake 多 toolchain；`render/skia/` 空壳 | 三 preset 通过；PC/Android 编出空壳 |
| **C2 · Skia 渲染后端** | `sk_backend`/`sk_canvas`/`sk_stamp`；offscreen + 窗口双 surface；stamp 位图池；GLES3 后端 | host offscreen 用固定 stamp 合成出笔刷痕迹（不依赖内核） |
| **C3 · libmypaint 内核**（同规划阶段 3） | 交叉编译 + `mypaint_surface → StampData` + `mypaint_kernel` | JNI 调 libmypaint 生成 stamp → 送入 Skia `drawImage` 可画 |
| **C4 · 平台层 + UI** | TextureView + EGL + Compose UI；PC GLFW + ImGui | 双平台看到 Skia 画布，UI 切换笔刷/颜色 |
| **C5 · 输入集成**（同规划阶段 5） | Jetpack Ink 点流 + 预测覆盖 | 双平台笔迹跟随良好 |
| **C6 · 全链路 + 性能测试** | AGI + 高速摄影测 §3.3 全部指标；重点抓 draw call 数 + CPU 栅格化耗时 | 满足 §3.3 或输出「哪些指标因 Skia 模型不达标」的证伪报告 |

**里程碑**：M1（C4 末）双平台 Skia 画布上屏；M2（C3 末）内核经 Skia 合成可画；M3（C6 末）性能报告 + 「是否继续用 Skia / 转 Vulkan」的决策结论。

---

## 10. 结论

**这条路线适合什么情况**：

- **团队明确「不碰 GPU/着色器细节」**：Skia 是零着色器、零 compute 的纯 API 路径，AI 写 `drawImage + setBlendMode` 远简单于写 `brush_composite.comp`。
- **目标是「快速功能验证 / 画板型应用」而非「Procreate 级手感」**：Skia 的混合模式（16 种原生）、`saveLayer`、蒙版、模糊滤镜、文字、矢量**现成**，省下 E/B 路线里「逐一手写 shader」的海量特效工作。
- **时间极紧、先用 Skia 跑通闭环**：从零到「能画 + 多混合 + 多滤镜」Skia 出图最快，是「先验证产品形态，性能后补」的最优选择。

**为何当前排在 E/B 之后（客观）**：

1. **模型不匹配是硬伤**：Skia 是「路径 + paint / draw-call / 纹理」模型，dab stamp 合成是「批量像素 blit/compute」模型，`drawImage` 适配带来 draw call 数、隐式 flush、saveLayer 三笔不可消除的 overhead，直接威胁「合成 <2ms」。
2. **终局天花板冲突**：性能终局是 Vulkan compute，而 Skia 合成层在那一天会整体报废（成为「累赘」），现在投入的 GLES/Vulkan 后端复杂度是沉没成本。
3. **libmypaint 交叉编译坑仍在**：Skia 侧的省事，被 libmypaint 的 glib/json-c 黑洞对冲，这条路线的「复杂度红利」是单边的。
4. **可控性最低**：像素行为与 dab 算法都是黑盒，无法像 E（自研）那样平滑 GPU 化。

**何时反而值得选（一句话）**：当「不碰 GPU 细节」或「快速验证 + 需要完整 2D 特效面」的诉求压过「极致笔刷手感/延迟天花板」时——Skia 用「性能上的 0.5 分让步」换「复杂度 + 功能广度的巨大红利」，是**原型机与功能验证期**的务实选择；但**一旦手感对标 Procreate 成为硬指标，应切换到路线 E（白盒移植 + Vulkan）**，Skia 路线只作为「先跑通、后替换」的临时垫脚石。

---

> **文档版本**：v1.0
> **编制依据**：DGCPaint_技术规划.md、笔刷渲染技术路线评审.md v2.0、绘画内核功能清单.md v1.0
> **Skia 参考**：SkBlendMode 枚举与语义（docs.skia.org / SkBlendMode.h）、Skia Vulkan backend（GrDirectContext::MakeVulkan / GrVkBackendContext / GrBackendRenderTarget）、SkCanvas::drawImage / saveLayer / clipShader 能力
> **输出目录**：docs/调研/
