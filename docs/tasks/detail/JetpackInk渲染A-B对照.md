# 任务书 · Jetpack Ink 渲染 A/B 对照（线8-渲染对照）

> 本任务书对应 `docs/tasks/任务线.md` 任务 **A8-1**。实施计划已通过 build-pipeline 审阅门（81/100）并推送：`docs/plans/ink-rendering-android-ab.md`。本任务横跨 `demo`（SDK，**零改动**）与 `paint-android`（消费者仓库，改动主体在消费端）——参照 D6-1/2/3 与 P7-2 先例（消费端作为 SDK 能力/决策的验收宿主纳入任务线），A8-1 作为「SDK 渲染路线是否替换为 Ink」的**对照决策宿主**入表；SDK 侧零 diff，故无 SDK worktree 实体改动，验收产物落在 `paint-android` 与其对比报告。

## 背景（brainstorming 结论，已核实）

- 用户的原始诉求是「SDK 预测/渲染换 google ink」；对照验证后确认：ink 内置 modeler 与 SDK `core/stroke_predictor`（B1-5 白盒移植）**同算法**，换预测无功能性收益。
- 真正有差异的是**渲染**：androidx.ink 1.0.0（stable）以矢量 mesh 低延迟上屏、**无像素 readback**；SDK 现路径为 Vulkan 离屏 → readback（3.1MB memcpy）→ 贴图（P7-2/P7-3 在打的延迟瓶颈）。
- 方向定为 **① Jetpack Ink（拿延迟收益）**：paint-android 接 androidx.ink，与 SDK 路径**应用内开关 A/B 切换**，量化延迟差距与手感。**SDK stroke modeler 保留、零改动**；SDK 渲染路径保留作基线。

## A8-1 · 目标与产出

**目标**：在 `paint-android` 增加一条 Jetpack Ink（androidx.ink 1.0.0）低延迟渲染路径，与现有 SDK（Vulkan 离屏→readback→贴图）路径可在应用内一键切换（主交付；独立 APK 锁死 ink 仅作退路），量化两路径延迟差距与手感。SDK 侧零改动。

**产出**

1. `paint-android`：`app/build.gradle.kts` 引入 androidx.ink 1.0.0（钉 stable，不用 rc/alpha；含 `ink-authoring-compose`/`ink-brush-compose`/`ink-rendering` 等官方 setup 依赖集）+ minSdk 兼容核实（androidx.ink 若要求 API 34+ 需 bump 或 API 门控）。
2. `paint-android`：`RenderMode`（SDK/INK）枚举 + `PaintScreen.kt` 顶部开关切换；`InkStrokeCanvas.kt` 用 `InProgressStrokes`/`StockBrushes` 实现 ink 模式能画；SDK 模式路径与调试面板原样保留。
3. `paint-android`：延迟/手感测量——`LatencyMetrics.kt`（纯 Kotlin TDD）提供帧时 p50/p99 累加器 + 输入到帧延迟代理；两模式各自埋点，HUD 显示；SDK readMs 保留，ink 模式标注无 readback。
4. `paint-android`：`InkPngExporter.kt` 离屏 bitmap → PNG（满足 build-pipeline「离屏输出图像」硬约束）；SDK 基线复用 host `dgc_cli` 离屏 → PNG（不改 SDK）。
5. `paint-android`：`docs/ink-ab-comparison.md` A/B 对比报告（延迟数字表 + 手感结论 + minSdk 约束 + 是否值得换渲染的结论与建议）。

## 验收（对应 plan R1-R6，已通过 build-pipeline 审阅门）

| 序号 | 目标 | 验收方式 |
|---|---|---|
| 1 | ink 模式能画 | 真机切到 Mode B，画出可见笔画（`InProgressStrokes` 实时渲染） |
| 2 | 两模式可快速切换 | `PaintScreen` 顶部开关一次点击在 SDK/ink 间切换，即切即生效 |
| 3 | SDK 模式零回归 | Mode A 下既有链路（输入→JNI→SDK→readback→贴图 + 调试面板）行为不变；`git diff` 确认 `sdk/`（demo）零改动 |
| 4 | 延迟量化 | 两模式各自记录帧时 p50/p99 + 输入到帧延迟代理 + SDK readMs，产出可比数字 |
| 5 | 离屏输出图像（硬约束） | host `dgc_cli` 离屏 → PNG（SDK 基线）；ink 侧 on-device 离屏 bitmap → PNG |
| 6 | 手感对照 | 同一手势两模式各画（快速笔画/圈/折线），人工记录「领先/滞后/抖动」感受 |

**验收共同前提**：`paint-android` `./gradlew :app:assembleDebug` 成功；`LatencyMetricsTest` 单测绿；SDK（`sdk/` submodule）零 diff；真机 A/B 数据人工记录并写进对比报告。

**依赖理由**：A8-1 不需要 SDK 新能力——SDK 渲染/预测/内核基线（B2-1 真实 Vulkan 后端、B3-1 真实内核、P7-1 非阻塞 readback）均已合并且已被 `paint-android` 消费，故依赖取这三项已完成者，保证立即可申领。

---

## 评审打「通过」的必要条件

| 任务 | 指标 |
|---|---|
| A8-1 | `paint-android` 编译 + `LatencyMetricsTest` 单测绿；真机 ink 能画且开关可切（R1/R2）；SDK `sdk/` 零 diff（R3）；帧时 p50/p99 + 延迟代理埋点产出（R4）；host `dgc_cli` PNG + ink on-device PNG（R5）；`docs/ink-ab-comparison.md` 对比报告含真机人工手感记录（R6）。真机延迟/手感数据为人工验收项，host 自动化（构建/单测/PNG）为可验证替代，真机数据如实标注「待真机确认/人工」 |
