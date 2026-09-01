# Bugfix 计划：PC/AD 笔刷设置无效 + AD Stroke 笔画消失 + AD 帧率上限

> 触发：`/bugfix-pipeline`，Bug 报告三条（2026-08-28）。
> 本计划对应 SDK 仓库 `demo`（`libdgc_paint`）+ 消费端 `paint-pc`/`paint-android` 的跨仓库缺陷修复。
> 走审阅门（≥80）→ 修复（TDD 先红后绿）→ 测试门（100）→ 收尾。

## 0. Bug 报告（用户原话）

1. PC/AD 笔刷设置均无效。
2. AD 仍旧设置帧率上限（已确认：现已稳定 60fps=屏刷上限，用户想解锁到无上限）。
3. PC/AD Stroke 设置均无效。
   - 补充（用户澄清）：PC 端滑杆拖到最大值，笔迹肉眼毫无变化；AD 端拖动任何 modeler 参数，笔画直接消失、画不出来了。

## 1. 复现与根因（① 已完成的实证，全部无头复现）

### 1.1 Bug #1：笔刷内核参数（settingId 0-2）无效 —— SDK 缺陷

- **复现**：直连 C API 画同一笔画，`dgcSetBrushSetting(DGC_DEFAULT_BRUSH, DGC_SETTING_RADIUS, 40)` vs 不设 → 导出的两个 PNG **字节级完全相同**（sha256 一致，墨迹像素 32352=32352）。
- **根因**：`dgcSetBrushSetting` 的 0-2 分支只写 `ctx->impl_->brush_settings[brush][settingId]`（`sdk_api/dgc_paint_c_api.cpp:452`）。全代码库 `grep brush_settings` **只有写入、无任何读取**。内核渲染用的是 `Engine::start()` 时 `createBrush(BrushParams{})` 注入的默认参数（半径≈10px/硬度0.7/不透明度1.0，`kernels/brush/brush.cpp:60-78 applyDefaults`）。这张 `brush_settings` 表被完全旁路。`dgc_paint_c_api.h:104-109` 注释自认"当前仅存参、不作用于默认笔刷"。
- **影响面**：paint-pc / paint-android 的"半径/硬度/不透明度"滑杆（D6-1 接线，均正确调用 `dgcSetBrushSetting`）→ 渲染零效果。CLI `set-param`（同样走到 0-2 分支）→ 同样无效。

### 1.2 Bug #3：Stroke modeler（settingId 4-12）—— SDK 双层缺陷

- **复现 1（塌缩成点）**：直连 C API 设**任意一个** modeler 参数为面板最大值，画 48 点波浪笔画 → 墨迹从 6608 塌缩到 **430（7%，一个点）**；8 个参数全部一致塌缩。
- **复现 2（抹平形状）**：即使给了正确时间戳（`dgcSetFixedTime`），墨迹也只恢复到 22%（ft=5556µs）~60%（ft=40000µs）；ASCII 渲染确认输出是**水平杠**而非波浪。白盒直跑 `StrokeModeler::Update`（dt=10000µs）：输入波浪 y∈[~57,~83]，**输出 y 范围仅 [70,74]** —— 垂直振荡几乎被完全抹平。
- **根因（两层）**：
  1. **时间源退化**：C API 每个点 `t_us = time_stepper.next()`（`dgc_paint_c_api.cpp:206`）；未调 `dgcSetFixedTime` 时 `FixedTimeStepper::next()` 恒返回 0（`core/determinism.h:60-63`）→ 所有点 dt=0 → `PositionModeler::Update` 里 `if (dt_s > 0.0)`（`core/stroke_predictor.cpp:163`）恒假 → **跳过弹簧积分，把每个点钉死在首点位置**（`out.x = pos_x_`，stroke_predictor.cpp:177-179）→ 整条笔画塌缩成首点一个点。
  2. **平滑动力学跟不上输入**：即使 dt>0，默认 `StrokeModelParams`（spring_mass_constant=400 → ωn=20 rad/s → 临界阻尼沉降 ~200ms，见 `core/stroke_predictor.h:57-64` 与 `PositionModeler::Configure(k=spring_mass_constant, c=spring_drag_constant)`）相对输入节奏（每点 5.5~10ms）过慢 → 弹簧把波浪的垂直分量全部抹平 → 输出点聚堆在首点附近 → 内核 dab-spacing 抑制 → 笔画成水平杠/消失。
- **影响面**：paint-pc/paint-android 的 9 个 modeler 滑杆（4-12）。AD 端"笔画消失"即复现 1 的精确症状；PC 端"毫无变化"指向（a）PC 构建陈旧或（b）滑杆拖动时 `strokeActive` 门控把调用丢弃导致 modeler 根本没激活——两项都需在修复后于真机复核（见 §5）。SDK 层缺陷本身已确定且必须修。

### 1.3 Bug #2：Android 帧率上限 —— 消费端缺陷

- **根因**：`paint-android` 是纯 Compose 上屏，帧循环 `LaunchedEffect { withFrameNanos }`（`PaintScreen.kt:136`）由 Choreographer/vsync 驱动，上限=屏幕刷新率；全仓 grep 无 `setFrameRate`/`eglSwapInterval`/present 模式开关（D6-3 的 vsync 开关只在 `paint-pc`）。SDK 离屏渲染本无 present，帧率上限完全在消费端。
- **影响面**：Android 真机锁屏刷 60fps，无解锁路径。

## 2. 修复方案

### 2.1 Bug #1（SDK）：笔刷内核参数 0-2 接入内核

- **引擎层**：不做多余的 `Engine::setBrushSetting` 中转层——对齐既有 `setBrushColor` 先例（`dgc_paint_c_api.cpp:468` 直接 `ctx->impl_->kernel->setBrushColor(...)`），C API 直接经 `ctx->impl_->kernel->setBrushSetting(default_brush, id, value)` 调内核。
- **内核层**：`core/interfaces/i_paint_kernel.h` 新增纯虚 `virtual void setBrushSetting(BrushHandle, brush::SettingId, float) = 0;`；`kernels/brush/brush_kernel.cpp` 实现为按句柄查 `Brush` 并调 `Brush::setBase(id, value)`（`kernels/brush/brush.cpp:275-283` 已有 setBase，含 RadiusLogarithmic/Hardness 等分支）。**同时必须在 `core/null/null_paint_kernel.h`（NullPaintKernel）补 no-op 实现**，否则 `DGCPAIN_HAVE_BRUSH` 关闭的非 brush 构建编译失败。线程约定与既有 `setBrushColor` 一致：仅笔画之间调用（brush 线程 strokeTo 读、C API 线程写；消费端已 `!strokeActive` 门控）。
- **C API 层**：`dgcSetBrushSetting` 的 0-3 分支在存 `brush_settings` 的同时（保留该 map 以维持 ABI/兼容），把 0-2 映射到内核 SettingId 并经 `engine->setBrushSetting(DGC_DEFAULT_BRUSH, ...)` 实际生效：
  - `0 radius` → `RadiusLogarithmic = log(max(value, 1e-3))`
  - `1 hardness` → `Hardness = clamp01(value)`
  - `2 opacity` → `Opaque = clamp01(value)`
  - `3 radius_log` → `RadiusLogarithmic = value`（直通）
- **文档**：更新 `dgc_paint_c_api.h:104-109` 注释（移除"仅存参、不作用于默认笔刷"，改为"经内核 setBase 实时生效，笔画之间生效"）；同步 `docs/brush_settings_mapping.md`（0-2 生效语义 + Fix B 若上调 spring 默认值，该滑杆建议范围 10-2000 需含新默认值）。

### 2.2 Bug #3（SDK）：修 modeler 集成 —— 时间源 + 跟踪动力学

**Fix A · 时间源（修塌缩成点）**：predictor 激活且未 `dgcSetFixedTime` 时，不能再给全 0 时间戳。实现于 C API 层（`dgcBeginStroke`/`dgcStrokeTo` 里构造 `StrokePoint.t_us` 处，`dgc_paint_c_api.cpp:194-206`）：当 `override_time==false` 且 `predictor_handle_ != nullptr` 时，改用**默认步长** `kDefaultModelerDtUs`（初值取 modeler 配置 `min_output_rate_hz` 的倒数，如默认 180Hz → 5555µs，使输入节奏与重采样周期 1:1，理由：避免人为上/下采样失真；数值允许实现时经回归测试微调，必须给出依据）。`override_time==true` 路径与 passthrough（无 predictor）路径完全不变。确定性：固定步长 → 输出仍确定。

**Fix B · 跟踪动力学（修抹平形状）**：`PositionModeler` 弹簧（ωn≈20 rad/s ≈ 200ms 沉降）与 WobbleSmoother（40ms）相对合成输入节奏过慢，抹平笔迹形状。目标行为（由回归测试锁定）：默认参数下激活 modeler 渲染的笔画，须**保持输入笔迹的形态**（横向跨度≥输入跨度的 80%，纵向极差≥输入纵向极差的 50%），不再塌缩成点/水平杠；同时保留 modeler 的平滑语义——用户调大平滑参数（spring/kalman/wobble）仍能让笔画更平滑。实现方向（实现 agent 用 systematic-debugging 定最小改动并记录依据）：
  - 首选：调整 `StrokeModelParams` 的默认 `spring_mass_constant`/`spring_drag_constant`（保持临界阻尼 ζ=1：c=2√k），使有效沉降 ≤ 2~3 个输入步长（当前 ωn=20 rad/s 需提到 ωn≈130~270 rad/s → k≈17000~70000），让弹簧跟住笔迹主体而仍平滑高频抖动。**约束**：passthrough 零变化；`test_stroke_predictor` 既有断言（平滑降方差、预测点 t_us 递增等）不得回归；默认值变更需同步消费端滑杆显示范围（见 §2.3 附注），并在计划/实现记录里给出依据。
  - 备选：`PositionModeler::Update` 增加 `dt_s <= 0` 的防御分支（不钉死在首点，而是向 target 直接推进或透传），防未来再次出现全零时间戳时塌缩。**Fix A 为必做，Fix B 为必做**，二者缺一都不能让 modeler 路径产出可用笔画。

**同步清理**：`dgc_paint_c_api.cpp` 首部注释（第 46-54 行"默认零回归"描述）在 Fix A 落地后需更新，如实说明"predictor 激活且无 override 时使用默认时间步"。

### 2.3 Bug #2（消费端）：Android 帧率解锁

- 目标：`paint-android` 摆脱 vsync 锁屏刷，达到无帧率上限（≥60fps 且能更高）。
- 方案：给 `PaintScreen` 增加"无帧率上限"绘制模式。paint-android 是纯 Compose `withFrameNanos`，无解锁 API 直接可用；可行路径（按优先级，实现 agent 择一或组合，需在计划/实现里写明选择依据）：
  - (a) `Surface.setFrameRate(...)`/窗口帧率属性（Android 12+），若 Compose 上屏路径暴露 Surface 则最轻量；
  - (b) 自建 `SurfaceView`+EGL，`eglSwapInterval(0)` 关闭垂直同步，SDK readback 结果以贴图上屏（与 paint-pc 同思路）；
  - (c) 若纯 Compose 内不可行，将该绘制/读回循环移出 vsync 回调（自建 Choreographer-free 循环），配合 (a)/(b)。
- 附注（Bug #3 Fix B 联动）：若 `spring_mass_constant` 默认值上调，`paint-pc`/`paint-android` 调试面板该滑杆的显示范围需同步更新到新默认值附近（UI 提示性调整，不影响 SDK 功能）。
- 验证：`paint-android` `assembleDebug` 编译通过；**帧率解锁的量化验证依赖真机人工实测**（本 bug 是消费端 present 行为，SDK 无头/离屏约束无法闭环），交付时如实标注"待真机确认"而非伪造达标。

## 3. 回归用例设计（先红后绿，全部无头 C API/引擎级）

1. **`test_brush_setting_applies`**（新增，C API 级，Vulkan 离屏）：同一笔画，`DGC_SETTING_RADIUS` 设为 40 vs 默认 → **红（现在）**：两 PNG 字节相同；**绿（修复后）**：半径 40 的笔画墨迹显著大于默认（阈值：>1.3×，且横向跨度显著增大，落在 40px 半径应有的量级）。同时断言 hardness/opacity 也改变输出（可合入同测试多子断言）。
2. **`test_modeler_stroke_renders`**（新增，C API 级）：设一个 modeler 参数（如 `DGC_SETTING_KALMAN_PROCESS_NOISE=0.01`），画 48 点波浪笔画 → **红（现在）**：墨迹 ≈430（7%，塌缩）；**绿（修复后）**：墨迹 ≥ 无 modeler 基线的 50% **且** 横向跨度 ≥ 输入跨度的 80% **且** 纵向极差 ≥ 输入纵向极差的 50%（既防塌缩成点，也防抹平成杠）。不调 `dgcSetFixedTime`（验证默认时间路径）。
3. **`test_modeler_param_changes_output`**（新增，C API 级）：同一输入，`PREDICTION_INTERVAL_MS` 极大 vs 极小 → 两次输出 PNG 必须不同（证明 modeler 参数有可观测效果，即修复后"设置生效"）。
4. **`test_modeler_deterministic`**（新增或并入）：同输入同参数两次运行，输出 PNG 逐字节一致（确定性可复现）。注意：不要求"与修复前一致"——Fix B 上调弹簧默认值必然改变 modeler 输出，属预期修复，不是回归。
5. **既有回归**：`test_readback_drain`/`test_midstroke_readback`/`test_perf_regression`/`test_continuous_input_regression`/`test_stroke_predictor`/`test_determinism` 零回归（passthrough 与 override 路径不得受影响）。

## 4. 影响面核对

- **SDK 改动文件**：`core/engine.h/.cpp`、`core/interfaces/i_paint_kernel.h`、`kernels/brush/brush_kernel.cpp`、`core/stroke_predictor.h`（默认参数，如走 Fix B 首选）、`sdk_api/dgc_paint_c_api.h/.cpp`、`core/determinism.h`（如需扩展 stepper 支持默认步长）、`docs/brush_settings_mapping.md`、`tests/CMakeLists.txt` + 新增 4 个测试文件。
- **不动**：`render/vulkan/vk_backend.cpp`、`core/stroke_predictor.cpp` 的算法结构（尽量不动；Fix B 如必须改仅改默认参数或 PositionModeler 防御分支，不动其它组件）、C API 签名/ABI。
- **消费端改动**：`paint-android`（Bug #2 帧率解锁 + 可选滑杆范围）；`paint-pc`（仅 Bug #3 Fix B 联动的滑杆范围提示，可选）。
- **既有调用方**：`cli/dgc_cli` 的 `set-param`（0-2 现在会真的改变渲染，属预期修复）；`tests/test_c_api_b1_6.c`（测返回码语义，不受影响）；所有 `dgcSetBrushSetting` 调用方契约不变（返回值/语义不变，仅 0-2 从"无效果"变"有效果"）。
- **确定性**：Fix A 只在"predictor 激活 && !override"路径引入固定默认步长，该路径当前本就塌缩（坏掉），不构成回归；override/passthrough 路径完全不变。

## 5. 验证方式

1. host `ctest` 全绿（含新增 4 测试 + 既有零回归）。
2. `-DDGCPAIN_SANITIZE=ON`（ASan/LSan）重跑 ctest 零泄漏零 UB。
3. `android-arm64` preset 仍编出 `.so`。
4. 离屏 PNG 落盘供人工对比（测试 1/2/3 导出）。
5. 消费端：`paint-android` `assembleDebug` 通过（+`gradlew test` 若存在）；`paint-pc` 若改滑杆范围则重编。
6. **真机人工复核（如实标注，不伪造）**：PC 端滑杆（笔刷+Stroke）拖到最大画波浪/抖动笔迹应肉眼可见改变；AD 端拖 modeler 参数笔画不再消失；AD 帧率解锁实测 ≥60fps 且无上限。这三项依赖用户真机条件，交付后由用户确认；test 门禁对三项真机项不做"已达标的假证明"，只保证其前置（SDK 无头回归 + 消费端编译）为绿。

## 6. 风险

1. **Fix B 弹簧默认值上调的幅度不确定**（ωn 目标 130~270 rad/s 是工程估计，非实测标定）：以回归测试 2 的三条断言为标尺，实现 agent 实测校准并在实现记录里写明最终值与依据。
2. **消费端 bug #2（AD 帧率解锁）无头不可验**：只能保证编译 + 结构正确，量化 fps 依赖真机；若真机仍不达标，属于本计划的遗留项如实报告，不在测试门内打假通过。
3. **PC 端"毫无变化"是否含消费端因素（构建陈旧/门控丢弃）**：SDK 修复后需在真机复核；若确认是 PC 构建陈旧，属交付流程问题（收尾阶段核对用户实际运行二进制），非代码缺陷。
4. **`test_stroke_predictor` 等既有 modeler 测试在 Fix B 下可能需微调阈值**：断言语义（平滑降方差、预测点递增）不变，只允许在数值阈值上做有依据的微调，且需在实现记录说明。
