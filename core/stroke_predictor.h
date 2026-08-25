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
struct StrokeModelParams {
    // wobble smoothing：低通时间常数 / 判静止速度下限（mm/s）
    float wobble_timeout_ms  = 40.0f;
    float wobble_speed_floor = 1.31f;

    // 重采样：固定 min_output_rate 上采样（保证输出点密度下限）
    float min_output_rate_hz = 180.0f;
    float end_of_stroke_stopping_distance_mm = 0.1f;

    // 弹簧质点（位置模型）：K/m（1/s²）与 C/m（1/s）。
    // 注：plan 草稿给的 1.0/20.0 会得到约 20 s 的响应时间常数（模型几乎不跟踪输入），
    //     此处按「绘画场景」调为临界阻尼（ζ=C/(2√K)=1，ωn=√K≈20 rad/s → 约 50 ms
    //     跟踪），既明显压制 100 Hz 级手抖，又能在百毫秒级笔划内跟上输入。
    float spring_mass_constant = 400.0f;
    float spring_drag_constant = 40.0f;

    // Kalman 预测：恒定速度模型的过程/量测噪声
    float kalman_process_noise   = 0.0005f;
    float kalman_measurement_noise = 0.004f;

    // 外推时长：Predict() 沿速度方向外推 future_ms 内、按 min_output_rate 布点
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
