# 设计 · SDK swapchain 直接上屏（Android 先行，消灭 readback 延迟）

> 状态：已过 brainstorming 用户审阅（2026-09-08），待写实施计划。
> 触发：A8-3/A8-4 两轮真机证实「离屏 render→GPU readback→CPU Bitmap→上屏」架构有 ~10ms
> 量级不可调和的延迟地板——A8-3 降低消费端读回节流无效（上屏恒 16ms），A8-4 换 SurfaceView
> 直绘无效（12ms ≈ SDK 11.8ms），均离 Ink（~5ms）有代差；唯一能真正逼近 Ink 的方向是
> 让 SDK 把 GPU 结果**直接 present 到屏幕**，不读回、不 CPU 搬运。

## 1. 背景与已定量根因（真机，非猜测）

MDP1221（MediaTek MT6789/Mali-G57）同一会话内三方对比（2026-09-08，A8-4 实测）：

| 模式 | 输入→读回 | 输入→上屏 | 说明 |
|---|---|---|---|
| SDK（离屏 readback + Compose 贴图） | ~9ms | ~11.8ms | 当前 main 路径 |
| SurfaceView 直绘（A8-4） | ~12ms | ~12ms | lockCanvas 直绘，绕开 Compose 无效 |
| Ink（对照） | n/a | ~5ms | HWUI 前缓冲直绘，无 readback |

结论链：
- 换消费端上屏方式（Compose → SurfaceView）对上屏延迟**无影响**（11.8 vs 12ms）→ Compose
  不是主因，A8-4 假设证伪。
- 两模式都背着 ~9-12ms 的 **readback 固有代价**（GPU 画完 → 读回 3.1MB 像素到 CPU → 再贴），
  这条 CPU 回读链路是离屏架构的下限，任何消费端上屏方式绕不掉。
- Ink 之所以 5ms，是因为它**没有 readback**：GPU（或 HWUI）直接画到前缓冲。

**推论**：要逼近 Ink，必须消灭 readback 链路 —— 让 SDK 的 composite 结果直接
blit 到屏幕 swapchain 上屏（全程 GPU 内部，不落 CPU）。

## 2. 目标与验收标准

**目标**：SDK 渲染路径（Mode A）交互态显示从「离屏 → readback → CPU → 上屏」改为
「离屏 composite → blit → swapchain present」，消灭 readback 延迟，让 Mode A 逼近 Ink。

**验收（真机 MDP1221，同机三方对比，硬门槛不豁免）**：
- 新增 `SWAPCHAIN` 渲染模式，`SWAPCHAIN vs SDK(离屏) vs Ink` 三方同机 A/B/C；
- **SWAPCHAIN 输入→上屏 lag ≤ ~7ms 且明显低于 SDK 当前 ~12ms**（逼近 Ink ~5ms）；
- 预测开/关各测一轮（覆盖 wet-tip 在 swapchain 上的显示，不允许预测开时毛边/尖慢半拍复现）；
- fps 稳定不掉帧（沿用 P7 门槛：连续绘制不掉破 60）、笔迹实时刷新无滞后；
- 离屏路径零回归：导出 PNG 与离屏逐位一致、host ctest（determinism 等）全绿；
- 无头验证仍走离屏（build-pipeline CLI/离屏硬约束由既有离屏路径满足，swapchain 为显示并存路径）。

## 3. 关键现状（已核实，swapchain 骨架半就位）

- C API `dgcSetSurface(DgcContext*, void* nativeWindow, int w, int h)` **已存在**
  （`sdk_api/dgc_paint_c_api.cpp:163`），内部调 `backend->init(nativeWindow, w, h)`；
  Null 后端接受 `nativeWindow == NULL`（headless）。消费端传 `ANativeWindow*` 即达 SDK，
  **无需新增 C API 入口**。
- `IRenderBackend::present()` **已存在**，engine 渲染循环每轮 composite 后调
  `backend_->present()`（`core/engine.cpp:444`）；`VkBackend::present()` 现为 no-op
  （`render/vulkan/vk_backend.cpp:1389`，注释明确「离屏模式 no-op，§4.0.5」）。
- `vk_backend.h` 注释明确当前设计「离屏 Canvas storage image（常驻 GENERAL），**不创建
  swapchain**，present() no-op」——本任务即把这个预设翻转。
- VkBackend 现有离屏权威图：`canvasImage`（真实墨迹）、`tipImage`（A8-2 预测瞬态）、
  `displayImage`（`canvas+tip` 经 merge.comp 合成的读回源，仅 `tipHasContent_` 为真时被写）。
- 显示源选择现有逻辑：`tipHasContent_` ? displayImage : canvasImage
  （`RefreshReadbackCacheLocked` 内，`vk_backend.cpp:1246-1269`）。

## 4. 架构总览

离屏 `canvasImage` 仍是**唯一权威**（composite 照旧画进它，determinism / dgcExportPNG /
离屏对照全部不变）。新增可选 onscreen 路径只负责「把 GPU 已有的结果显示出去」：

```
触笔 → engine 攒批 → composite 画进 canvasImage (+tip 画进 tipImage)
     → present()（已绑定 onscreen 时）：
          acquire swapchain image
          → 源选择（tipHasContent_ ? displayImage(先 merge canvas+tip) : canvasImage）
          → blit 源 → swapchain image
          → queuePresent
     （全程 GPU 内部，无 readback、无 CPU 像素搬运）
```

## 5. SDK 侧改动（demo 仓库，`render/vulkan/` + `sdk_api/`）

### 5.1 可选 swapchain 成员与扩展
- `VkBackend` 增加可选 `VkSurfaceKHR` + `VkSwapchainKHR` 句柄（仅 `init` 收到非空
  `nativeWindow` 时创建；仍离屏时保持不创建、present no-op，现有离屏路径零行为变化）。
- 依赖扩展：`VK_KHR_surface` + `VK_KHR_android_surface`（Android 侧）+ `VK_KHR_swapchain`
  （若实例/设备创建尚未 enable，按需补；Android NDK Vulkan 头自带这些扩展声明）。
- surface 由 `nativeWindow`（`ANativeWindow*`）经 Android 平台调用创建。
- 所有权沿用现有 `VkDeviceHandle`/`Vk*Handle` RAII 模板（`vkDestroySwapchainKHR`/
  `vkDestroySurfaceKHR` 包装），全 SDK 零裸 new/delete。Pimpl 边界不变（`DgcContext`
  仍不透明句柄，`dgcSetSurface` 已在外层）。

### 5.2 present 实现
- `present()` 当 swapchain 已绑定时：`vkAcquireNextImageKHR` → 按 §3 源选择准备 blit 源
  （有 tip 时先执行一次 `canvas+tip → displayImage` 的 merge dispatch）→ `vkCmdBlitImage`
  （源 → swapchain image）→ `vkQueuePresentKHR`。离屏（未绑定）仍 no-op。
- 有 tip 的 merge 逻辑复用现读回路径的 merge 管线/barrier 约定（`vk_backend.cpp:1246-1269`
  同一套），避免另写一份。
- **present mode**：优先 `VK_PRESENT_MODE_MAILBOX_KHR`（低延迟、不撕裂），设备不支持则退
  `FIFO`。真机逐档验证（MAILBOX 能显著降低「等下一个 vsync」的排队时间，是逼近 Ink 的关键
  之一；若真机 MAILBOX 反而掉帧/不稳，如实记录退回 FIFO，不强行）。

### 5.3 显示驱动节奏
- 不需要消费端 ReadbackScheduler/输入驱动 readback 了：engine 渲染循环**每轮 composite 后
  已调 present()**，触笔进 → 攒批 → composite → present 天然输入驱动。
- 预测尖（tip 每批变）由同一节奏刷新，不额外加机制。但需确认：预测激活期无真实笔点时是否
  需要周期性重 present（tip 静止后不必，跟 A8-2 现状一致即可，不引入新轮询）。

### 5.4 resize / 生命周期（本轮裁剪边界）
- `dgcResize`（已存在）触发 swapchain 重建（沿用 create/teardown 路径）。
- 后台（onPause/ANativeWindow 释放）→ 消费端断开 surface → 重建 swapchain；基本状态机覆盖
  正常前台竖屏 + 简单前后台切换。完整状态机（旋转多模式/多窗口/窗口销毁竞态）记录为已知
  限制留后续任务，不在本轮扩张。
- 若 present 期间 surface 失效（返回 `VK_ERROR_OUT_OF_DATE_KHR`/`VK_ERROR_SURFACE_LOST_KHR`），
  置失效标志、重建 swapchain 后继续，不崩溃（错误处理是主路径要求，非回退设计）。

## 6. 消费端改动（paint-android）

- 新增 `RenderMode.SWAPCHAIN`（第 4 态，与现有 SDK离屏 / A8-4 SurfaceView / Ink 并存）。
- JNI：把 `Surface` 经 `ANativeWindow_fromSurface` 转 `ANativeWindow*`，调 `dgcSetSurface`
  （nativeWindow + 画布尺寸）；onPause/onResume 或 surface 变化时重传/断开。
- **SWAPCHAIN 模式下废弃** readback→Bitmap→Compose/SurfaceView 上屏链路
  （ReadbackScheduler / 双缓冲 bitmap / `presentBitmapToSurface` 不再参与显示）；
  画布承载在一个 `SurfaceView` 上，SDK 直接 present 到它。导出 PNG 按钮仍走
  `dgcExportPNG`（离屏权威路径，功能不变）。
- 手势/清空/撤销/颜色/预测开关全部照旧（走 C API，与 surface 类型无关）。
- 离屏对照（SDK readback）与 Ink 对照都保留，三态同机 A/B/C。
- HUD：`readMs` 在 SWAPCHAIN 模式标 N/A（无 readback）；延迟探针打点挪到 present 完成
  （与现 `drawLagProbe` 语义对等的时机），测量口径与离屏/Ink 模式一致可比。

## 7. A8-4 失败结论的处理

A8-4 `SDK_SURFACE_VIEW`（SurfaceView lockCanvas 直绘）真机实测 12ms ≈ SDK 11.8ms，
**证伪「换消费端上屏方式可改善延迟」**——按任务线规范如实收尾为「无改善/已排除」，代码保留
在 A8-4 分支作参考，不并入 main 作可用渲染模式，由本设计的 SWAPCHAIN 取代其位置。

## 8. 测试与验证计划

| 改动 | 验证方式 |
|---|---|
| SDK swapchain（Vulkan） | host 全量 ctest 全绿（离屏/determinism 零回归——权威路径未动）；Android arm64 `.so` 编出 |
| SDK 零回归 | `dgcExportPNG` 与离屏逐位一致（host 既有导出测试）；离屏对照模式仍工作 |
| 消费端 | gradle `testDebugUnitTest` + `assembleDebug`；模式切换/手势/清空/导出接线 code review |
| 真机硬验收 | MDP1221 三方同机 `SWAPCHAIN vs SDK vs Ink`，预测开/关各测；输入→上屏 ≤~7ms 且明显低于 SDK ~12ms；fps 稳定；笔迹跟手、无毛边、预测尖不慢半拍 |

## 9. 风险与边界

- **R1 swapchain 在 Mali/MTK 上的 present mode 行为未知**：MAILBOX 支持度/稳定性需真机实测；
  这是逼近 Ink 的关键变量，也是最大不确定点。测出 MAILBOX 不稳则如实退回 FIFO 记录，不强行。
- **R2 消费端渲染架构再换一次**：SurfaceView（A8-4 已铺）+ ANativeWindow 接线，与 Compose
  状态隔离需要小心（SurfaceView 在 Compose 中的层叠/尺寸变化）。沿用 A8-4 已验证的
  SurfaceView 承载方式。
- **R3 生命周期裁剪**：仅竖屏前台 + 基本重建；旋转/resize/后台竞态记录为已知限制，不扩张。
- **R4 离屏与 onscreen 双路径维护成本**：权威仍是离屏，swapchain 只做 blit 显示；若真机
  验证后 onscreen 稳定，可评估后续是否让 onscreen 成为默认、离屏收编为导出专用（本期不做）。
- **非目标**：不做 PC swapchain（paint-pc 另开）；不做预测模型/调参改动；不改 stroke
  modeler；不引入曲率感知外推。

## 10. 交付边界小结

- 改动仓库：demo（SDK `render/vulkan/` + `sdk_api/` 只读小改）+ paint-android（消费端
  RenderMode.SWAPCHAIN + JNI surface 接线）。SDK 工程约束（RAII/Pimpl/零泄漏）照旧。
- build-pipeline 硬约束（CLI + 离屏输出图像）：由既有离屏/CLI 路径满足，本设计不触碰。
- 验收真机数据为硬门槛：MDP1221 真实手指连续绘制实测，不用 `adb shell input swipe` 合成
  输入替代；「SWAPCHAIN 上屏 ≤~7ms」若实测未达，如实报告实际值并区分「未达预期」与
  「方案无效」，不粉饰。
