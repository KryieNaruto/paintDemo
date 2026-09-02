#include "core/stroke_predictor.h"

#include <algorithm>
#include <cmath>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// 白盒移植 Ink Stroke Modeler 的内部组件（全部在匿名命名空间，值成员 + RAII）。
//
// 管线（Update）：WobbleSmoother → Resampler → PositionModeler → KalmanPredictor。
// 管线（Predict）：沿卡尔曼速度均匀外推 + StrokeEndPredictor 停笔点。
//
// 确定性约束：
//  - 时间源只读 StrokePoint.t_us，不读真实时钟（wall clock / 系统时钟）；
//  - 无任何 RNG（不吃 core/determinism.h 的随机源）；
//  - 欧拉积分固定微步 kPositionStepS（1 ms），输出可逐位复现。
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

// ── WobbleSmoother ──────────────────────────────────────────────────────────
// 镜像 ink_stroke_modeler::WobbleSmoother：低通滤波（运动）→ 时间变权移动平均
// （静止 dwell 分支累积平均）。抑制手抖（高频分量）。
struct WobbleSmoother {
    void Reset() {
        has_ = false;
        dwell_count_ = 0;
        dwell_sum_ = {};
    }
    void Configure(float timeout_ms, float speed_floor) {
        timeout_s_ = timeout_ms / 1000.0f;
        speed_floor_ = speed_floor;
    }

    StrokePoint Smooth(const StrokePoint& in) {
        if (!has_) {
            last_ = in;
            has_ = true;
            return in;
        }
        const double dt_s = double(in.t_us - last_.t_us) / 1e6;
        const double dx = double(in.x) - last_.x;
        const double dy = double(in.y) - last_.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const double speed = dt_s > 0.0 ? dist / dt_s : 0.0;  // mm/s

        StrokePoint out = in;
        if (speed < speed_floor_) {
            // 静止：dwell 累积平均（时间变权移动平均的静止分支）。
            dwell_sum_.x += in.x;
            dwell_sum_.y += in.y;
            dwell_sum_.pressure += in.pressure;
            dwell_sum_.tilt_x += in.tilt_x;
            dwell_sum_.tilt_y += in.tilt_y;
            ++dwell_count_;
            out.x = dwell_sum_.x / float(dwell_count_);
            out.y = dwell_sum_.y / float(dwell_count_);
            out.pressure = dwell_sum_.pressure / float(dwell_count_);
            out.tilt_x = dwell_sum_.tilt_x / float(dwell_count_);
            out.tilt_y = dwell_sum_.tilt_y / float(dwell_count_);
        } else {
            // 运动：一阶低通（时间常数 timeout_s_）。
            dwell_sum_ = {};
            dwell_count_ = 0;
            const double alpha = dt_s / (dt_s + timeout_s_);
            out.x = float(last_.x + alpha * (in.x - last_.x));
            out.y = float(last_.y + alpha * (in.y - last_.y));
            out.pressure = float(last_.pressure + alpha * (in.pressure - last_.pressure));
            out.tilt_x = float(last_.tilt_x + alpha * (in.tilt_x - last_.tilt_x));
            out.tilt_y = float(last_.tilt_y + alpha * (in.tilt_y - last_.tilt_y));
        }
        last_ = out;
        return out;
    }

    StrokePoint last_{};
    bool has_ = false;
    int dwell_count_ = 0;
    StrokePoint dwell_sum_{};
    float timeout_s_ = 0.04f;
    float speed_floor_ = 1.31f;
};

// ── Resampler ───────────────────────────────────────────────────────────────
// 镜像 SamplingParams 的 min_output_rate：固定上采样，保证输出点密度下限。
// 输入已更密则原样透传；稀疏时在相邻点间等距插值补点。
struct Resampler {
    void Reset() { has_ = false; }
    void Configure(float min_output_rate_hz) {
        period_us_ = std::uint64_t(1e6 / min_output_rate_hz + 0.5);
        if (period_us_ == 0) {
            period_us_ = 1;
        }
    }

    void Push(const StrokePoint& p, std::vector<StrokePoint>* out) {
        if (!has_) {
            last_ = p;
            has_ = true;
            out->push_back(p);
            return;
        }
        const std::uint64_t dt =
            (p.t_us >= last_.t_us) ? (p.t_us - last_.t_us) : 0;
        if (dt <= period_us_) {
            last_ = p;
            out->push_back(p);
            return;
        }
        // 稀疏：从 last_ 到 p 之间等距补点（k=1..n；k=n 恰落在 p 时不再补 p）。
        const std::uint64_t n = dt / period_us_;
        for (std::uint64_t k = 1; k <= n; ++k) {
            const double f = double(k * period_us_) / double(dt);
            StrokePoint ip = p;
            ip.x = float(last_.x + (p.x - last_.x) * f);
            ip.y = float(last_.y + (p.y - last_.y) * f);
            ip.pressure = float(last_.pressure + (p.pressure - last_.pressure) * f);
            ip.tilt_x = float(last_.tilt_x + (p.tilt_x - last_.tilt_x) * f);
            ip.tilt_y = float(last_.tilt_y + (p.tilt_y - last_.tilt_y) * f);
            ip.t_us = last_.t_us + k * period_us_;
            ip.is_predicted = false;
            out->push_back(ip);
        }
        if (p.t_us != last_.t_us + n * period_us_) {
            out->push_back(p);
        }
        last_ = p;
    }

    StrokePoint last_{};
    std::uint64_t period_us_ = 5555;
    bool has_ = false;
};

// ── PositionModeler ─────────────────────────────────────────────────────────
// 镜像 ink_stroke_modeler::PositionModeler：弹簧质点（二阶）模型，固定微步
// 欧拉积分（半隐式）。加速度 = K/m·(target - pos) - C/m·vel；步长 kStepS 锁定。
struct PositionModeler {
    static constexpr double kStepS = 0.001;  // 1 ms 固定微步（锁定，不读真实时钟）

    void Reset() { has_ = false; }
    void Configure(float k, float c) {
        k_ = k;
        c_ = c;
    }

    StrokePoint Update(const StrokePoint& target) {
        if (!has_) {
            pos_x_ = target.x;
            pos_y_ = target.y;
            vel_x_ = 0.0;
            vel_y_ = 0.0;
            last_t_us_ = target.t_us;
            has_ = true;
            return target;
        }
        // bugfix（P7-4）：真实时间戳可能乱序/倒退，uint64 相减会下溢成巨大正数，
        // 使下方 while 以 kStepS 步长积分巨大 dt → 实际死循环。先按 dt=0 归一
        // （触发 integrate_dt<=0 → kStepS 防御分支），与 Resampler 的 >= 口径一致。
        const double dt_s = (target.t_us >= last_t_us_)
            ? double(target.t_us - last_t_us_) / 1e6
            : 0.0;
        // 防御分支（bugfix Fix B 备选）：dt<=0（全 0/负时间戳，本应被 C API 的 Fix A 消除）
        // 时按一个固定微步推进弹簧，避免把每个点钉死在首点（修复前笔画塌缩成点的根因），
        // 防未来再次出现全零时间戳时退化。dt>0 的正常路径（override/fixedtime）不变。
        double integrate_dt = dt_s;
        if (integrate_dt <= 0.0) {
            integrate_dt = kStepS;
        }
        {
            double remaining = integrate_dt;
            while (remaining > 0.0) {
                const double h = std::min(remaining, kStepS);
                const double ax = k_ * (double(target.x) - pos_x_) - c_ * vel_x_;
                const double ay = k_ * (double(target.y) - pos_y_) - c_ * vel_y_;
                vel_x_ += ax * h;
                vel_y_ += ay * h;
                pos_x_ += vel_x_ * h;
                pos_y_ += vel_y_ * h;
                remaining -= h;
            }
        }
        last_t_us_ = target.t_us;
        StrokePoint out = target;
        out.x = float(pos_x_);
        out.y = float(pos_y_);
        out.is_predicted = false;
        return out;
    }

    double pos_x_ = 0.0;
    double pos_y_ = 0.0;
    double vel_x_ = 0.0;
    double vel_y_ = 0.0;
    std::uint64_t last_t_us_ = 0;
    double k_ = 400.0;
    double c_ = 40.0;
    bool has_ = false;
};

// ── KalmanPredictor ─────────────────────────────────────────────────────────
// 镜像 ink_stroke_modeler::KalmanPredictor：每轴独立恒定速度卡尔曼（状态 [pos,
// vel]），从位置量测估计平滑速度，供 Predict 外推。确定性、无随机。
struct KalmanPredictor {
    void Reset() { has_ = false; }
    void Configure(double q, double r) {
        q_ = q;
        r_ = r;
    }

    void Update(const StrokePoint& p) {
        if (!has_) {
            for (int a = 0; a < 2; ++a) {
                x_[a][0] = (a == 0) ? p.x : p.y;
                x_[a][1] = 0.0;
                P_[a][0][0] = 1.0;
                P_[a][0][1] = 0.0;
                P_[a][1][0] = 0.0;
                P_[a][1][1] = 1.0;
            }
            last_t_us_ = p.t_us;
            has_ = true;
            return;
        }
        // bugfix（P7-4）：乱序/倒退时间戳下 uint64 相减下溢成巨大正数，会绕过下方
        // dt<=0 防御、把卡尔曼状态量推到离谱值。按 dt=0 归一走防御分支（跳过更新）。
        const double dt = (p.t_us >= last_t_us_)
            ? double(p.t_us - last_t_us_) / 1e6
            : 0.0;
        last_t_us_ = p.t_us;
        if (dt <= 0.0) {
            return;
        }
        const double dt2 = dt * dt;
        const double dt3 = dt2 * dt;
        const double dt4 = dt2 * dt2;
        for (int a = 0; a < 2; ++a) {
            const double meas = (a == 0) ? p.x : p.y;
            // predict：x = F x，F = [[1, dt], [0, 1]]
            const double xp0 = x_[a][0] + dt * x_[a][1];
            const double xp1 = x_[a][1];
            // P = F P Fᵀ + Q（恒定速度离散过程噪声）
            const double p00 =
                P_[a][0][0] + 2.0 * dt * P_[a][0][1] + dt2 * P_[a][1][1] + q_ * dt4 / 4.0;
            const double p01 = P_[a][0][1] + dt * P_[a][1][1] + q_ * dt3 / 2.0;
            const double p10 = p01;
            const double p11 = P_[a][1][1] + q_ * dt2;
            // update：H = [1, 0]
            const double S = p00 + r_;
            const double K0 = p00 / S;
            const double K1 = p10 / S;
            const double innov = meas - xp0;
            x_[a][0] = xp0 + K0 * innov;
            x_[a][1] = xp1 + K1 * innov;
            // P = (I - K H) P
            P_[a][0][0] = (1.0 - K0) * p00;
            P_[a][0][1] = (1.0 - K0) * p01;
            P_[a][1][0] = p10 - K1 * p00;
            P_[a][1][1] = p11 - K1 * p01;
        }
    }

    Vec2 velocity() const { return Vec2{x_[0][1], x_[1][1]}; }  // mm/s

    double x_[2][2]{};
    double P_[2][2][2]{};
    std::uint64_t last_t_us_ = 0;
    double q_ = 0.0005;
    double r_ = 0.004;
    bool has_ = false;
};

// ── StrokeEndPredictor ───────────────────────────────────────────────────────
// 镜像 ink_stroke_modeler::StrokeEndPredictor：假设速度沿当前方向按一阶阻尼
// 指数衰减，估计「停笔点」（速度近似归零前的剩余位移），供 Predict 末端点。
struct StrokeEndPredictor {
    void Reset() { has_ = false; }
    void Configure(float drag, float stopping_distance_mm) {
        drag_ = drag;
        stop_dist_ = stopping_distance_mm;
    }

    void Update(const StrokePoint& last, const Vec2& vel) {
        last_ = last;
        vel_ = vel;
        has_ = true;
    }

    Result<StrokePoint> PredictEnd() const {
        if (!has_) {
            return {};
        }
        const double disp_x = vel_.x / drag_;
        const double disp_y = vel_.y / drag_;
        StrokePoint end = last_;
        if (std::sqrt(disp_x * disp_x + disp_y * disp_y) >= stop_dist_) {
            end.x = float(last_.x + disp_x);
            end.y = float(last_.y + disp_y);
        }
        end.is_predicted = true;
        return {end, true};
    }

    StrokePoint last_{};
    Vec2 vel_{};
    float drag_ = 40.0f;
    float stop_dist_ = 0.1f;
    bool has_ = false;
};

}  // namespace

// ── StrokeModeler::Impl ─────────────────────────────────────────────────────
//
// D6-1 线程安全（plan §4.3 点 2）：Configure/Update/Predict/Reset 由不同线程
// 调用（C API 调用线程 setBrushSetting → Configure；引擎输入线程 → Update/
// Predict/Reset），Impl 内持一把非递归 std::mutex 保护全部内部状态。
//
// 死锁规避：Reset() 是公开入口会自行加锁；Configure() 内部语义上也要做一次
// 「清空状态」，若 Configure 加锁后再调用会加锁的 Reset() 会对同一把非递归锁
// 重入自锁死锁。故拆出私有的 ResetLocked()（假定调用方已持锁，内部不再加锁），
// Configure() 加锁一次后内联调用 ResetLocked()；公开 Reset() 加锁后调用
// ResetLocked()。
struct StrokeModeler::Impl {
    void ApplyParams() {
        wobble_.Configure(params_.wobble_timeout_ms, params_.wobble_speed_floor);
        resampler_.Configure(params_.min_output_rate_hz);
        position_.Configure(params_.spring_mass_constant, params_.spring_drag_constant);
        kalman_.Configure(params_.kalman_process_noise, params_.kalman_measurement_noise);
        end_pred_.Configure(params_.spring_drag_constant,
                            params_.end_of_stroke_stopping_distance_mm);
    }

    // 无锁内部重置：调用方必须已持有 mutex_（Configure/Reset 的公开入口负责加锁）。
    void ResetLocked() {
        wobble_.Reset();
        resampler_.Reset();
        position_.Reset();
        kalman_.Reset();
        end_pred_.Reset();
        has_output_ = false;
        last_output_ = {};
    }

    std::mutex mutex_;  // 守卫以下全部状态；四个公开方法各自加锁，互斥执行。

    StrokeModelParams params_{};
    WobbleSmoother wobble_;
    Resampler resampler_;
    PositionModeler position_;
    KalmanPredictor kalman_;
    StrokeEndPredictor end_pred_;

    // 覆盖语义由结构显式承载（无挂起预测缓冲）：Update 只发真实点并推进
    // last_output_，Predict 只从 last_output_ 重外推，故「真实点到达即覆盖同段预测点」。
    bool has_output_ = false;
    StrokePoint last_output_{};  // 最近一次 Update 产出的真实点（Predict 外推基准）
};

StrokeModeler::StrokeModeler() : impl_(std::make_unique<Impl>()) {
    impl_->ApplyParams();  // 构造期单线程，无需加锁。
}

StrokeModeler::~StrokeModeler() = default;

void StrokeModeler::Configure(const StrokeModelParams& params) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->params_ = params;
    impl_->ApplyParams();
    impl_->ResetLocked();  // 内联重置，避免对 Reset() 重入加锁自死锁。
}

void StrokeModeler::Reset() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->ResetLocked();
}

void StrokeModeler::Update(const StrokePoint& raw, std::vector<StrokePoint>* out) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    StrokePoint w = impl_->wobble_.Smooth(raw);

    std::vector<StrokePoint> resampled;
    impl_->resampler_.Push(w, &resampled);

    for (const StrokePoint& r : resampled) {
        StrokePoint pos = impl_->position_.Update(r);
        impl_->kalman_.Update(pos);
        impl_->end_pred_.Update(pos, impl_->kalman_.velocity());
        impl_->last_output_ = pos;
        impl_->has_output_ = true;
        out->push_back(pos);
    }
}

void StrokeModeler::Predict(std::vector<StrokePoint>* out) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->has_output_) {
        return;
    }
    const StrokePoint& last = impl_->last_output_;
    const Vec2 v = impl_->kalman_.velocity();

    const double period_us = 1e6 / double(impl_->params_.min_output_rate_hz);
    const double interval_us = impl_->params_.prediction_interval_ms * 1000.0;
    // 均匀外推点数：预测区间内按 min_output_rate 布点（至少 1 个）。
    std::uint64_t n = std::uint64_t(interval_us / period_us);
    if (n == 0) {
        n = 1;
    }
    for (std::uint64_t k = 1; k <= n; ++k) {
        const double dt_s = double(k) * period_us / 1e6;
        StrokePoint p = last;
        p.x = float(last.x + v.x * dt_s);
        p.y = float(last.y + v.y * dt_s);
        p.t_us = last.t_us + std::uint64_t(k * period_us);
        p.is_predicted = true;
        out->push_back(p);
    }

    // 末端：StrokeEndPredictor 的停笔点（落在均匀外推点之后）。
    const Result<StrokePoint> end = impl_->end_pred_.PredictEnd();
    if (end.ok) {
        StrokePoint p = end.value;
        p.t_us = last.t_us + std::uint64_t(interval_us);
        out->push_back(p);
    }
}
