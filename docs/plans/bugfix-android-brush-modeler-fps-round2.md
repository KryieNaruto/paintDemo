# Bugfix 计划：AD 画笔设置无效（陈旧二进制）+ AD modeler 参数说明不明 + AD 绘制 60→30 掉帧

> 触发：`/bugfix-pipeline`，用户 Bug 报告三条（2026-08-28，paint-android 工作空间）。
> 范围：SDK submodule（`demo`）+ 消费端 `paint-android`。走审阅门（≥80）→ 修复（TDD 先红后绿）→ 测试门（100）→ 收尾。

## 0. Bug 报告（用户原话）

1. 画笔设置无效，粗细，硬度，透明度都无效。
2. modeler参数设置，虽然有中文，但是不知道是干什么的。
3. 帧率爆降，一绘制，帧率直接从60调到30。PC没这个问题。

## 1. 复现与根因（① 已完成的实证，全部无头）

### 1.1 Bug #1：画笔设置（粗细/硬度/透明度）无效 —— 根因=陈旧二进制（SDK 源码已修）

- **SDK 源码现状**：`aca1169`（2026-08-28 02:30:17）已修复 0-2 settingId 死存储问题——
  `dgcSetBrushSetting` 0-3 分支在存 `brush_settings` 表的同时直连内核
  `IPaintKernel::setBrushSetting` → `BrushKernel::setBrushSetting` → `Brush::setBase`
  （`sdk_api/dgc_paint_c_api.cpp:479-507`，映射 radius→RadiusLogarithmic=log(v)、
  hardness→Hardness=clamp01、opacity→Opaque=clamp01）。
- **回归测试**：`test_brush_setting_applies` 已存在且**当前源码全绿**（本次 host ctest 24/24 全过，
  含 radius=40 墨迹 >1.3× 默认 + hardness/opacity 改变输出）。该测试导出
  `bugfix_brush_radius40.png` 等离屏图。
- **用户症状的根因**：**陈旧二进制**。时间戳实证：SDK 修复 `aca1169` 于 **02:30** 落地；
  superproject 于 02:33 钉到该 commit；而用户设备上的 APK `app/build/outputs/apk/debug/app-debug.apk`
  **构建于 02:08**，早于修复——设备上跑的 `libdgc_paint.so` 不含 0-2 生效逻辑，与修复前
  "死存储"症状（三个滑杆全无效）精确吻合。
- **影响面**：消费端接线（`PaintScreen.kt:295` → `PaintNative.nativeSetBrushSetting` →
  `paint_android_jni.cpp:83` 用 `DGC_DEFAULT_BRUSH`=1 → 内核已建默认笔刷句柄 1）在当前源码下
  **正确**；SDK 测试证明内核按 handle 1 生效。无剩余代码缺陷。

### 1.2 Bug #2：modeler 参数有中文但不知道干什么 —— 消费端 UI 说明缺失

- **复现（无头读码实证）**：`PaintScreen.kt:61-74` 的 `BRUSH_SETTINGS` 列表，modeler 滑杆
  （id 4-12）标签只有技术名 + 英文标识（如 `弹簧质量常量 spring_mass_constant`、
  `卡尔曼过程噪声 kalman_process_noise`），**无任何通俗效果说明**。UI 层（`PaintScreen.kt:285-299`）
  每滑杆只渲染 label + 读数，无说明文本。
- **对照**：SDK 唯一事实来源 `docs/brush_settings_mapping.md` 的「改参效果（人工可辨）」列
  有 9 条通俗白话（如「越大响应越快、越跟手」「越大抑制过冲越强、运动越粘滞」），
  但消费端 UI 完全不展示。
- **根因**：modeler 参数本身已生效（`test_modeler_param_changes_output` 绿：PREDICTION_INTERVAL_MS
  极大/极小输出 PNG 不同），但面板缺少面向用户的通俗说明 → 用户不知道每个滑杆干什么。
- **影响面**：仅 `paint-android` 调试面板可读性；SDK 零改动。

### 1.3 Bug #3：绘制时 60→30 掉帧（Android，PC 无）—— SDK 快照刷新频率过高

- **设备实测量（SDK 自有文档 `docs/perf/瓶颈分析.md`，XP-Pen MDP1221 / Mali-G57 MC2）**：
  全画布读回（GPU→CPU）在 Mali 上是主要剩余瓶颈（纯 SDK 读回 avg **13.4ms**；
  HOST_COHERENT memcpy 3.1MB ~11ms；HOST_CACHED 后降到 ~2-4ms）。
- **代码实证**：`render/vulkan/vk_backend.cpp:827-830` —— `CompositeLocked` 在**每一次**
  composite 批提交完成后无条件调用 `RefreshReadbackCacheLocked()`（全画布
  `vkCmdCopyImageToBuffer` + `SubmitAndWait` + 3.1MB memcpy，刷新 readback 快照缓存）。
  `ClearCanvasLocked:852` 同样。渲染线程在连续绘制时每 ≤`kMaxBatchDurationMs`=4ms
  composite 一次（`core/engine.cpp:305-310` overCap 触发）。
- **因果链（Android 弱 GPU）**：连续绘制 → 渲染线程每 4ms 一次 composite → **每次**都付
  全画布 GPU→CPU 快照拷贝（Mali 数 ms）→ 渲染线程过载 + Mali GPU 饱和 → 主线程每帧渲染
  （readback 拷贝 + `copyPixelsFromBuffer` + Compose `drawImage`，与渲染线程共享同一 GPU）
  被拖长到 >16.7ms → FPS 从 60（空闲，无 composite）掉到 30（绘制中）。
- **PC 对照**：桌面 GPU（UHD770）快照拷贝 ~1ms，渲染线程轻松跟上 → 不掉帧。解释
  「PC 没这个问题」。
- **无头复现探针**（`/tmp/probe_fps/readback_impact.cpp`，1080×720、500 点）：Mode A（纯 stroke）
  15.5ms vs Mode B（每点 strokeTo+readback）72.1ms，**slowdown 4.66×**——readback 高频调用
  把批量 composite 收益打回原形（与 P7-2 任务书 `PC-Android真机性能瓶颈修复.md` 记录的
  「readback 高频调用逼渲染线程极小批量 flush」同一机制）。快照刷新每 composite 一次即此机制
  在 GPU 侧的体现。
- **影响面**：所有走 readback 快照缓存的消费端（paint-android 每帧、paint-pc 每帧）。P7-2
  任务书产出 1 的 flush 节流**尚未实现**（`core/engine.cpp` 无 `kMinFlushIntervalMs`），但本 bug
  的主因是快照刷新频率，不依赖 flush 节流。

## 2. 修复方案

### 2.1 Bug #1：无代码改动 —— 交付重建 APK（陈旧二进制修复）

- 源码已是修复态（1.1）。**修复交付物 = 用当前 submodule（aca1169）重建 `app-debug.apk`**
  并安装/核对设备二进制（`scripts/` 现有 adb install 链路），使设备真正跑上含 0-2 生效逻辑的
  `libdgc_paint.so`。
- 回归：既有 `test_brush_setting_applies` 保持绿（已覆盖）。真机滑杆生效待用户复核（如实标注）。
- 不加回退/绕过（不引入"版本警告弹窗"等掩盖方案）。

### 2.2 Bug #2（消费端）：modeler 滑杆补通俗效果说明

- `PaintScreen.kt`：`BrushSettingSpec` 增加 `effect: String` 字段（**取自**
  `sdk/docs/brush_settings_mapping.md`「改参效果（人工可辨）」列，逐条对齐 9 个 modeler 参数）；
  滑杆 label 下方渲染该说明（次级文本，字号更小、颜色更淡）。
- "Stroke Modeler" 段落（`D6-1` 段标题下方）加一段白话引导：「以下参数控制笔迹的平滑/预测
  引擎（stroke modeler）：数值越大 → 效果见各滑杆说明；改动仅在笔画之间生效，对新笔画生效」。
- 不改 SDK、不改滑杆范围/默认值（已对齐 SDK 新默认）。

### 2.3 Bug #3（SDK）：快照刷新从「每次 composite」改为「消费者请求/结算时」

**目标**：连续绘制中把全画布 GPU→CPU 快照拷贝从「每 4ms 一次」降到「每次 readback/drain
请求 + 输入排空结算时一次」，消除 Mali GPU 饱和。

**设计**（保持 C API ABI 与既有 direct-backend 测试兼容）：

1. `core/interfaces/i_render_backend.h`：新增虚方法
   `virtual void requestSnapshotRefresh() {}`（默认空实现；Null 桩无需改）。
2. `render/vulkan/vk_backend.{h,cpp}`：
   - `Impl` 增加 `std::atomic<bool> snapshotRefreshRequested_{false}`（非阻塞标志，线程安全，
     不参与 GPU 提交串行化）。
   - 覆写 `requestSnapshotRefresh()`：`store(true)`，不碰 GPU、不阻塞。
   - `CompositeLocked` 末尾：`if (snapshotRefreshRequested_.exchange(false)) RefreshReadbackCacheLocked();`
     —— 只在消费者请求过时才付全画布快照拷贝；overCap 自动合批的 composite 不再刷新。
   - `ClearCanvasLocked` 保持**无条件**刷新（清屏正确性）。
   - 新增 `DGCPAIN_TEST_HOOKS` 计数器 `snapshotRefreshCount_`（每次实际刷新 +1）+ 访问器
     `testSnapshotRefreshCount()`（对齐既有 `testDispatchCount` 模式，`vk_backend.h:48-53`）。
3. `core/engine.cpp`：
   - `requestFlush()`（非阻塞 catch-up，`dgcReadbackPixels`/`dgcExportPNG` 调用）：在置
     `flush_requested_` 的同时 `backend_->requestSnapshotRefresh()` —— 消费者想要新鲜快照。
   - `flush()`（阻塞 drain）：经复用 `requestFlush()` 联动置位；**排空完成后追加
     `backend_->flushReadbackCache()` 收尾强制刷新**（实现修订：`requestSnapshotRefresh` 的
     单次原子标志只会被 drain 的**首个** post-request composite 消费，无法覆盖尾部多批输入；
     故 drain 在 `composited_==submitted_` 后由调用线程同步执行一次全画布快照拷贝，保证
     「最后一次 composite 之后的输入尾部」也被捕获——dgcFlush 本就阻塞、非每帧，追加 GPU
     等待与既有语义一致）。
   - `renderLoop` 队列排空分支**不再** `requestSnapshotRefresh()`（实现修订：此处结算刷新
     会让刷新次数变成时序相关、回归测试 flaky，实测 refresh 在 4~23 间抖动；移除后刷新
     频率 = clear + 消费者请求 + drain 收尾，确定性可断言。正确性由 drain 收尾强制刷新
     + 读回请求经下一次 composite 消费覆盖，语义不变）。
4. `sdk_api/dgc_paint_c_api.cpp`（仅 `DGCPAIN_TEST_HOOKS`）：新增测试访问器
   `dgcTestSnapshotRefreshCount(DgcContext*)`（`dynamic_cast<VkBackend*>` 读计数），供 C API 级
   回归测试使用；生产构建零开销。

**确定性**：快照刷新时机从"每 composite"变为"每请求"，但内容仍由渲染线程在完整 composite 批
提交后发布 → 读回永远读到"完整画布"（≤1 批滞后，语义不变）；drain 请求强制刷新 → 精确像素
路径不变。

## 3. 回归用例设计（先红后绿，全部无头）

1. **（SDK，Bug #3）`test_snapshot_refresh_throttle`**（新增，C API 级 + DGCPAIN_TEST_HOOKS）：
   - 场景：1080×720 离屏；连续 stroke（足够多点触发多次 overCap composite），期间按
     「每 N 点一次」的消费节奏做 readback（镜像 Android 每帧读回）。
   - **红（修复前）**：`snapshotRefreshCount ≈ compositeCount`（每次 composite 都刷新）。
   - **绿（修复后）**：`snapshotRefreshCount ≪ compositeCount`，且 ≈（readback 次数 + 结算次数）；
     同时最终 readback 断言墨迹完整（无孔洞）+ 导出 PNG（`bugfix_snapshot_throttle.png`）
     供无头对比。
   - 门槛给容差：`snapshotRefreshCount <= compositeCount / 2` 且 > 0。
2. **（SDK，Bug #1）`test_brush_setting_applies`**（既有）：保持绿，作为 bug #1 回归。
3. **（app，Bug #2）`BrushSettingSpecTest`**（新增 JUnit，`app/src/test/`，对齐 `CoordsTest`）：
   - 断言 `BRUSH_SETTINGS` 中**所有 modeler spec（id≥4）的 `effect` 字段非空且非技术标识本身**
     （即说明 ≠ 参数名/英文标识）；radius/hardness/opacity（0-2）可注明"立即/下一笔生效"。
   - **红**：现无 `effect` 字段（编译失败即红）；**绿**：字段存在且断言通过。
4. **既有零回归**（全绿底线）：`test_perf_regression` / `test_continuous_input_regression` /
   `test_readback_drain` / `test_midstroke_readback` / `test_determinism` / `test_offscreen` /
   `test_brush_setting_applies` / `test_modeler_param_changes_output` / `test_stroke_predictor` /
   `test_composite_barrier_repro`（TEST_HOOKS 构建）等。

**直连 `VkBackend::composite()` 的既有测试影响**（`test_gpu_dab_raster`/`test_brush_offscreen`/
`test_renderdoc_capture`/`test_composite_barrier_repro`）：修复后 composite 不再无条件刷快照，
若这些测试依赖 composite 后 `readback()`（快照缓存）取值，需改为调用 `requestSnapshotRefresh()`
或走 `exportPNG`（GPU 直读路径 `ReadbackLocked`，不受影响）——implement agent 逐一核对并按
最快路径修正，且作为既有回归全绿验证的一部分。

## 4. 影响面核对

- **SDK 改动文件**：`core/interfaces/i_render_backend.h`（+2 虚方法：`requestSnapshotRefresh` /
  `flushReadbackCache`，均默认空）、`render/vulkan/vk_backend.{h,cpp}`（请求标志 +
  CompositeLocked 门控 + drain 收尾强制刷新 + 计数 hook）、`core/engine.cpp`（requestFlush
  联动 + flush 收尾强制刷新）、`sdk_api/dgc_paint_c_api.cpp`（仅 TEST_HOOKS 访问器）、
  `tests/CMakeLists.txt`（注册新测试）。
- **不动**：C API 签名/ABI、`dgc_paint_c_api.h` 对外契约、笔刷内核、stroke_predictor 算法、
  `paint-pc`、Null 后端（默认空实现零改动）。
- **消费端改动**：`paint-android`（Bug #2 标签说明 + Bug #1 重建 APK）。
- **既有调用方**：`cli/dgc_cli` 读回走同一快照路径，行为语义不变（读回可能 ≤1 批滞后，
  与既有契约一致）；所有 `dgcReadbackPixels` 调用方契约不变。

## 5. 验证方式

1. host `ctest` 全绿（含新增 `test_snapshot_refresh_throttle` + 既有零回归）。
2. `-DDGCPAIN_TEST_HOOKS=ON` 构建下 `test_snapshot_refresh_throttle` 先红后绿（记录两态数值）。
3. 离屏 PNG 落盘（`bugfix_snapshot_throttle.png` / `bugfix_brush_radius40.png` 等）供对比。
4. `paint-android`：`./gradlew testDebugUnitTest` 全绿（含新增 `BrushSettingSpecTest`）；
   `./gradlew assembleDebug` 重建 APK（Bug #1 交付）。
5. **Android 真机量化（如实标注，不伪造）**：60→30 掉帧的 60fps 达标依赖真机实测
   （弱 GPU 场景，`test` 门禁只保证无头回归 + 编译绿；真机项交付时标注「待真机确认」，
   与既有 `bugfix-settings-modeler-fps.md` §5.6 的「真机人工复核」先例一致）。

## 6. 风险

1. **Bug #3 快照节流对 Mali 上 fps 收益的量化**：无头只能证明"快照刷新频率从每 composite
   降到每请求"这一机制被修复（回归测试断言），60fps 达标依赖真机；若真机仍不足，属遗留项
   如实报告，不打假通过。
2. **direct-backend 测试**若依赖旧"composite 即刷快照"语义：已在 §3 列出，逐一核对修正，
   纳入零回归底线。
3. **drain 后快照新鲜度**：修复保证 `dgcFlush` 联动请求刷新，`test_determinism` 精确像素
   语义不变；实现时用 `test_readback_drain`/`test_determinism` 验证。
4. **消费端重建成本**：gradle assembleDebug 需 NDK 28.2（本机 `/usr/lib/android-sdk/ndk/` 已有），
   无设备连接（`adb devices` 当前全 offline）时仅交付 APK + 说明，装机核对待用户连接。
