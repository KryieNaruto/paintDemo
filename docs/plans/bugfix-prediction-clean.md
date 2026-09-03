# Bugfix 计划 · 预测开/关两态转角干净可逆（core/stroke_predictor）

> bugfix-pipeline ②（计划）：只写本计划，不改 core/sdk_api 行为、不写实现。
> 需求方已定目标：**「预测 开/关」两态都干净可逆，转角都像 passthrough 一样无凸点**。
> 修复落 SDK **core/stroke_predictor**（机制 B + 机制 A），消费端语义不变（仍拨 12=0/16）。

## ① 根因 + 目标

### 目标（验收语义）
- **关（OFF，消费端 `dgcSetBrushSetting(12,0)`）**：真·无预测外推、转角干净。
- **开（ON，interval=16）**：平滑 + 预测保留（直线快速段仍领先），但**抑制 90° 过弯/高曲率处的预测外溢**，转角干净。
- 两态互切**可逆、无状态残留**（ON1≡ON2 字节级）。
- 干净参照 = **P（passthrough，从未激活 modeler）**；用户真机补充确认：P 转角零凸点即干净基线。修复目标 = 让 modeler 激活后的 OFF 与 ON 也达到同等级转角干净，**不新增 modeler 去激活 API**。

### 根因（代码级）

全部在 `core/stroke_predictor.cpp` 的 `StrokeModeler::Predict()`（:400-432）内。

**机制 B —— 关（interval=0）≠ 真·无预测**：
- `Predict()` :408-411 用 `interval_us = prediction_interval_ms*1000` 布点数
  `n = uint64(interval_us / period_us)`，:411-414 **`if (n==0) n=1`** —— interval=0 时仍强制 n=1。
- :425-431 **无条件追加 StrokeEndPredictor 停笔点**（`end.ok` 在 ≥1 次 Update 后恒真，见 :294-307）。
- ⇒ interval=0 时每个真实点仍产出「1 个沿 v 外推点 + 1 个停笔点」——不是真透传。白盒取证：interval0 输出 273 点、**含 ~110 个 is_predicted 点、其中 27 个盘心越出理想框（最远 4.1px）**；在部分配置下这些残余在 PNG 留出 25px 级淡墨（① 记录的 OFF 残留）。

**机制 A —— 开（interval=16）过弯外溢**：
- `Predict()` :405-423 沿卡尔曼速度 `v = kalman_.velocity()`（:406）**无条件均匀外推** n 个点（`last + v·(k·period)`，:417-419）；:426-431 停笔点同样沿 v（`end_pred_` 在 Update :393 以 `kalman_.velocity()` 更新）。
- 90° 转角处**卡尔曼恒定速度估计方向滞后真实切线**（速度外推越过刚转过的角），预测点落包络外；engine（`core/engine.cpp` inputLoop :216-234）对每个真实点 `Update`+`Predict`，输出**直接合成进墨（composite，无擦除）** → 转角外溢被**永久合入最终墨**，幅度随 interval 放大。
- 白盒取证（我重跑，canonical const 1500px/s r5，见下）：interval16 的 165 个预测点中 **50 个盘心越框、最远 9px**。

**证据（当前代码，红）** —— 正式回归所锚定的 red 值：

```
[wb:intv16] interval=16ms out_points=328 real_outside_box=0 pred_outside_box=50(max=9.0px)
[wb:intv0 ] interval= 0ms out_points=273 real_outside_box=0 pred_outside_box=27(max=4.1px)

PNG 转角越界（margin=radius+3=8px，P/OFF/ON 同一条 640 闭合框，60Hz 真 t_us 1500px/s）：
state  count  maxpx
P      0      0.0     <- 干净基线
ON1    272    4.0     <- 红（预测外溢带）
OFF    0      0.0     <- PNG 已净，但白盒有 27 个越框预测点（机制 B 残余，弱配置下留墨）
ON2    272    4.0     <- 红；与 ON1 字节级一致（无内部残留，可逆前提成立）
O0     0      0.0     （首设即 OFF）
human 档案同质：ON1 243/3.0（r5）；P/OFF 0。r8..r20 同形态（ON1 count 300±，OFF 0）。
```

> 机制 A 根因的**直接几何证据**（我的离线重放，55 真实点驱动白盒）：
> 全部 50 个越框预测点的「卡尔曼 v 方向 vs 最近真实位移方向」夹角 θ ∈ **57°–160°**
> （逐边：RIGHT 23 个 θ≈57-75°、BOTTOM 21 个 θ≈105-122°、LEFT 4 个、FIL 2 个）；
> 不越框的预测点大多 θ 小（直线段 θ≈0）。⇒ **外溢 = 高曲率/转向处 v 方向滞后于真实切线**。

## ② 修复设计（可编码，落 core/stroke_predictor.cpp）

改动全部在 `StrokeModeler::Predict()` 及 `Impl` 的一个小状态量上；**Pimpl/RAII 边界不变、无裸 new/delete、无新增 C ABI**。

### 机制 B：interval<=0 → Predict 完全旁通

在 `Predict()` :400-402 的 `has_output_` 检查之后、任何外推/末端计算之前加早退：

```cpp
if (impl_->params_.prediction_interval_ms <= 0.0f) {
    return;   // OFF：不产出任何外推点，也不追加 StrokeEndPredictor 停笔点
}
```

- 覆盖 :411-414 强制 n=1 与 :426-431 无条件停笔点两条路径。
- `Update()` 不受影响（平滑照常）；OFF 输出 = 纯真实平滑点。
- 正的小 interval（如 1ms）**不在本计划范围**：仍走既有 n 计算，仅去掉「interval<=0」这一个 OFF 语义空洞（原 :411-414 的 n=0→1 兜底保留给 (0, period) 正区间，避免动测试 `test_modeler_param_changes_output` 的 1ms 分支预期）。实现时如需更严可顺带把 n=0→1 兜底改为「正区间至少 1 点」，但本计划不强求。

**engine 空输出消费安全性（已核对，无需改 engine）**：
- `core/engine.cpp` inputLoop :216-234：每次外部 StrokePoint 先 `pred->Update(ev.point,&out)` 再 `pred->Predict(&out)`。`Update()` 经 Resampler（stroke_predictor.cpp :101-133 的 `Push`）**保证恒产出 ≥1 个真实点**（首点必发；dt<=period 原样发；稀疏插值分支 k=1..n 至少 1 点），故 `Predict()` 追加 0 点时 `out` 仍非空。
- :228-234 按 `out` 最后一项 `count_submission=true` 展开事件、屏障计数（`submitted_`/`composited_`）按外部提交计，**与展开出的点数无关**（:220-227 的 D6-1 注释本就依赖「out 恒非空」，现明确该不变量由 Update 单独保证）——空 `Predict` 不崩、不破坏 End/屏障计数。
- 建议顺带把 engine :224-227 注释补一句「Predict 可能追加 0 点（OFF/高曲率抑制），非空由 Update 单独保证」，纯注释、不改逻辑。

### 机制 A：高曲率/转向处「抑制外推」判据（含可照抄规则）

**思想**：本引擎把每个预测点都永久合进墨（无擦除），所以任何「方向会偏离真实轨迹」的预测都会在转角留下抹不掉的凸点。真正的健康判据不是「速度大小」而是 **卡尔曼 v 的方向是否还贴着最近真实行进方向**：直线匀速段 v≈真实切线 → 照常按 v·interval 领先且与未来真实墨重合（无害）；转过 90°/高曲率处 v 滞后 → 预测盘心必探出包络 → 本轮**不产出任何预测点**（uniform 外推与 StrokeEndPredictor 停笔点一起抑制）。

**新增极小状态**：`Impl` 目前只存最近一个真实点 `last_output_`（:361-362）。判据要「最近真实行进方向」= `last_output_ − prev_output_`。在 `Impl` 加：

```cpp
bool  has_prev_ = false;   // ResetLocked() 置 false
StrokePoint prev_output_{}; // Update() 每次推真实点时滚动成前一个
```

`Update()` :390-397 循环内每推一个 `pos` 就把 `prev_output_=last_output_` 后再 `last_output_=pos`（首点无 prev）。`ResetLocked()` :340-348 一并清零。无其余跨笔状态、无计数器 → 可逆/确定。

**Predict() 判定（紧接机制 B 早退之后、外推循环 :415 之前插入）**：

```cpp
const double interval_us = impl_->params_.prediction_interval_ms * 1000.0;
// （机制 B 已保证 interval_us > 0 到这里）
const Vec2 v = impl_->kalman_.velocity();
const StrokePoint& last = impl_->last_output_;
const bool haveDisp = impl_->has_prev_ &&
    (last.x != impl_->prev_output_.x || last.y != impl_->prev_output_.y);
if (haveDisp) {
    // 最近真实行进方向 d = last - prev_output_（px）
    const double dx = last.x - impl_->prev_output_.x;
    const double dy = last.y - impl_->prev_output_.y;
    // v 对 d 的滞后角 θ（无符号）：atan2(|d×v|, d·v)
    const double cross = dx * v.y - dy * v.x;
    const double dot   = dx * v.x + dy * v.y;
    const double lagDeg = std::fabs(std::atan2(std::fabs(cross), dot)) * 180.0 / M_PI;
    if (lagDeg > 50.0) {          // 启动阈值；校准带 40°–60°，以 ③ 红→绿为准微调
        return;                    // 高曲率/转向：uniform 与停笔点都不发
    }
}
// 通过 → 既有 :415-431 逻辑（n 个均匀外推点 + StrokeEndPredictor 停笔点）
```

**为何直线匀速段不受影响、仍按 v·interval 领先**：直线段 v 与最近真实位移同向 → `cross≈0 → lagDeg≈0°`，不触发抑制，`:415-423` 照常发 `last + v·(k·period)`（k=1..n，总长 v·interval）。抑制只在 v 明显偏离真实行进时触发——这正是会探出包络的情形。

**为什么是 50° 起、为何覆盖到全拐角**：离线取证所有越框预测点 θ≥57°，其中 ≤50° 的为 0 个；50° 阈值击杀 50/50 越框点、保住直线段预测。若实现调参把阈值放宽到 ≤60°，仍能击杀 47/50（剩 3 个右/底边 57-60° 边缘点，白盒零越框断言会红）——故阈值不得超过 60°，以 ③ 的零越框白盒断言为最终门。

**可选微调（不影响可逆性，需 ResetLocked 清零的都得清零）**：
- 速度下限（dwell/停笔不再外推）：`|v| < 60 px/s → return`（可与停止距离参数联动；本计划视作可选第二道门，默认加上更稳）。
- 滞回：连续 2 轮 lagDeg 超阈才抑制、连续 2 轮低于阈（减 10°）才恢复，避免阈值边缘每轮抖动。**不得**用任何跨笔状态：一切滞回计数器必须在 `ResetLocked()` 清零，否则破坏 ON1≡ON2。

### 状态可逆（B/A 共同约束）
- 无新增去激活 API、无跨笔残留：B/A 新增状态只有 `prev_output_`/`has_prev_`（`ResetLocked()` :340-348 清），A 判据每轮用「当前真实输出 + 当前 kalman v」纯几何计算，无持久 latch（若用可选滞回则按上条在 Reset 清零）。
- 既有 ON1≡ON2 字节级证据（272≡272，白盒同流同参）必须保持；③ 有专门的可逆断言。

## ③ 回归用例（先红后绿，无头）

把临时 repro 收敛为正式回归：`tests/test_repro_corner_bug.cpp` → **`tests/test_predictor_corner_clean.cpp`**，
在 `tests/CMakeLists.txt` **替换**末尾的 TEMP 注册块（删掉 `test_repro_corner_bug` 的 add_executable/add_test 与源文件），改为：

```cmake
add_executable(test_predictor_corner_clean test_predictor_corner_clean.cpp)
target_include_directories(test_predictor_corner_clean PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_predictor_corner_clean PRIVATE dgc_paint)
add_test(NAME test_predictor_corner_clean COMMAND test_predictor_corner_clean)
```

正式测试 = 白盒几何断言（恒跑，纯 CPU、确定性、无 GPU/AA 抖动）+ 离屏 PNG 断言（后端可用才跑，证据落盘）。沿用 repro 的序列构造 `MakeClosedSquare`（160..480 闭合框、60Hz 真 t_us、1500px/s、r5、margin=radius+3=8）。

**(a) 转角越界墨 ≈ P（PNG 黑盒；红→绿）**：复用 `Protrude()` 统计，五态 P/ON1/OFF/ON2/O0 断言：
- `ON1.count ≤ P.count + 16 且 ON1.maxpx ≤ P.maxpx + 1.0`（P 恒 0）——**当前红（272/4.0）→ 修后绿**；
- 同款对 `OFF`、`ON2`、`O0`。
- **容差理由**：P 基线 margin=radius+3 已内嵌盘半径(5) + 抗锯齿余量（~3px），干净转角的 AA 至多探出带边缘 1px、整张 4 角合计 ≤ 十几像素；容差 16px/1.0px 相对红值(272/4.0) 留约 20 倍余量，抗跨驱动 AA 差异又不放行部分修复。

**(b) 白盒几何 / 防「关掉预测装干净」**（恒跑，这是最硬的门）：
- interval16 闭合框：`#is_predicted 盘心越框 == 0` 且 `越框最大距离 == 0`（**当前 50 个 / 9px → 修后 0**）。
- interval0 闭合框：`#is_predicted 总 == 0`（**当前 ~110 个、含 27 越框 → 修后 0**）。
- interval16 **直线快速段领先仍在**（防把 ON 改成不透传充干净）：冷启动直线快划（+x、1500px/s、60Hz、≥30 点）下，尾段一次 `Predict()` 产出非空，且存在 `is_predicted` 点满足 `x > last_real.x`、`t_us > last_real.t_us`，领先总长落在 `[0.4, 1.2]·v·interval`（防过度外推回归，沿用 real_time 测试口径）。
- 可逆：同一序列先 interval16 整条再 interval0 再 interval16，前后两次 interval16 的完整输出（x/y/t_us/is_predicted 全序列）**字节级相等**。

**(c) determinism + 既有 modeler 测试零回归**：
- 白盒断言区改为双实例跑同一序列，interval16 与 interval0 输出逐字节相等。
- 回归跑通（应保持绿）：`test_stroke_predictor`、`test_stroke_predictor_real_time`、`test_modeler_deterministic`、`test_determinism`、`test_modeler_stroke_renders`、`test_modeler_param_changes_output`（100ms vs 1ms 的 diff>0：低曲率段仍大量发预测，预计不回归，需实跑确认）、`test_engine`、`test_continuous_input_regression`、`test_ownership_loop`、`test_flush_throttle_engine`。

**实现顺序（先红后绿）**：只写回归（① 的临时 repro 即红）→ `ctest -R test_predictor_corner_clean` 确认红 → 落机制 B → 落机制 A → 全绿。禁止用「关掉整个预测/不透传」充当干净：机制 B 只作用于 interval<=0，机制 A 只抑制高滞后角，(b) 的直线领先断言专门挡这条。

## ④ 影响面核对

- **engine 对 Predict 输出消费**：`core/engine.cpp` inputLoop :216-234 展开 `out`；空 `Predict` 安全（Update 恒 ≥1 真实点），屏障/`count_submission` 不变量不变；仅注释层面建议补充。无代码改动。
- **StrokeEndPredictor 路径**：机制 B 使 interval<=0 不再触发 `PredictEnd`；机制 A 使高滞后角时不触发。`end_pred_` 仍在 Update :393 更新（无害），正常 ON 的停笔点保留。
- **CLI/批处理（合成时间 / override）**：`dgcStrokeTo`/`dgcStrokeToAt` → engine，无结构改动；OFF 态展开事件数变少，flush 计数按外部提交不受影响。`dgcSetFixedTime`（override）路径真实 t_us 忽略，A/B 判据只吃几何与 kalman v（均由输入 t_us 序列确定性导出），无 wall-clock → `test_modeler_deterministic`/`test_determinism` 保持绿。
- **dgcStrokeTo(非 At) 调用方**：时间步进器路径不变（sdk_api :230-245），消费端语义（12=0/16）不变。
- **既有优化（P7-1 批量 composite / P7-2 节流 / P7-4 时间戳校准）**：本改动只在 predictor 输出端加减预测点，engine/backend/readback 全不触；A 是高曲率抑制算法修正，不是绕过。

## ⑤ 验证（无头）

单命令：`cmake --build build/host-linux --target test_predictor_corner_clean && ctest --test-dir build/host-linux -R 'test_predictor_corner_clean|test_modeler_deterministic|test_determinism|test_stroke_predictor|test_modeler_param_changes_output|test_continuous_input_regression|test_engine'`。
白盒断言为测试内置 pass/fail；离屏 PNG 五态落 `build/host-linux/tests/`（供人工核对「转角无凸点」），红→绿对照取本节 ① 表。

## ⑥ 不做
- 消费端按钮默认显示「开」但 SDK 实为 passthrough 的「按钮撒谎」（另案，需求方排除于本 SDK 计划）。
- 新增 modeler 去激活 API（需求方已明确排除；干净基线 P 只作参照，不加旁路开关）。
- 修「机制 C」（预测点被永久合墨无擦除的架构性偏差）——本计划在 predictor 端抑制外溢规避之，不引入擦除/重绘架构。
- 正小 interval（如 1ms）的强制 n=1 语义、曲线平滑度/拐角内圆弧度（spring 内凹 fillet 属 smoothing 本体，不在「外溢凸点」指标内）——本计划只管越出包络的墨。
