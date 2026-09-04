# bugfix · 预测开启时曲线弧外瞬态毛边（绘制中「飞出→被清掉」）

> 状态：实施计划（待审阅门 ≥80 → 修复 → 测试门 100）。
> 触发：A8-2 真机预验证——`merge_spv.h.in` 崩溃已修（e3e7133），预测开启 + interval=30ms 下
> 抬笔后无毛边，但**绘制过程中**肉眼仍能捕捉到毛边飞出然后被迅速清掉（wet-tip 层每次预测批
> 先清后画的瞬态行为），且用户质疑「是否 stroker 参数没调好导致不跟手」。

## 1. 根因（实证，非猜测）

**症状**：预测开启（interval=30ms，消费端下发）画中等曲率曲线时，预测尖沿外推方向飞离曲线
弧外（径向越界），下一真实点到达即被 wet-tip 层清掉 → 瞬态毛边。

**复现**（无头、纯 CPU、确定性）：`tests/test_predictor_curve_overshoot.cpp`（新增）——圆弧
（R=60/120/240px @ v=1000px/s @ 60Hz）逐点 `Update+Predict`（同 engine inputLoop），测量预测点
相对其外推基准真实点的径向凸出 `fringe = dist(center, pred) − dist(center, base)`。

实测（修复前，红）：

| R | fringe | 外推速度幅值 | 与圆周切线夹角 |
|---|---|---|---|
| 60 | 8.98 px | 528 px/s | 31.3° |
| 120 | 13.59 px | 745 px/s | 37.2° |
| 240 | 16.23 px | 872 px/s | 39.9° |

**根因**（`core/stroke_predictor.cpp` `Predict()`）：外推速度 `v_pred` 默认取**卡尔曼估计速度
`v_kalman` 的方向**（`stroke_predictor.cpp:454-467`，减速段才换 `v_true`）。圆弧上卡尔曼速度是
恒定速度模型对各轴正弦位置量测的估计，其**方向滞后圆周切线 ~30–40°**（相位滞后），幅值也被
衰减（528–872 px/s，输入 1000 px/s）。沿这个滞后方向外推 `interval=30ms`，把预测尖横向推离
曲线 `≈ |v|·interval·sin(lag) ≈ 9–16 px` → 毛边。机制 A（`stroke_predictor.cpp:496-507`）的 40°
硬截止正好卡在这个滞后角附近，中等曲率（lagDeg<40°）被漏放。

**影响面**：`Predict()` 是唯一的预测外推出口，被 engine `inputLoop`（`core/engine.cpp:222`）在
每个真实输入点后调用。影响所有「预测开启 + 曲线」路径；直线段 `last−prev` 与 `v_kalman` 同向，
不受影响；`is_predicted` 点只进 wet-tip 层（A8-2），不影响导出/落笔墨。

## 2. 修复方案

外推**方向**改用「最近真实行进方向」（弦 `last_output_ − prev_output_`，≈ 圆周切线，仅落后
半个采样转角 ~2–8°），**幅值**保留减速守卫（`min(|v_kalman|, |v_true|)`，不变）。即在
`Predict()` 里把

```cpp
Vec2 v_pred = v_kalman;
if (has_prev_) { …v_true… if (|v_true| < |v_kalman|) v_pred = v_true; }
```

改为「取 `v_true` 的方向 + `min(|v_kalman|,|v_true|)` 的幅值」：

```cpp
Vec2 v_pred = v_kalman;
if (has_prev_) { …v_true… if (|v_true| > 0) v_pred = v_true * (min(|v_kalman|,|v_true|) / |v_true|); }
```

等价语义：减速段（`|v_true|<|v_kalman|`）`v_pred = v_true`（与现状逐位一致）；稳态直线/圆弧
`v_pred = 弦方向 · |v_kalman|`。直线段弦方向=卡尔曼方向 → 零变化；圆弧段方向纠正 → fringe 从
9–16px 压到 `|v_kalman|·interval·sin(≈2–8°) ≈ 0.9–2.2px`。

机制 A（夹角硬门，`stroke_predictor.cpp:496-507`）**保留不变**：它用 `v_kalman` 暴露高曲率转角
（90° 拐角仍由它抑制），方向纠正不改变拐角处「弦 vs 卡尔曼」的夹角判据。

**不做**（记录为后续，不进本次）：卡尔曼速度幅值衰减（曲线上 |v_kalman| 偏低 → 预测领先偏短、
曲线处仍略不跟手）是独立问题，本次只修「方向滞后」导致的毛边；弧预测（三点拟合圆弧沿弧外推）
是更大改动，留作后续任务评估。

## 3. 回归用例设计（TDD 先红后绿）

新增 `tests/test_predictor_curve_overshoot.cpp`（纯 CPU 白盒，无条件注册，仿
`test_predictor_decel_clean.cpp`）：

- **（A）核心（先红）**：R ∈ {60, 120, 240} @ v=1000px/s @ 30ms 圆弧 trace，所有 is_predicted 点
  相对外推基准的径向凸出 `fringe ≤ 3px`。修复前 8.98/13.59/16.23px 红；修复后 ≈0.9–2.2px 绿。
- **（B）守卫**：同一 trace 仍产出领先预测点（`pred_total>0` 且 `lead_seen`），防「关掉预测装干净」。

**先红后绿步骤**：先注册并构建本测试 → 运行确认 3 项 `fringe<=3` 全 FAIL（红）→ 应用 §2 修复 →
重新构建运行确认全 PASS（绿）→ 跑既有 `test_predictor_corner_clean` / `test_predictor_decel_clean`
/ `test_stroke_predictor` / `test_stroke_predictor_real_time` 确认零回归。

## 4. 影响面核对

- `core/stroke_predictor.cpp` `Predict()`：仅改外推方向来源，不动 `Update()`、机制 A、机制 B、
  `StrokeEndPredictor`、Configure/Reset。
- 直线段行为逐位不变（弦=卡尔曼方向）→ 既有 `test_predictor_corner_clean`（门 4 直线领先）、
  `test_predictor_decel_clean`（直线减速）应零回归。
- 减速段 `v_pred = v_true` 与现状一致 → decel 守卫零回归。
- 大半径弧（R=1500，corner_clean 门 6）：弦方向≈切线，几乎无变化 → 领先仍在。
- 确定性：方向来源仍是确定性状态（`last_output_`/`prev_output_`），无 RNG/时钟 → 逐位复现不变。
- 只影响 is_predicted 点（wet-tip 层），导出/落笔墨不涉及。

## 5. 验证方式（无头 CLI + 离屏）

- 白盒（本测试）为纯 CPU，`ctest -R test_predictor_curve_overshoot` 即 CLI 复现入口（已红）。
- 涉及渲染的「真机曲线无毛边」是 A8-2 人工验收项；本 bugfix 的 host 可验证替代 = 白盒 fringe
  硬门 + 全量 ctest 绿。离屏 PNG 证据不新增（fringe 是纯几何量，白盒已量化；真机目验由用户在
  §7 handoff 复测）。
- 全量 `ctest`（host-verify）0 失败 0 跳过。

## 6. 改动文件清单

| 文件 | 改动 |
|---|---|
| `core/stroke_predictor.cpp` | `Predict()` 外推方向改用弦方向（幅值保留减速守卫），约 3 行 |
| `tests/test_predictor_curve_overshoot.cpp`（新增） | 圆弧 fringe 白盒回归（A 核心 + B 守卫） |
| `tests/CMakeLists.txt` | 注册 `test_predictor_curve_overshoot` |

## 7. 真机 handoff

消费端（paint-android `a8-2-preverify`）已下发 interval=30ms 并接 wet-tip 层。本修复落地后，
用户复测：预测开启画中等曲率曲线 → 绘制中**无**毛边飞出；直线段笔尖仍领先跟手。若不跟手仍
明显，回到 latency 维度（§2「不做」的卡尔曼幅值衰减 / readback 延迟，属 A8-3 范畴）。
