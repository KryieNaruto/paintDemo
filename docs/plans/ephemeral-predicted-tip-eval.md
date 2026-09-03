# 评估 · 渲染层「预测点不永久合墨」（transient wet-tip layer）

> 状态：**可行性评估**（非实现计划）。触发：A8-1 真机复测——Mode A SDK 预测有架构性天花板：
> ① interval(16ms)<真机显示延迟(~1.5-2帧) → 从不领先只追不上；② 预测点永久合墨、沿当前切线
> 外推 → 曲线上每个 dab 沿切线突一小点（弧外毛边）。修复「不过冲」(0aa99e1) 已去掉大越界尾，
> 但①②由「永久合墨 + 切线外推」的本质矛盾产生，模型器/参数层无法根治。本文评估根治方向：
> **预测点只临时显示（wet-tip 层），落笔/提交时不进永久墨**。评估后如需实施，另起新任务走
> brainstorm→计划→审阅/测试双门，不在本文档内实现。

## 1. 现状（代码实证）

- 数据流：外部 StrokePoint →（predictor 激活时 Update→真实点 + Predict→预测点，均带
  `is_predicted`）→ engine `inputLoop` 各自压成 `StrokeEvent`（事件内 point 保留 `is_predicted`）
  → brush 线程 `kernel_->strokeTo(brush, ev.point)` 产出 `StampData`（core/types.h:11 ——
  **无 is_predicted 字段**）→ `RenderBatch` → 渲染线程 `backend_->composite(stamps)` →
  **单张常驻 storage image**。
- `is_predicted` 只在 modeler 输出层有意义；跨过 `strokeTo` 后**不可区分** → 预测 dab 与真实 dab
  一样永久 over-composite 进画布。这就是「预测点永久合墨」的直接原因（core/engine.cpp inputLoop
  217-234、brushLoop 262-274、render/vulkan/vk_backend.cpp composite 全量入 storage）。
- readback/导出共用同一 storage：`dgcReadbackPixels`（Android 显示路径）
  与 `dgcExportPNG` 都读同一画布快照缓存（sdk_api/dgc_paint_c_api.cpp:380-430）。

## 2. 目标行为（对照 Ink）

预测点的用途是**在绘制进行中把湿笔尖顶到手指附近**（遮盖 Mode A 显示延迟），一旦真实点追到、
或落笔结束，预测点不应留在最终墨里。理想：
- 绘制中：画布上显示「已提交真实墨 + 一段预测湿尖」；
- 落笔/导出：只含真实墨（预测尖被丢弃）；
- 曲线：预测尖沿切线外推只是一小段临时显示，被真实点追上即消失 → 无弧外毛边。

## 3. 方案：瞬态 wet-tip 层（推荐）

**两片画布 + 预测批次分流**：

1. **贯通 is_predicted 到批次**：`RenderBatch` 加 `bool predicted`。engine `brushLoop` 从
   `ev.point.is_predicted` 打标（事件层已携带，改动小）。`composited_/submitted_` 屏障只统计
   真实提交（count_submission 语义已在，预测事件不计数）→ flush 语义不变。
2. **后端第二片 storage `tipImage`**：
   - 预测批次 → composite 到 `tipImage`；每次渲染线程消费预测批次前**先清 tipImage**再画当批
     （预测尖每输入重推，旧尖作废；只清该批区域/全清，量级小）。
   - 真实批次 → 原 `canvasImage`（行为零变）。
   - `kernel_->strokeTo` **不改**（预测点也走同内核产 dab，只是落点目标不同）。
3. **显示 vs 导出分流**：
   - `dgcReadbackPixels`（Android 显示）→ 返回 canvas+tip 合并（GPU merge 或读两张后合成）。
   - `dgcExportPNG` / 导出 → 只读 canvas（tip 永不出现在导出）。
   - `dgcClear` / `endStroke` / `undo` → 清 tipImage（落笔即丢弃预测尖）。
4. **interval 调大到 ~真机延迟(≈30ms)**：因预测不再永久成墨，放大 interval 不再有毛边/越界尾
   → 预测尖真正把湿笔顶到手指（治 ①）。模型器「不过冲」修复(0aa99e1)保持兼容且更稳。

**确定性/零回归面**：导出路径 & 预测 OFF 路径 = committed 单画布，逐位不变；预测 ON 只在
显示层多一块会随输入确定性地清/画的 tipImage（输入确定性 → 行为确定性）。host golden /
determinism 测试若在 stroke 进行中读 readback，需明确口径（进行中读回含 tip 与否）。

## 4. 备选（否决及理由）

| 方案 | 否决理由 |
|---|---|
| 落笔时「清区域 + 重放该笔真实 dab」 | 画布是累积 composite，重放前清掉的矩形会误删与该笔重叠的更早笔迹；除非每笔独立层（= 又回到分层，且层数随笔画增长）。 |
| 模型器只发真实点、不发预测（interval=0） | 就是现在的 OFF —— 无延迟遮盖，②消失但 ①(2cm 拖尾)完全暴露。 |
| 保持小 interval 掩盖毛边 | 治标：追不上依旧（interval<显示延迟），毛边只是变小。 |
| 消费端 Ink 接管交互（A8-1 结论方向） | 与本文案不冲突、可并行：Ink 已是「预测临时」的现成实现；若 Ink 够用则本文案优先级可降。本文案的价值 = 保留 SDK Mode A 也能有低延迟交互手感 + 导出仍干净，供「不想绑 Android Ink」的路径。 |

## 5. 改动面与风险

- **改动面**：engine（批次打标、tip 清/分流的渲染线程逻辑）· render/vulkan 后端（第二 storage、
  clear、merge/双读、显示快照口径）· sdk_api C API（readback vs export 口径分叉、endStroke/clear
  清 tip）· 消费端（interval 默认/下发时机）。kernels/brush **不改**。
- **P7 系列约束交互**：readback 快照缓存/节流（P7-1/2/3）基于单画布 + requestFlush——tip 层
  需并入「显示快照」但又要避开「每次读回付全画布 merge」的 20fps 陷阱 → 合并只在「有 tip 且被
  请求」时付（区间窄）。这是本文案**最大的工程风险点**，计划阶段必须给 readback/merge 预算。
- **undo/clear**：tip 层不参与 undo 栈（预测非笔画内容）。
- **测试**：host 无头可测——预测 ON 中间读回含 tip（像素级）、落笔后读回=纯真实、
  导出恒无 tip、确定性双跑一致；Android 真机复测 ①追不上 ②毛边 是否消失。均能走既有
  离屏 PNG + ctest 门。

## 6. 结论与建议

**技术上可行、方向正确**：预测点只临时显示是根治 ①+② 的唯一干净路径（Ink 即此架构），
且 committed/导出路径可保持逐位不变、P7 优化面基本不动。工程代价集中在一块「显示侧 tip
merge + readback 口径分叉」，属中-大改动。

**建议**：作为一个**新任务**推进（先 brainstorm 收敛 readback/merge 预算与显示口径 → 写计划 →
审阅/测试双门），不在本次 bugfix 内顺手做。若产品上已倾向「交互态直接用 Ink」，本文案可作为
备选/降级优先级记录，不必立刻实施。本文档不实现、不测试。
