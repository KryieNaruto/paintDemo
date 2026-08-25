# UI 画布接入 + 性能验证 · 设计（2026-08-24）

> 状态：设计草案（待审阅）
> 触发：build-pipeline / 需求「UI 接入，无需考虑 SDK 内部，只需用即可；UI 双线同步（PC/Android）；一个画布即可，验证 shader dab 与上屏等渲染性能是否达标，对标 Procreate」
> 范围：**消费者仓库**（`paint-pc` / `paint-android`）接入 SDK C API + 读回上屏 + FPS 浮层。不改 SDK 内部。

---

## 1. 背景与事实核查

| 事实 | 现状 | 影响 |
|---|---|---|
| SDK 上屏路径 | **无窗口化 present**（`VkBackend::present()` no-op，`vk_backend.h:16` 明确本期不做 swapchain） | UI 只能用**读回 + 贴图**上屏 |
| 可用渲染链路 | `dgcSetOffscreenSurface(w,h)` → 输入 → `dgcRender` → `dgcReadbackPixels`/`dgcExportPNG` | 上屏 = `dgcReadbackPixels` 后贴图 |
| 真实笔刷内核 | **B3-1 未合并**；Null 内核 `strokeTo` 返回空（无可见 dab） | 「shader dab 性能」须等 B3-1 |
| 消费者仓库 | `paint-pc` / `paint-android` 已存在，均为**空壳**（输入回调 TODO 桩 / 仅 nativeHello） | 在现有壳上做接入，不新建 |
| SDK submodule 钉 | 两仓都钉 `43500e56`（早于 B1-7 确定性） | 接入前需把 submodule 前移到含 B1-7/B1-8 的版本 |
| 确定性 | `dgcSetRandomSeed(seed)` / `dgcSetFixedTime(t)` 已接通 | 可用固定 seed 复现笔迹，便于对比 |

**用户已拍板**：
1. **方案 A**：先通链路（空画布 + 输入 + 读回上屏 + FPS 浮层），性能达标项标注依赖 B3-1。
2. **上屏方式**：读回 → 贴图（PC GL 纹理 / Android ImageBitmap）。

---

## 2. 目标与验收

### 目标（本期，方案 A）
双平台各自打通「窗口/画布 → 输入 → SDK C API → 读回 → 贴图上屏」最小闭环，并实时显示 FPS / 帧时 / 读回耗时，作为渲染性能的可视化验证界面。UI 只 `#include "dgc_paint_c_api.h"`，只链接 `dgc_paint`。

### 验收标准
1. **PC（paint-pc）**：`dgcCreate` → `dgcSetOffscreenSurface` → 输入（鼠标拖拽）经 `dgcBeginStroke/StrokeTo/EndStroke` → 每帧 `dgcReadbackPixels` → OpenGL 贴图到画布面板 → 窗口显示白底画布 + 输入轨迹坐标 + FPS 浮层。
2. **Android（paint-android）**：JNI 把 `MotionEvent`（坐标/压感/tilt）转 C API → Compose `ImageBitmap` 每帧从 JNI readback 更新 → 显示画布 + FPS 浮层。
3. **FPS 浮层**：显示「FPS / 帧时 ms / 读回耗时 ms」三项，随笔画实时刷新（§3.3 可测项）。
4. **不越界**：不 include `core/`，不改 SDK 任何文件。
5. **性能达标项（UI 可测）**：持续笔画时**稳定 60fps（120Hz 屏测 120fps）**（§3.3）。compute/stamp 内部耗时不在 UI 层测。
6. **依赖声明**：计划中把「shader dab 性能」验收标记为「依赖 B3-1（真实内核）」，B3-1 合并后自然达标，非本期验收项。

### 非目标（本期）
- 不做 SDK swapchain/present（SDK 内部任务）。
- 不测 SDK 内部 compute/stamp 耗时（SDK 侧另测）。
- 不做笔刷面板/颜色面板/撤销 UI（本期只一个画布）。

---

## 3. 架构

```
┌───────────────────────────┐   ┌───────────────────────────┐
│ paint-pc (GLFW+ImGui+GL)  │   │ paint-android (Compose)   │
│ 输入回调 → C API           │   │ MotionEvent → JNI → C API │
│ glTexImage2D 贴图 → 画布    │   │ ImageBitmap ← readback    │
└──────────┬────────────────┘   └──────────┬────────────────┘
           │ 只 #include "dgc_paint_c_api.h"，只链接 dgc_paint
           ▼                                ▼
┌──────────────────────────────────────────────────────────┐
│  dgc_paint SDK（submodule sdk/）                          │
│  dgcSetOffscreenSurface / Begin|StrokeTo|EndStroke /      │
│  dgcRender / dgcReadbackPixels / dgcExportPNG            │
└──────────────────────────────────────────────────────────┘
```

- **单一数据流**：`输入事件 → C API → SDK 离屏渲染 → dgcReadbackPixels 返回 RGBA8 → UI 上传纹理/位图 → 画布显示`。
- **每帧流程**：UI 渲染循环每帧（或每收到输入后）`dgcReadbackPixels` 读回画布 → 更新纹理/位图 → 绘制到画布区域 → 叠加 FPS 浮层。
- **线程**：PC 主线程即 UI+渲染循环（同步读回，最简单）；Android 读回放 Render Thread，JNI 调用同线程，避免跨线程。

### 平台差异
| 项 | PC | Android |
|---|---|---|
| 窗口/画布 | GLFW 窗口 + ImGui 面板 | Compose `Canvas` + `ImageBitmap` |
| 输入 | GLFW 鼠标/笔回调 | `MotionEvent` → JNI |
| 贴图 | `glTexImage2D` + `glDrawArrays` 到 GL 纹理 | `Bitmap.createBitmap` + `drawBitmap` |
| 读回频率 | 每帧（vsync 驱动） | 每帧（Choreographer / `withFrameNanos`） |
| C API 调用线程 | UI 主线程 | JNI 调用的 Render Thread |

---

## 4. 关键决策与理由

| 决策 | 选择 | 理由 |
|---|---|---|
| 上屏通路 | 读回 + 贴图 | SDK 无 present；不改 SDK 即可用；读回耗时单独打点可量化 |
| 接入推进 | 方案 A（先通链路后验性能） | 双线不空等；B3-1 后笔迹自然显现 |
| 画布尺寸 | 与窗口/视图等比（如 1280×800 / 屏等宽高） | 读回 1:1，避免缩放采样差异 |
| 性能口径 | UI 可测项（FPS/帧时/读回耗时） | §3.3 中 compute/stamp 归 SDK 侧 |
| 是否改 SDK | 否 | 需求「只需用即可」 |
| submodule 指针 | 前移到含 B1-7/B1-8 的 commit | 需确定性；新 C API 签名 |

---

## 5. 文件改动清单

### paint-pc（仓库 `/home/qiansenwei/workspace/paint-pc`）
| 文件 | 动作 | 说明 |
|---|---|---|
| `src/main.cpp` | 修改 | `#include "dgc_paint_c_api.h"`；`dgcCreate`，退出 `dgcDestroy` |
| `src/app.cpp` | 修改 | 输入回调接 C API；每帧 readback → GL 纹理；FPS 浮层 |
| `src/app.h` | 修改 | 持 `DgcContext*` + GL 纹理/程序句柄 |
| `src/gl_canvas.h/.cpp` | 新增 | 最小 GL 纹理贴图（quad + shader + 纹理上传） |
| `CMakeLists.txt` | 修改 | 链接 `dgc_paint`（已有）；加 `gl_canvas` 源 |
| `sdk` submodule | 更新 | 前移到含 B1-7/B1-8 的 commit |
| `README.md` | 修改 | 补运行/构建说明 |

### paint-android（仓库 `/home/qiansenwei/workspace/paint-android`）
| 文件 | 动作 | 说明 |
|---|---|---|
| `jni/paint_android_jni.cpp` | 修改 | 加 `nativeInit/nativeSetCanvas/nativeStrokeBegin|To|End/nativeReadback/nativeExportPNG/nativeDestroy`；持 `DgcContext*` |
| `app/src/main/java/com/dgcamp/paint/jni/PaintNative.kt` | 新增 | `external fun` 声明 |
| `app/src/main/java/com/dgcamp/paint/MainActivity.kt` | 修改 | 画布 View 接 MotionEvent → PaintNative；每帧 readback → ImageBitmap → Canvas |
| `app/src/main/java/com/dgcamp/paint/CanvasView.kt` | 新增 | Compose/自定义 View 画布 + FPS 浮层 |
| `app/src/main/AndroidManifest.xml` | 修改 | （如需要）横屏已锁 |
| `app/build.gradle.kts` | 修改 | 若需 ndk ABI / jniLibs 配置 |
| `sdk` submodule | 更新 | 同 PC |
| `README.md` | 修改 | 补运行说明 |

---

## 6. 错误处理与边界
- `dgcCreate` 失败 → 弹错误并退出（PC）/ toast（Android）。
- `dgcReadbackPixels` 返回非 0 → 记 `dgcGetLastError`，跳过本帧贴图。
- 窗口 resize → `dgcSetOffscreenSurface` 重设（或 `dgcResize`），重建纹理/位图。
- 输入超出画布 → 丢弃该点（避免负坐标进 SDK）。
- 无显示环境（PC headless）→ 沿用现有优雅退出。

## 7. 风险与依赖
| 风险/依赖 | 等级 | 说明/缓解 |
|---|---|---|
| B3-1 未合并，无可见笔迹 | 高 | 本期验收只到「链路通 + FPS 达标」，性能项标注依赖 B3-1 |
| submodule 前移可能改变 C API 签名 | 中 | 前移后按新头重编译；以 `dgc_paint_c_api.h` 为准 |
| Android readback 每帧全量拷贝带宽 | 中 | 4K 画布约 33MB/帧；本期用 ≤1080p 画布验证，性能浮层可量化 |
| GL 纹理 Y 翻转（坐标系） | 低 | 贴图时翻转 UV；已在 gl_canvas 处理 |
| 两平台并行 worktree | 低 | 各仓库独立，互不阻塞 |

## 8. 测试策略
- **PC**：`cmake --build` 成功；有显示环境下启动窗口，白底画布可见，拖拽有轨迹坐标更新 + FPS 浮层刷新；无显示环境 `--headless` 优雅退出。`dgcExportPNG` 导出一张 PNG 验证离屏链路。
- **Android**：`./gradlew assembleDebug` 成功；连接设备/模拟器运行，画布 + FPS 浮层显示，拖拽输入不崩。
- **门禁**：build-pipeline 审阅 ≥80 + 测试 100 分（0 失败 0 跳过）。受限于无 Android 真机时，Android 侧以「编译通过 + 代码审阅」为测试口径，真机验证标注为人工后续项。

## 9. 依赖任务映射
| 依赖 | 状态 |
|---|---|
| B1-4 C API 封装 engine | 已通过 |
| B1-6 完整 C API（离屏/readback/确定性） | 已通过 |
| B1-7 确定性 | 已通过 |
| B1-8 SDK 所有权重构 | 已通过 |
| B2-1 Vulkan 离屏后端 | 已通过 |
| **B3-1 真实内核（shader dab 性能验收前提）** | **可申领/待审核** |

> 本期不阻塞 B3-1：链路先通，性能达标项在 B3-1 合并后自然满足。
