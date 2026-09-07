// bugfix（TDD 先红后绿）：预测「开」在真实 device interval（30ms）下，中等曲率圆弧处
// 外推沿滞后的卡尔曼速度方向横向飞离曲线 → 预测尖越出真实轨迹（用户所见「绘制中毛边飞出，
// 然后被迅速清掉」的瞬态毛边）。
//
// 根因（见 docs/plans/bugfix-prediction-curve-overshoot.md §1）：Predict() 外推速度 v_pred
// 默认取卡尔曼速度 v_kalman 的**方向**；圆弧上卡尔曼速度方向滞后圆周切线 ~30–40°（恒定速度
// 模型对圆弧各轴正弦量测的相位滞后，幅值也被衰减）。沿滞后方向外推 interval=30ms → 预测尖
// 横向凸出 fringe ≈ |v_kalman|·interval·sin(lag) ≈ 9–16px。机制 A（8a0ee97）只按「卡尔曼 v
// vs 最近真实位移」夹角做 >40° 硬截止，正好卡在该滞后角附近 → 中等曲率（lagDeg<40°）漏放。
//
// 断言分层（纯 CPU、无 GPU、确定性）：
//   (A) 核心（先红）：若干中等曲率圆弧（R=60/120/240px @ 1000px/s @ 30ms）trace 里
//       所有 is_predicted 点相对其外推基准真实点的径向凸出 fringe ≤ 3px。修复前
//       8.98/13.59/16.23px 红；修复后（方向改弦方向）≈1.6/2.3/1.7px 绿。
//   (B) 守卫（防「为干净而关掉预测」）：同一 trace 里仍产出领先预测点（pred_total>0 且
//       存在 t_us 晚于该步末真实点的领先点）——保证修复保留延迟遮盖领先。
// 零回归由既有 test_predictor_corner_clean（门 4 直线领先 / 门 6 大弧）、
// test_predictor_decel_clean（直线减速）承担。
//
// 无 gtest 依赖，main 返回失败计数（仿 test_predictor_decel_clean.cpp 结构）。
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

constexpr double kPi = 3.14159265358979323846;

struct Samp {
    double x, y;
    std::uint64_t t_us;
};

// 圆弧：圆心 (cx,cy)、半径 R、θ0→θ1（弧度）沿弧长按 speed/hz 等距采样（60Hz）。
// 弧上各点严格落在圆周上（径向 |dist(center,p)−R| ≈ 0），供「预测点径向越界」白盒测量。
std::vector<Samp> MakeArc(double cx, double cy, double R, double theta0, double theta1,
                          double speed, double hz) {
    const std::uint64_t dt_us = std::uint64_t(1e6 / hz + 0.5);
    const double step = speed / hz;  // 每样本沿弧长的位移（px）
    std::vector<Samp> out;
    double theta = theta0;
    std::uint64_t t = 1000000u;
    int guard = 0;
    while (theta < theta1 && guard < 200000) {
        Samp p;
        p.x = cx + R * std::cos(theta);
        p.y = cy + R * std::sin(theta);
        p.t_us = t;
        out.push_back(p);
        theta += step / R;  // 沿弧长推进：Δθ = Δs / R
        t += dt_us;
        ++guard;
    }
    return out;
}

struct TraceStat {
    double pred_max_overshoot = 0.0;  // 预测尖相对外推基准真实点的最大径向凸出（px）
    double real_max_dev = 0.0;        // 真实点相对理想圆的径向偏差（诊断：平滑切角）
    int    pred_total = 0;
    bool   lead_seen = false;         // 守卫 B：存在领先预测点
    // 诊断：max-fringe 那一步的外推速度幅值 / 与圆周切线的夹角（度）。
    double diag_v_mag = 0.0;
    double diag_v_off_tangent_deg = 0.0;
};

// 预测尖毛边 = 相对其外推基准（该步末真实点，= Predict() 的 last_output_）的径向凸出：
//   fringe = dist(center, pred) − dist(center, base)
// 圆弧上切线外推的凸出 ≈ (v·interval)²/(2R)，与「平滑切角」解耦（后者同时作用于 base 与
// 真实曲线，不进入 fringe）。base 取该步 Update 产出的最后一个真实点。
TraceStat TraceArc(const std::vector<Samp>& seq, double cx, double cy, double R,
                   float interval_ms) {
    StrokeModelParams par;
    par.prediction_interval_ms = interval_ms;
    StrokeModeler m;
    m.Configure(par);
    TraceStat st;
    for (const Samp& s : seq) {
        StrokePoint raw{};
        raw.x = float(s.x);
        raw.y = float(s.y);
        raw.pressure = 0.5f;
        raw.t_us = s.t_us;
        raw.is_predicted = false;
        std::vector<StrokePoint> real;
        m.Update(raw, &real);
        std::vector<StrokePoint> step;
        m.Predict(&step);
        double base_x = 0.0, base_y = 0.0;
        std::uint64_t base_t = 0;
        bool has_real = false;
        for (const auto& r : real) {
            if (!r.is_predicted) {
                base_x = r.x;
                base_y = r.y;
                base_t = r.t_us;
                has_real = true;
                const double d = std::fabs(
                    std::sqrt((double(r.x) - cx) * (double(r.x) - cx) +
                              (double(r.y) - cy) * (double(r.y) - cy)) - R);
                if (d > st.real_max_dev) {
                    st.real_max_dev = d;
                }
            }
        }
        if (!has_real) {
            continue;
        }
        const double base_radial =
            std::sqrt((base_x - cx) * (base_x - cx) + (base_y - cy) * (base_y - cy));
        // 诊断：外推速度幅值 + 与圆周切线夹角（用首个预测点 p0，dt=1/min_output_rate）。
        if (!step.empty() && step.front().is_predicted) {
            const StrokePoint& p0 = step.front();
            const double dt_s =
                (p0.t_us > base_t) ? double(p0.t_us - base_t) / 1e6 : 0.0;
            if (dt_s > 0.0) {
                const double vx = (double(p0.x) - base_x) / dt_s;
                const double vy = (double(p0.y) - base_y) / dt_s;
                const double vmag = std::sqrt(vx * vx + vy * vy);
                const double theta_b =
                    std::atan2(base_y - cy, base_x - cx);  // 基准点圆心角
                const double tx = -std::sin(theta_b), ty = std::cos(theta_b);  // 圆周切线
                const double dot = vx * tx + vy * ty;
                const double cross = vx * ty - vy * tx;
                const double off = std::atan2(std::fabs(cross), dot) * 180.0 / kPi;
                if (vmag > st.diag_v_mag) {
                    st.diag_v_mag = vmag;
                    st.diag_v_off_tangent_deg = off;
                }
            }
        }
        for (const auto& p : step) {
            if (!p.is_predicted) {
                continue;
            }
            ++st.pred_total;
            const double pr = std::sqrt((double(p.x) - cx) * (double(p.x) - cx) +
                                        (double(p.y) - cy) * (double(p.y) - cy));
            const double fringe = pr - base_radial;  // 径向凸出（切线外推的弧外越界）
            if (fringe > st.pred_max_overshoot) {
                st.pred_max_overshoot = fringe;
            }
            if (p.t_us > base_t) {
                st.lead_seen = true;
            }
        }
    }
    return st;
}

}  // namespace

int main() {
    // 中等曲率圆弧 × 真实 device interval（30ms）。fringe ≈ |v_kalman|·interval·sin(lag)，
    //   v=1000px/s 下卡尔曼幅值被衰减到 528/745/872px/s、方向滞后切线 31/37/40° →
    //   修复前 R=60/120/240 fringe 8.98/13.59/16.23px（全红，>3px 界）。
    const double hz = 60.0;
    const float interval = 30.0f;
    const double bound = 3.0;

    // 正式断言档：v=1000px/s，R ∈ {60, 120, 240}。
    const double radii[] = {60.0, 120.0, 240.0};
    for (double R : radii) {
        const auto seq = MakeArc(320.0, 320.0, R, 0.0, 2.0 * kPi * 1.5, 1000.0, hz);
        const TraceStat st = TraceArc(seq, 320.0, 320.0, R, interval);
        std::fprintf(stderr,
                     "[curve:R=%.0f@1000] fringe=%.2fpx real_dev=%.2fpx "
                     "pred_total=%d lead_seen=%d v_mag=%.0fpx/s off_tangent=%.1f°\n",
                     R, st.pred_max_overshoot, st.real_max_dev, st.pred_total,
                     int(st.lead_seen), st.diag_v_mag, st.diag_v_off_tangent_deg);
        char a_name[128];
        std::snprintf(a_name, sizeof(a_name),
                      "[R=%.0f] predicted fringe <= %.1fpx", R, bound);
        CHECK(st.pred_max_overshoot <= bound, a_name);
        char b_name[128];
        std::snprintf(b_name, sizeof(b_name),
                      "[R=%.0f] still emits predicted lead (pred_total>0 & lead_seen)", R);
        CHECK(st.pred_total > 0 && st.lead_seen, b_name);
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_predictor_curve_overshoot] PASS\n");
    } else {
        std::fprintf(stderr, "[test_predictor_curve_overshoot] FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
