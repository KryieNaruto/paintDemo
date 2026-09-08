# SDK swapchain 直接上屏（Android 先行）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 SDK 渲染路径把 composite 结果直接 blit 到屏幕 swapchain 上屏（不 readback、不 CPU 搬运），真机输入→上屏从 ~12ms 降到 ≤~7ms、逼近 Ink ~5ms。

**Architecture:** 离屏 `canvasImage` 仍是唯一权威（composite 照旧画进它，determinism/导出零回归）。新增可选 onscreen 路径：`dgcSetSurface` 收到非空 `nativeWindow` 时，VkBackend 创建 `VkSurfaceKHR`+`VkSwapchainKHR`；engine 渲染循环每轮 composite 后已调的 `present()`（现 no-op）改为 acquire → 源选择（`tipHasContent_` ? merge 后 displayImage : canvasImage）→ blit → queuePresent。全程 GPU 内部。

**Tech Stack:** Vulkan（`VK_KHR_surface`/`VK_KHR_android_surface`/`VK_KHR_swapchain`）、C API（既有 `dgcSetSurface`）、paint-android JNI + Compose/SurfaceView。

**Spec:** `docs/superpowers/specs/2026-09-08-swapchain-direct-present-design.md`（本计划从 spec 论证；执行者两文同读）

## Global Constraints

- 离屏 `canvasImage` 权威路径零改动：determinism / `dgcExportPNG` / host ctest 零回归（spec §2/§5）。
- SDK 工程约束：所有权一律 RAII（沿用 `VkDeviceHandle`/`Vk*Handle` 模板，禁裸 new/delete）；对外功能经 Pimpl/既有 C ABI（本任务不新增 C API 符号，复用 `dgcSetSurface`）。
- 本轮仅 Android（paint-android），竖屏正常前台 + 基本 surface 重建；完整生命周期状态机记录为已知限制（spec §5.4/§9 R3）。
- 真机验收为硬门槛：真实手指连续绘制，**禁用 `adb shell input swipe` 合成输入替代**（spec §10）。
- present mode：优先 `VK_PRESENT_MODE_MAILBOX_KHR`，不支持/不稳退 `FIFO`，如实记录不强行（spec §5.2）。
- 不回退/兜底路径：依赖缺失即补齐或如实报阻塞（build-pipeline 原则）。

---

## 任务分解（跨两仓，按依赖序）

### Task 1（demo/SDK）：VkBackend 可选 swapchain 创建与 teardown

**Files:**
- Modify: `render/vulkan/vk_backend.cpp`（`init(PlatformSurface, w, h)` 现仅打印占位日志 `"windowed surface path not implemented (B2-1)"`）
- Modify: `render/vulkan/vk_backend.h`（新增成员/前置声明）
- Modify: `render/vulkan/vk_handles.h` 或同目录句柄模板（若需新增 `VkSurfaceKHR`/`VkSwapchainKHR` 的 RAII 包装，沿用现有 `Vk*Handle` 模式；否则用现有模板实例化）

**Interfaces:**
- Consumes: `IRenderBackend::init(PlatformSurface, int, int)`（`dgcSetSurface` 已调它）；`PlatformSurface` 即 `ANativeWindow*` 落地类型。
- Produces: `VkBackend` 内部状态「swapchain 已绑定」；`resize(w,h)` 分流离屏重建 canvas vs onscreen 重建 swapchain。

- [ ] 摸清实例/设备创建时扩展 enable 现状（VkInstance/VkDevice 创建代码里 extension 列表），补 `VK_KHR_surface`/`VK_KHR_android_surface`（实例级）+ `VK_KHR_swapchain`（设备级），Android 构建条件启用。
- [ ] `VkBackend::init(surface != nullptr, w, h)`：经 `vkCreateAndroidSurfaceKHR` 建 surface → 查 present mode（优先 MAILBOX，记录实际选中 mode）→ 建 swapchain（image count/format/尺寸取 surface 能力）→ 存句柄。
- [ ] teardown：销毁 swapchain/surface（析构顺序与 image/device 一致，RAII）。
- [ ] 离屏路径零行为变化：`init(nullptr, ...)` 仍走 `initOffscreen`，present no-op。
- 验证：Android arm64 `.so` 编出；host ctest 全绿（离屏未动）；无 ASan/LSan 泄漏（host sanitize 构建）。

### Task 2（demo/SDK）：present() 真实现 + tip 显示源选择

**Files:**
- Modify: `render/vulkan/vk_backend.cpp`（`VkBackend::present()` 现 no-op，`~1389`；`RefreshReadbackCacheLocked` 的 merge/blit 源选择逻辑 `~1246-1269` 作复用参考）
- 不改：`core/engine.cpp` 渲染循环（每轮 composite 后已调 `present()`，`444`）

**Interfaces:**
- Consumes: Task 1 的 swapchain；既有 `tipHasContent_`/`canvasImage`/`tipImage`/`displayImage` + merge 管线。
- Produces: `present()` 行为：离屏 no-op；onscreen acquire →（有 tip：先 merge canvas+tip→displayImage）→ blit 源（tipHasContent_ ? displayImage : canvasImage）→ `vkQueuePresentKHR`。

- [ ] onscreen 绑定下 present 主路径：acquire（处理 `VK_ERROR_OUT_OF_DATE_KHR`/`SUBOPTIMAL` → 重建 swapchain 后重试）→ 源选择 → `vkCmdBlitImage` → present。
- [ ] tip 显示：复刻 `RefreshReadbackCacheLocked` 的 merge dispatch/barrier 约定到 present 前（仅 `tipHasContent_` 为真时付这次 merge，与离屏读回同一口径）。
- [ ] 错误处理：surface 失效/out-of-date → 置失效、重建 swapchain 后继续，不崩溃。
- [ ] 生命周期：`resize`/surface 重建调用点接入 Task 1 teardown/create。
- 验证：编出；离屏/导出/determinism host 测试零回归（present 语义只在 onscreen 分支生效）。swapchain 功能本身需真机（无 headless surface），依赖 Task 3/4 消费端接线后才可实测。

### Task 3（paint-android）：RenderMode.SWAPCHAIN + JNI ANativeWindow 接线

**Files:**
- Modify: `app/src/main/java/com/dgcamp/paint/jni/PaintNative.kt`（新增 native 入口传 Surface）
- Modify: `app/src/main/cpp`（jni `paint_android_jni.cpp`，现 `nativeInit(w,h)` 纯离屏，见 PaintNative.kt `:17`）
- Modify: `app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`（RenderMode 第 4 态 `SWAPCHAIN`；HUD `readMs` 标 N/A 分支；延迟探针打点挪到 present 完成）
- 复用 A8-4 已验证的 SurfaceView 承载（`app/src/main/java/com/dgcamp/paint/ui/PaintSurfaceHost.kt` 在 `task/A8-4` 分支作参考；本分支若未含则按同样模式接入）

**Interfaces:**
- Consumes: Task 1/2 的 `dgcSetSurface(ctx, ANativeWindow*, w, h)`（消费端把 Surface 转 ANativeWindow 传入）；现有手势/清空/撤销/颜色/预测开关 C API。
- Produces: `SWAPCHAIN` 模式下走 SDK present 链路、废弃 readback→bitmap→上屏；`SDK`(离屏)/`Ink` 对照模式保留。

- [ ] JNI 新入口（如 `nativeInitSurface(surface: Surface, w, h)`）：`ANativeWindow_fromSurface` → `dgcSetSurface`；surface 重建/onPause 时对应断开/重传。
- [ ] `RenderMode.SWAPCHAIN` 第 4 态 + 三态循环改四态（或按 UI 取舍：SDK↔SWAPCHAIN 快速对照 + Ink 独立入口）。
- [ ] SWAPCHAIN 分支：不启动 ReadbackScheduler/读回循环；SurfaceView 承载由 SDK present；导出按钮仍走 `dgcExportPNG`。
- [ ] HUD：`readMs` 标 N/A；延迟探针在 present 完成打点（口径与离屏 `drawLagProbe` 对等可比）。
- 验证：gradle `testDebugUnitTest`（新增模式分支单测如适用）+ `assembleDebug` 绿。

### Task 4（paint-android + demo）：真机三方 A/B/C 验收 + 回归

**Files:**
- Modify（如测得需调）: `render/vulkan/vk_backend.cpp` present mode/merge 时机调优
- 文档：`docs/ink-ab-comparison.md`（paint-android）/ demo spec §10 追加实测结论

- [ ] MDP1221 装新 APK，真机同一会话真实手指连续绘制，`SWAPCHAIN vs SDK(离屏) vs Ink` 三方、预测开/关各一轮，HUD 读 lagProbe/drawLagProbe/fps。
- [ ] 记录：SWAPCHAIN 输入→上屏 lag（目标 ≤~7ms 且明显低于 SDK ~12ms）、fps（不掉破 60）、毛边/预测尖跟手目视、导出 PNG 与离屏一致。
- [ ] MAILBOX vs FIFO 若有差异如实记录；未达目标则如实报告实测值并区分「未达预期」与「方案无效」。
- [ ] 回归：demo host ctest 全绿、paint-android 全单测绿、离屏对照模式仍工作。
- 收尾：按进程卫生清理 adb/gradle 遗孤（记录清理前后 RSS），两仓 git 干净，spec/对比报告留痕实测结论。

---

## Self-Review

- **Spec coverage**：spec §5.1→Task1；§5.2/§5.3→Task2；§6→Task3；§2/§8 真机硬验收→Task4；§5.4 生命周期边界→Task1/2 teardown+Task3 surface 重建；§7 A8-4 失败结论→在任务线收尾 A8-4 时处理（不并入本计划实现）。
- **Placeholder scan**：无 TBD/「适当错误处理」类占位；每个 Task 给出文件/接口/关键点与验证。
- **Type consistency**：`dgcSetSurface(ctx, ANativeWindow*, w, h)`、`tipHasContent_`/`displayImage`/`canvasImage`、`RenderMode.SWAPCHAIN`、`nativeInitSurface` 命名前后一致。

**已知执行期需现场核实的点**（task-plan/execute 在 worktree 内对实际代码确认，不属占位）：
- 实例/设备 extension enable 的具体代码位置与 Android 条件宏；
- `PlatformSurface` 落地类型与 ANativeWindow 的转换点；
- `Vk*Handle` 模板能否直接实例化 swapchain/surface（含对应 vkDestroy fn 签名），否则按同模板模式补一个包装；
- merge 管线复用到 present 前的最小改动方式（避免与读回路径重复）。
