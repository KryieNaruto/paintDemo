# `DgcBrushSetting` 参数映射清单（D6-1）

> 对应 `sdk_api/dgc_paint_c_api.h` 的 `DgcBrushSetting` 枚举与 `dgcSetBrushSetting(ctx, brush, settingId, value)`。
> 默认值字面量来自 `core/stroke_predictor.h` 的 `StrokeModelParams`（settingId 4-12）。

## 笔刷内核基础参数（settingId 0-3，B3-1 语义，本任务不改）

| settingId | 名称 | 含义 | 默认值 | 单位 | 消费端滑杆建议范围 |
|---|---|---|---|---|---|
| 0 | `DGC_SETTING_RADIUS` | 半径 | — | px | 1 – 100 |
| 1 | `DGC_SETTING_HARDNESS` | 硬度 | — | 0-1 | 0.0 – 1.0 |
| 2 | `DGC_SETTING_OPACITY` | 不透明度 | — | 0-1 | 0.0 – 1.0 |
| 3 | `DGC_SETTING_RADIUS_LOG` | 半径（对数刻度别名） | — | px（log） | 1 – 100 |

> 注：0-3 目前仅存参（`Impl::brush_settings`），不作用于默认笔刷渲染（见任务书 D6-1 范围说明），
> 调试面板可保留控件但不承诺画面变化；本任务的「改参后笔迹明显变化」验收由下表 4-12 项承载。

## Stroke Modeler 参数（settingId 4-12，D6-1 新增，透传到 `core/stroke_predictor.h`）

惰性激活：`dgcCreate` 默认不创建/注入预测器（passthrough，零回归）；首次对以下任一
settingId 调用 `dgcSetBrushSetting`，SDK 才会 `make_unique<StrokeModeler>()` 并注入引擎，
之后每次调用仅 `Configure` 刷新参数。**激活后对新提交的笔画点生效**（生产约定：面板在
`strokeActive == false` 时提交，见风险 R7）。

| settingId | 名称 | 含义 | 默认值 | 单位 | 消费端滑杆建议范围 | 改参效果（人工可辨） |
|---|---|---|---|---|---|---|
| 4 | `DGC_SETTING_WOBBLE_TIMEOUT_MS` | 抖动消除超时（低通时间常数） | 40.0 | ms | 0 – 200 | 越大越平滑但越迟滞跟手 |
| 5 | `DGC_SETTING_WOBBLE_SPEED_FLOOR` | 抖动消除最低速度（静止判定阈值） | 1.31 | mm/s | 0 – 10 | 越大越容易判定为「静止抖动」而被压平 |
| 6 | `DGC_SETTING_MIN_OUTPUT_RATE_HZ` | 最小输出采样率（上采样密度下限） | 180.0 | Hz | 20 – 500 | 越大补点越密、曲线越平滑，也决定预测点间距 |
| 7 | `DGC_SETTING_END_OF_STROKE_STOPPING_DISTANCE_MM` | 抬笔停止距离（停笔点判定阈值） | 0.1 | mm | 0.01 – 5 | 越大末端预测点越倾向继续外推 |
| 8 | `DGC_SETTING_SPRING_MASS_CONSTANT` | 弹簧质量常量 K/m | 400.0 | 1/s² | 10 – 2000 | 越大响应越快、越跟手 |
| 9 | `DGC_SETTING_SPRING_DRAG_CONSTANT` | 弹簧阻尼常量 C/m | 40.0 | 1/s | 1 – 200 | 越大抑制过冲越强、运动越「粘滞」 |
| 10 | `DGC_SETTING_KALMAN_PROCESS_NOISE` | 卡尔曼过程噪声 | 0.0005 | — | 0.00001 – 0.01 | 越大越信任最新输入，速度估计更灵敏但更抖 |
| 11 | `DGC_SETTING_KALMAN_MEASUREMENT_NOISE` | 卡尔曼测量噪声 | 0.004 | — | 0.0001 – 0.1 | 越大越不信任单次量测，估计速度越平滑但滞后 |
| 12 | `DGC_SETTING_PREDICTION_INTERVAL_MS` | 预测间隔（Predict 外推时长） | 16.0 | ms | 0 – 100 | 越大预测点越远（越易见"抢跑"漂移） |

## 非法 settingId

`settingId < 0` 或 `settingId >= DGC_SETTING_COUNT`（当前 13）一律返回 `DGC_ERR_INVALID_ARG`，
并可经 `dgcGetLastError()` 取得描述串（`"invalid argument"`），不崩溃。

## 线程/时序约定

- 4-12 项为 **context 级单例**（与传入的 `brush` 句柄具体值无关，仅要求非
  `DGC_INVALID_BRUSH` 以保持 C ABI 语义一致）。
- 生产建议：调试面板在 `strokeActive == false`（两笔画之间）时提交设置调用，
  避免笔画中途改参只影响后续点、不回溯当前笔画（见 D6-1 计划风险 R7，任务书允许
  「对新笔画（或实时）生效」）。
