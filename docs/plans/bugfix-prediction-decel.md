# 修复计划 · Stroke Predictor 减速/停笔预测不过冲（「扯」根因）

> 触发：A8-1 真机 A/B（paint-android Mode A SDK vs Mode B Ink）——预测「开」时快速笔画
> 抢跑、停笔后尾部越界/回扯严重（用户原话「预测抢跑会扯特别严重」）。
> 流程：bugfix-pipeline ①→⑦。本文件为 **② 计划阶段产物**，不实现/不测试。
> 前置修复参照：`8a0ee97 fix(stroke_predictor): 预测开关两态转角干净可逆`（机制 A/B，
> 仅覆盖**转向/高曲率**，不覆盖直线减速停笔）。

## 1. 复现与根因（① 已做）

**代码级根因**（`core/stroke_predictor.cpp`）：

1. 引擎把 `Update()`（真实点）+ `Predict()`（预测点）全部送进 dab 内核，**永久合墨、无擦除**
   （`8a0ee97` 提交说明原文「预测点永久合进墨、无擦除」；engine.cpp inputLoop 把 out 里每个点
   都转成 StrokePoint 事件 → strokeTo → composite）。故**任何比真实轨迹更远的预测点都会留在成墨里**。
2. `Predict()` 的外推速度 = **Kalman 恒定速度估计**（`kalman_.velocity()`）。直线**减速到停**时，
   Kalman v 滞后于真实减速（恒定速度模型 + process/measurement noise 调校），在手指已明显减速/接近
   停止的最后若干真实点处，Predict 仍按偏高的 v 均匀外推 `n = interval/period` 个点 + StrokeEndPredictor
   停笔点 → 成墨尾部越过真实停笔点，抬手后永久留下凸出尾。
3. `8a0ee97` 机制 A（`lagDeg>40°` 抑制）判据是「Kalman v 方向 vs 最近真实位移方向」夹角——只拦
   **转向**；直线减速时夹角≈0，机制 A 不触发。机制 B（interval≤0 旁通）只覆盖「关」，不覆盖「开」。

**无头复现（host，确定性）**：见 `tests/` 计划新增回归（§3），现状数值（探测脚本 /tmp/repro3.cpp，
60Hz、ramp 8pt→2400px/s、steady 24pt、decel 10pt→0、停 2pt，interval=16ms）：

```
in_stop=1320.0  real_max=1301.4 (ovr=-18.6)   ← 弹簧/平滑不越界（真实输出反而滞后）
pred_max=1325.1 (pred past real=23.7px, past in_stop=5.1px)   ← 预测尾越出真实轨迹 23.7px
```

- 更高速度（真实快速直线 5000+px/s）外推尾按 `~v·interval` 放大 → 越界可达数十~上百 px，即用户
  所见「抢跑扯」。稳态运动中 wet tip 领先 `v·interval`（这是**要保留**的延迟遮盖），问题只在
  **减速/停笔后这段领先不随真实减速坍缩，且永久成墨**。

**影响面**：`Predict()` 是唯一条外推路径（engine.cpp inputLoop 对每个真实 StrokePoint 调一次
`Predict`）。所有「预测开」的笔画都受影响；不影响「关」（interval≤0，机制 B 早退）与 `Update()`
平滑本身。

## 2. 修复方案（模型器外推速度「随真实减速坍缩」）

不改渲染/引擎/内核（永久合墨架构不变），只改 `StrokeModeler::Predict()` 的**外推速度选取**，
使外推在手指减速/停笔时**自动坍缩**，稳态运动时**保留原有领先**。

**改动点（core/stroke_predictor.cpp，Impl + Predict）**：

1. Impl 新增极小状态：`double recent_vx_/recent_vy_`（px/s）= 最近一次真实输出的**真实速度**
   `(last_output_ − prev_output_) / Δt_us`，在 `Update()` 推真实点时随 `prev_output_/last_output_`
   窗口一起滚动（复用已有 `has_prev_` 判定；Δt 由两次输出 `t_us` 差算，需防 0/乱序）。
   `ResetLocked()` 一并清零（确定性/可逆，仿机制 A 的 prev_output_ 处理）。
2. `Predict()` 里选外推速度 `v_pred = |v_true| < |v_kalman| ? v_true : v_kalman`（按分量取 norm 更小者，
   方向随所取向量）：
   - **稳态/直线**：`|v_true| ≈ |v_kalman|` → 取任一同 ≈ v → 外推 `≈ v·interval`，**领先保留**
     （既有直线领先门 4 的 `[0.4,1.2]*v*interval` 契约不变）。
   - **减速/停笔**：真实位移快速变小 → `|v_true| < |v_kalman|` → 取 v_true → 外推点按真实减速
     收缩，末点趋近最近真实点 → **不留下越界尾**。
   - **起步加速**：Kalman v 暂态小 → 取 kalman → 领先平滑渐入（不回归）。
   - 机制 A 转向夹角门保持（仍基于 kalman v 方向 vs 真实位移方向），不受影响。
3. 均匀外推 + StrokeEndPredictor 停笔点统一改用 `v_pred`（同一 v 选取），保证末端停笔点也不越界。

> 说明：这是「消费端调大 interval 匹配真机延迟」的**前置**——若外推尾会永久越界，调大 interval
> 只会放大「扯」。本计划先修模型器「不过冲」，再谈默认 interval（见 §6 范围边界）。

**替代方案（不选，理由）**：末端 erasure/整笔重渲染（引擎/内核层，需回滚已合墨预测点，
改动面大、触碰 composite/undo/PartialDabs，超出「模型器 bugfix」范围）；无条件去掉预测
（丢延迟遮盖收益，违背 A8-1 对照方向）；单阈值 |v|<X 早退（会在起步暂态拦空，`8a0ee97`
已明确此路不通）。

## 3. 回归用例设计（TDD 先红后绿，无头）

**新增 `tests/test_predictor_decel_clean.cpp`**（仿 `test_predictor_corner_clean.cpp` 纯 CPU
白盒结构，无 gtest，main 返回失败计数），插入既有 CMake（`tests/CMakeLists.txt`）。

用例 **CheckDecelNoOverrun**（核心，先红）：
- 档案 `MakeFlickStop()`：60Hz；ramp-in 8pt 0→2400px/s、steady 24pt @2400、decel 10pt→0、停 2pt
  （≈ 探测脚本 /tmp/repro3.cpp 的 realistic fast-flick-to-lift），interval=16ms。
- 逐点 `Update`+`Predict` 收集全部输出（含 is_predicted）。
- **断言 A（红）**：所有输出点中，`pred_max - real_max <= 8px`（real_max = 非预测点 x 最大；
  pred_max = 预测点 x 最大）。现状 ≈23.7px → 红。
- **断言 B（守卫）**：steady 段仍有领先——存在 is_predicted 点 x/t 大于最后真实点（防「为干净而
  关掉预测」），且该领先点落在「最后一段稳态真实速度 × interval」量级内（≈ 现有门 4 精神）。

既有门回归（零回归，跑全量）：`test_stroke_predictor`、`test_stroke_predictor_real_time`、
`test_modeler_stroke_renders`、`test_modeler_param_changes_output`、`test_predictor_corner_clean`
（直线领先门 4 / 人形减速门 5 / 弧门 6 / 可逆 / 确定性 / PNG）。

**红→绿步骤**：先只加断言 A 的用例跑→确认红（打印现状值 ~23px）；实现 §2 改动；再跑→断言 A/B
绿；再跑既有 5 个测试零回归。

## 4. 影响面核对（复用 8a0ee97 既有断言作守卫）

| 既有行为 | 守卫 | 本改动影响 |
|---|---|---|
| 直线稳态领先 ≈ v·interval | corner 门 4 `[0.4,1.2]*v*interval` | 稳态 |v_true|≈|v_kalman|，领先不变 |
| 人形减速过 90° 角不外溢 | corner 门 5 `pred_out==0` | 转向由机制 A 拦，减速再收一步，仍 0 |
| 低曲率大弧保持领先 | corner 门 6 `emit>=half` | 弧上 |v_true|≈|v_kalman|，照常 |
| 开/关两态互切可逆、双实例确定 | corner 可逆/确定性断言 | 新状态随 ResetLocked 清零，结构同 prev_output_ |
| OFF(interval≤0)=纯真实 | 机制 B（`interval<=0` 早退在改动之前） | 不触碰 |
| 渲染五态越界墨≈P（离屏可用时） | corner PNG 断言 | 减速收尾只会更不越界 |

## 5. 无头验证方式

- `cmake --build build/host-linux -j`（或先 `cmake -S . -B build/host-linux`），跑：
  `ctest --test-dir build/host-linux --output-on-failure` 全绿；
- 新增 `test_predictor_decel_clean` 单独先红后绿记录在案（测试门核验）；
- PNG 类断言走既有离屏后端路径（不可用时白盒恒跑，如实标注）。

## 6. 范围边界（本计划交付 SDK 模型器修复）

- **本期只改 demo/SDK `core/stroke_predictor.*` + tests/**（paint-android `sdk/` submodule 前移到
  demo 新 HEAD 由消费者 setup 流程处理）。
- **consumer 侧「预测默认开 + interval 匹配真机延迟」是后续 step（独立于本 bugfix 门）**：
  依赖本修复先落地（否则调大默认 interval 会放大越界）；届时在 paint-android 用真机实测 Mode A
  真实显示延迟（screenrecord/帧提取或人手感）后定 interval，并让 app 初始化即下发 modeler 设置
  （当前仅滑杆动过才下发，默认是 OFF passthrough）。本计划不承诺 consumer 改动，防超范围。

## 7. 风险

1. `recent_v_` 在 `prev_output_` 窗口缺首个输出时不可用 → 退化为 kalman v（首个预测点前无 2 个
   真实点，外推本就应保守/无）；不引入未初始化读。
2. Δt_us 乱序/0 → 复用 `>=` 归一（防 uint64 下溢），真实速度不可算时退 kalman v。
3. 改变外推速度可能使个别既有白盒数值断言微移（如弧门 emit 计数）→ 以全量测试门为准，
   若某断言超阈值仅因数值移动非行为退化，如实报告并校准确认（不得放宽「越界/凸点」类硬门）。
