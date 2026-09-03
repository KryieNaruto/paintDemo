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

// 跨平台 π（审阅建议 1）：M_PI 在 MSVC（无 _USE_MATH_DEFINES）与 Android bionic
// 头文件间可用性不一致；std::acos 又非标准 constexpr（MSVC 拒编）。用字面量最稳。
constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

// 模平方（减速不过冲用：比较卡尔曼 v 与最近真实速度的大小，取较小者作外推速度）。
static double Norm2(const Vec2& v) { return v.x * v.x + v.y * v.y; }

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
        has_prev_ = false;
        prev_output_ = {};
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

    // 机制 A 的极小窗口状态：最近 2 个真实输出。判据用「最近真实行进方向」=
    // last_output_ − prev_output_ 与卡尔曼 v 的夹角检测高曲率/转向（预测外溢根因）。
    // 必须随 ResetLocked() 清零（可逆/确定性）；无任何跨笔画 latch / 计数器。
    bool has_prev_ = false;
    StrokePoint prev_output_{};  // Update() 每次推真实点时滚动成前一个
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
        // 减速不过冲（bugfix-prediction-decel）：喂给 StrokeEndPredictor 的停笔点速度取
        // 「卡尔曼 v 与最近真实速度的较小者」。稳态运动两者≈相等 → 停笔点保留 v·interval
        // 量级；减速/停笔时真实位移坍缩 → 停笔点随之坍缩，不留下越出真实轨迹的永久凸尾
        // （预测点永久合墨、无擦除）。最近真实速度 = (本 pos − 前一个真实输出)/Δt；
        // Δt 乱序/0 时退化卡尔曼 v。
        const Vec2 v_k = impl_->kalman_.velocity();
        Vec2 v_eff = v_k;
        if (impl_->has_output_) {
            const std::uint64_t d = (pos.t_us >= impl_->last_output_.t_us)
                                        ? pos.t_us - impl_->last_output_.t_us
                                        : 0u;
            if (d > 0u) {
                Vec2 v_t;
                v_t.x = (double(pos.x) - double(impl_->last_output_.x)) / (double(d) / 1e6);
                v_t.y = (double(pos.y) - double(impl_->last_output_.y)) / (double(d) / 1e6);
                if (Norm2(v_t) < Norm2(v_k)) {
                    v_eff = v_t;
                }
            }
        }
        impl_->end_pred_.Update(pos, v_eff);
        if (impl_->has_output_) {  // 已存在前一个真实输出：滚动窗口供机制 A 判据用
            impl_->prev_output_ = impl_->last_output_;
            impl_->has_prev_ = true;
        }
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
    const Vec2 v_kalman = impl_->kalman_.velocity();

    // 减速不过冲（bugfix-prediction-decel）：外推速度取「卡尔曼 v 与最近真实速度的较小者」。
    // 稳态运动两者≈相等 → 保留 v·interval 领先；减速/停笔时最近真实位移坍缩 → v_pred 随之
    // 坍缩，末批预测点落在最近真实点附近，不留下越出真实轨迹的永久凸尾（预测点永久合墨、
    // 无擦除）。最近真实速度 = (last_output_ − prev_output_)/Δt；无 prev / Δt 乱序时退化
    // 卡尔曼 v。
    // 注意：外推用 v_pred，但机制 A 的转向夹角门仍用 v_kalman —— 只有滞后的卡尔曼 v 才能
    // 暴露高曲率转角（v_pred 在减速段≈真实位移方向，夹角≈0 会漏拦转角，回归凸点）。
    Vec2 v_pred = v_kalman;
    if (impl_->has_prev_) {
        const std::uint64_t d = (last.t_us >= impl_->prev_output_.t_us)
                                    ? last.t_us - impl_->prev_output_.t_us
                                    : 0u;
        if (d > 0u) {
            Vec2 v_true;
            v_true.x = (double(last.x) - double(impl_->prev_output_.x)) / (double(d) / 1e6);
            v_true.y = (double(last.y) - double(impl_->prev_output_.y)) / (double(d) / 1e6);
            if (Norm2(v_true) < Norm2(v_kalman)) {
                v_pred = v_true;
            }
        }
    }

    const double period_us = 1e6 / double(impl_->params_.min_output_rate_hz);
    const double interval_us = impl_->params_.prediction_interval_ms * 1000.0;

    // ── 机制 B（OFF，审阅建议 4：置于 has_output_ 检查之后，首点行为两态一致）──
    // interval<=0 → Predict 完全旁通：不产出任何均匀外推点，也不追加
    // StrokeEndPredictor 停笔点（旧逻辑 :411-414 的 n=0→1 与 :425-431 无条件停笔点
    // 两条路径都会被这里挡住）。Update() 不受影响（平滑照常），OFF 输出 = 纯真实
    // 平滑点。正的小 interval（如 1ms）仍走下方既有 n 计算（n=0→1 兜底保留给
    // (0, period) 正区间，不动 test_modeler_param_changes_output 的 1ms 分支预期）。
    if (interval_us <= 0.0) {
        return;
    }

    // ── 机制 A（抑制高曲率/转向处外推）──
    // 本引擎把每个预测点永久合进墨（无擦除），故任何「方向偏离真实轨迹」的预测都会
    // 在转角留下抹不掉的凸点。判据 = 卡尔曼 v 方向是否还贴着最近真实行进方向：
    //   last_output_ − prev_output_ = 最近真实位移方向 d；
    //   θ = ∠(d, v)（无符号，atan2(|d×v|, d·v)）。
    // 直线/低曲率段 v≈真实切线 → θ≈0，照常走下方 v·interval 领先；90° 转角/高曲率处
    // v 滞后真实切线 → θ>50°，本轮不产出任何预测点（uniform 外推与停笔点一起抑制）。
    // 夹角门用 v_kalman（而非外推的 v_pred）——减速段 v_pred≈真实位移方向，用它判夹角
    // 会把转角漏放成凸点（bugfix-prediction-decel 归位）。
    //
    // 注：不加「卡尔曼 |v| < 阈值」无条件早退——会在笔划起步 ~40ms（卡尔曼速度未收敛
    // 暂态）把 Predict 拦空，违反「has_output_ 后产出领先点」契约并打回 test_stroke_predictor
    // 直线暖机样本。低速/停笔时 v→0，外推点自然坍缩到最近真实点附近（延伸 ∝ v），
    // 不会留下外凸漂移点，转角凸点由下方夹角门（机制 A）+ 机制 B 负责。
    if (impl_->has_prev_) {
        const double dx = double(last.x) - double(impl_->prev_output_.x);
        const double dy = double(last.y) - double(impl_->prev_output_.y);
        if (dx != 0.0 || dy != 0.0) {
            const double cross = dx * v_kalman.y - dy * v_kalman.x;
            const double dot = dx * v_kalman.x + dy * v_kalman.y;
            const double lagDeg = std::atan2(std::fabs(cross), dot) * 180.0 / kPi;
            if (lagDeg > 40.0) {  // 启动阈值(CALIB40)；校准带 40°–60°，零越框白盒断言为门
                return;
            }
        }
    }

    // 均匀外推点数：预测区间内按 min_output_rate 布点（至少 1 个）。
    std::uint64_t n = std::uint64_t(interval_us / period_us);
    if (n == 0) {
        n = 1;
    }
    for (std::uint64_t k = 1; k <= n; ++k) {
        const double dt_s = double(k) * period_us / 1e6;
        StrokePoint p = last;
        p.x = float(last.x + v_pred.x * dt_s);
        p.y = float(last.y + v_pred.y * dt_s);
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
