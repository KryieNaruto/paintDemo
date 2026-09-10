// bugfix-prediction-underscale 正式回归（TDD 先红后绿）：稳态匀速段的外推前移量必须
// 贴着 |v_kalman|·interval，不得被「每样本瞬时弦速度」压到其之下。
//
// 根因（core/stroke_predictor.cpp Predict() 减速保护支路）：
//     const double nk = Norm2(v_kalman);
//     const double nt = Norm2(v_true);   // v_true = 一个重采样间隔的瞬时弦速度
//     if (nt < nk) { v_pred = v_true; }  // 本意「只在减速/停笔时走」
// v_true 是**单次采样间隔**的弦速度，v_kalman 是卡尔曼平滑速度。稳态匀速段两者本应相等，
// 但只要样本时间戳/位置带一点抖动（真机必然），|v_true| 就会在 |v_kalman| 上下摆动 →
// 该分支**在匀速段约一半样本恒命中**，把 v_pred 压到弦速度之下（真机实测 v_pred/|v_kalman|
// 中位数 0.88，抑制 ≈12–14%），外推距离系统性不足。这正是「预测间隔调大后直线段仍不领先」
// 的成因之一。
//
// 断言分层（纯 CPU、无 GPU、确定性）：
//   (A) 稳态前移量：带时间戳抖动的匀速直线，其 lead / (v·interval) 均值 ≥ kMinRatio。
//       修复前 ≈0.912（红），修复后 ≈0.99（绿）。
//   (B) 抖动不敏感（自校准，不吃卡尔曼稳态偏置）：同一速度下「带抖动」的均值不得比
//       「理想无抖动」低超过 kMaxDegrade。修复前 0.912 vs 0.999 → 红。
//   (C) 守卫（防「为干净而把预测关掉」）：稳态段始终产出 is_predicted 领先点。
//
// 防过冲能力由既有 test_predictor_decel_clean / test_predictor_corner_clean /
// test_predictor_curve_overshoot 承担（本文件只加「不许压低」这一侧的门）。
//
// 无 gtest 依赖，main 返回失败计数（仿 test_predictor_decel_clean.cpp）。
#include "core/stroke_predictor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s\n", name);                      \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

namespace {

constexpr double kMinRatio = 0.96;     // (A) 稳态前移量下限（占 v·interval）
constexpr double kMaxDegrade = 0.05;   // (B) 抖动相对理想的最大劣化
constexpr int kSkipWarmup = 100;       // 卡尔曼收敛前的暂态不计入统计
constexpr double kV = 2000.0;          // px/s
constexpr double kHz = 60.0;
constexpr float kIntervalMs = 16.0f;

struct Samp {
    float x, y;
    std::uint64_t t_us;
};

// 确定性伪随机（固定种子 LCG）。测试自带，非引擎 RNG（引擎不读随机源，见
// core/stroke_predictor.cpp 确定性约束），故不影响「同输入同输出」契约。
struct Lcg {
    std::uint32_t s;
    double next() {
        s = s * 1103515245u + 12345u;
        return double((s >> 16) & 0x7fff) / 32767.0 - 0.5;  // [-0.5, 0.5]
    }
};

// 匀速直线档案。jit=true 时叠加真机式采样抖动：位置 ±0.6px、时间戳 ±0.2·dt。
std::vector<Samp> MakeSteadyLine(bool jit, int n = 400) {
    const std::uint64_t dt = std::uint64_t(1e6 / kHz + 0.5);
    Lcg rng{12345u};
    std::vector<Samp> out;
    double x = 0.0;
    std::uint64_t t = 1000000u;
    for (int i = 0; i < n; ++i) {
        Samp s;
        s.x = float(x + (jit ? rng.next() * 1.2 : 0.0));
        s.y = float(300.0 + (jit ? rng.next() * 1.2 : 0.0));
        s.t_us = t + (jit ? std::uint64_t((rng.next() + 0.5) * 0.4 * double(dt)) : 0u);
        out.push_back(s);
        t += dt;
        x += kV / kHz;
    }
    return out;
}

struct SteadyStat {
    double mean_ratio = -1.0;  // 稳态窗口内 mean(lead / (v·interval))
    bool lead_seen = false;    // 守卫 C：稳态段始终有领先点
};

// 理想（无抖动无平滑偏置）外推前移量：Predict 取 n = floor(interval/period) 个点，
// 最远点 dt = n·period → lead = v·n·period。与 Predict 内 n 计算同式。
double IdealLead() {
    const double period_us = 1e6 / 180.0;  // StrokeModelParams::min_output_rate_hz 缺省
    const double interval_us = double(kIntervalMs) * 1000.0;
    std::uint64_t n = std::uint64_t(interval_us / period_us);
    if (n == 0) {
        n = 1;
    }
    return kV * (double(n) * period_us) / 1e6;
}

SteadyStat Trace(const std::vector<Samp>& seq) {
    StrokeModelParams par;
    par.prediction_interval_ms = kIntervalMs;
    StrokeModeler m;
    m.Configure(par);

    const double ideal = IdealLead();
    SteadyStat st;
    double sum = 0.0;
    int cnt = 0;
    int i = 0;
    for (const Samp& s : seq) {
        StrokePoint raw{};
        raw.x = s.x;
        raw.y = s.y;
        raw.pressure = 0.5f;
        raw.t_us = s.t_us;
        raw.is_predicted = false;
        std::vector<StrokePoint> real;
        m.Update(raw, &real);
        std::vector<StrokePoint> step;
        m.Predict(&step);

        double last_real_x = 0.0;
        std::uint64_t last_real_t = 0;
        bool has_real = false;
        for (const auto& r : real) {
            if (!r.is_predicted) {
                last_real_x = r.x;
                last_real_t = r.t_us;
                has_real = true;
            }
        }
        double lead = 0.0;
        for (const auto& p : step) {
            if (p.is_predicted && has_real) {
                const double d = std::fabs(double(p.x) - last_real_x);
                if (d > lead) {
                    lead = d;
                }
                if (p.x > last_real_x && p.t_us > last_real_t) {
                    st.lead_seen = true;
                }
            }
        }
        if (i >= kSkipWarmup) {
            sum += lead / ideal;
            ++cnt;
        }
        ++i;
    }
    if (cnt > 0) {
        st.mean_ratio = sum / double(cnt);
    }
    return st;
}

}  // namespace

int main() {
    const SteadyStat jit = Trace(MakeSteadyLine(true));
    const SteadyStat ideal = Trace(MakeSteadyLine(false));

    std::fprintf(stderr,
                 "[steady] ideal_lead=%.3fpx  nojit_mean_ratio=%.4f  "
                 "jit_mean_ratio=%.4f  degrade=%.4f  jit_lead_seen=%d\n",
                 IdealLead(), ideal.mean_ratio, jit.mean_ratio,
                 ideal.mean_ratio - jit.mean_ratio, int(jit.lead_seen));

    // (A) 稳态前移量不得明显低于 v·interval。
    //     修复前 jit_mean_ratio ≈ 0.912（红）；修复后 ≈ 0.990（绿）。
    CHECK(jit.mean_ratio >= kMinRatio,
          "[steady] jittered steady-line extrapolation lead >= 96% of |v|*interval");

    // (B) 抖动敏感度：稳态前移量不得因采样抖动而劣化（自校准，不吃卡尔曼稳态偏置）。
    //     修复前 degrade ≈ 0.087（红）；修复后 ≈ 0.005（绿）。
    CHECK(ideal.mean_ratio - jit.mean_ratio <= kMaxDegrade,
          "[steady] extrapolation lead insensitive to per-sample timestamp jitter");

    // (C) 守卫：预测不得被关成「干净」——稳态段必须仍有领先点。
    CHECK(jit.lead_seen, "[steady] steady-line lead still emitted");

    if (failures == 0) {
        std::fprintf(stderr, "[test_predictor_steady_no_underscale] PASS\n");
    } else {
        std::fprintf(stderr, "[test_predictor_steady_no_underscale] FAILED (%d)\n",
                     failures);
    }
    return failures == 0 ? 0 : 1;
}
