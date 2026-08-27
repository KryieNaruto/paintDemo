#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/types.h"  // StrokePoint（已含 t_us / is_predicted，B1-1 落地）

// ─────────────────────────────────────────────────────────────────────────────
// B1-5 · 白盒移植 Ink Stroke Modeler → core/stroke_predictor（SDK 内部，不进 C ABI）
//
// 把 Google ink-stroke-modeler 的平滑预测算法白盒重写进 SDK 内核（跨平台纯 C++，
// 不依赖 Android 输入管线、不引入 Abseil），对外不暴露任何新 C ABI 类型。
//
// 管线（Update）：WobbleSmoother（低通滤波 + 时间变权移动平均）
//              → Resampler（min_output_rate 上采样）
//              → PositionModeler（弹簧质点 + 固定步长欧拉积分）
//              → KalmanPredictor（恒定速度卡尔曼，估计速度）
// 管线（Predict）：沿卡尔曼速度外推 + StrokeEndPredictor（停笔点估计）
//
// 确定性：时间源只读 StrokePoint.t_us（B1-7 FixedTimeStepper 在 C API 侧填好），
//         不读真实时钟（wall clock）、不吃 RNG，输出可逐位复现。
// 所有权：经典 Pimpl，唯一堆所有权令牌为 impl_（std::unique_ptr），无裸 new/delete。
// ─────────────────────────────────────────────────────────────────────────────

// 调参集合（镜像 ink SamplingParams + StrokeModelParams 的绘画场景子集）。
//
// D6-1：以下 9 个字段均为消费端调试面板可调的 stroke modeler 参数（经
// `DgcBrushSetting` 的 DGC_SETTING_WOBBLE_TIMEOUT_MS .. DGC_SETTING_PREDICTION_INTERVAL_MS
// 透传，见 sdk_api/dgc_paint_c_api.h 与 docs/brush_settings_mapping.md）。
struct StrokeModelParams {
    // wobble smoothing（抖动消除）：低通滤波的时间常数。
    // 物理/手感含义：越大，输入被平滑的窗口越长，抖动压制越强但跟手感越迟滞
    // （数值上是 WobbleSmoother 一阶低通 alpha = dt/(dt+timeout_s) 里的 timeout_s）。
    // 单位：毫秒（ms）。
    float wobble_timeout_ms  = 40.0f;

    // wobble smoothing（抖动消除）：判定「静止/微动」的速度下限。
    // 物理/手感含义：瞬时速度低于此阈值即视为停笔抖动，走 dwell 累积平均分支
    // （压住手抖）；高于阈值走运动分支（走低通，尽量跟手）。
    // 单位：毫米/秒（mm/s）。
    float wobble_speed_floor = 1.31f;

    // 重采样：固定 min_output_rate 上采样，保证输出点密度下限。
    // 物理/手感含义：越大，稀疏输入（快速划动/低采样率设备）被补点越密，笔迹
    // 曲线越平滑；同时决定 Predict() 外推点的时间间隔（period = 1/rate）。
    // 单位：赫兹（Hz）。
    float min_output_rate_hz = 180.0f;

    // 抬笔停止距离：StrokeEndPredictor 判定「停笔点」的位移阈值。
    // 物理/手感含义：沿当前速度方向按阻尼衰减估算的剩余位移若小于该值，视为
    // 已停笔（末端预测点直接取当前位置，不再外推），越大越倾向于继续外推末端。
    // 单位：毫米（mm）。
    float end_of_stroke_stopping_distance_mm = 0.1f;

    // 弹簧质点（位置模型）：弹簧刚度 K 与质量 m 的比值 K/m。
    // 物理/手感含义：越大，弹簧把「目标位置（输入点）」拉向自身的力越强、响应
    // 越快、越跟手；越小则跟踪越迟滞、笔迹越平滑但滞后感越明显。
    // 注：plan 草稿给的 1.0/20.0 会得到约 20 s 的响应时间常数（模型几乎不跟踪输入），
    //     此处按「绘画场景」调为临界阻尼（ζ=C/(2√K)=1，ωn=√K≈20 rad/s → 约 50 ms
    //     跟踪），既明显压制 100 Hz 级手抖，又能在百毫秒级笔划内跟上输入。
    // 单位：1/秒²（1/s²）。
    float spring_mass_constant = 400.0f;

    // 弹簧质点（位置模型）：阻尼系数 C 与质量 m 的比值 C/m。
    // 物理/手感含义：越大，弹簧运动衰减越快（类似「粘滞阻力」），抑制过冲/震荡；
    // 与 spring_mass_constant 共同决定阻尼比 ζ=C/(2√K)，同时复用作
    // StrokeEndPredictor 的位移衰减率。
    // 单位：1/秒（1/s）。
    float spring_drag_constant = 40.0f;

    // Kalman 预测：恒定速度模型的过程噪声（状态转移不确定性）。
    // 物理/手感含义：越大，卡尔曼滤波越信任「最新量测」而非历史轨迹，估计速度
    // 对手部动作变化的反应越灵敏（但也更易受噪声干扰、外推抖动更大）。
    float kalman_process_noise   = 0.0005f;

    // Kalman 预测：位置量测噪声（传感器/输入点抖动的不确定性）。
    // 物理/手感含义：越大，卡尔曼滤波越不信任单次量测、估计速度越平滑（但对
    // 真实加减速的响应越滞后），用于压制输入设备本身的量测抖动。
    float kalman_measurement_noise = 0.004f;

    // 外推时长：Predict() 沿卡尔曼估计速度方向外推的时间窗口，按
    // min_output_rate 布点，越大预测点越远、越靠前遮盖输入延迟，但笔画结束/
    // 转向时被真实点覆盖修正的幅度也越大（越容易"抢跑"看到明显预测漂移）。
    // 单位：毫秒（ms）。
    float prediction_interval_ms = 16.0f;
};

// 轻量 Result<T>：Abseil StatusOr 的 C++20 替代（std::expected 需 C++23，
// 本仓库 CMAKE_CXX_STANDARD 20）。value + ok，无异常跨 ABI 语义。
template <typename T>
struct Result {
    T    value{};
    bool ok = false;
};

// 平滑 + 预测器（SDK 内部类，镜像 ink_stroke_modeler::StrokeModeler 的 API）。
class StrokeModeler {
public:
    StrokeModeler();
    ~StrokeModeler();
    StrokeModeler(const StrokeModeler&) = delete;
    StrokeModeler& operator=(const StrokeModeler&) = delete;

    // 调参（可选；缺省用 StrokeModelParams 默认值）。会 Reset()。
    void Configure(const StrokeModelParams& params);

    // 清空内部缓冲 / 弹簧 / 卡尔曼 / 预测挂起缓冲（Begin/EndStroke 时调用）。
    void Reset();

    // 输入一个原始真实点（is_predicted=false），追加平滑后的真实点到 *out
    // （is_predicted=false）。每次 Update 以真实输入为准重做平滑并推进
    // last_output_（Predict 的外推基准），使「真实点到达即覆盖同段预测点」由
    // 结构显式承载：Update 只发真实点，Predict 只从最近真实点外推。
    void Update(const StrokePoint& raw, std::vector<StrokePoint>* out);

    // 沿当前速度外推未来点，追加预测点到 *out（is_predicted=true）。
    // 无状态（尚未 Update）时产出为空。
    void Predict(std::vector<StrokePoint>* out);

private:
    struct Impl;                       // 内部组件与状态全藏在 .cpp
    std::unique_ptr<Impl> impl_;       // RAII 持有，无裸 new/delete
};
