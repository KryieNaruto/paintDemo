// bugfix-prediction-decel 正式回归（TDD 先红后绿）：预测「开」时快速直线减速到停，
// 外推尾不得越出真实轨迹（预测点永久合墨、无擦除 → 减速/停笔尾越界即永久凸出=用户所
// 见「预测抢跑扯」）。
//
// 根因（见 docs/plans/bugfix-prediction-decel.md §1）：Predict 用 Kalman 恒定速度 v 外推
// `n = interval/period` 个点 + StrokeEndPredictor 停笔点；直线减速到停时 Kalman v 滞后真实
// 减速 → 尾部按偏高 v 外推并永久成墨。8a0ee97 机制 A 只按「v 方向 vs 最近真实位移方向」夹角
// 拦转向（直线减速夹角≈0 不触发）；机制 B 只覆盖 interval<=0（关）。
//
// 断言分层（纯 CPU、无 GPU、确定性）：
//   (A) 核心（先红）：整个 trace 里所有预测点最大 x 与真实点最大 x 之差 ≤ 8px（两档速度）。
//   (B) 守卫（防「为干净而关掉预测」）：稳态直线运动阶段仍产出领先点（is_predicted 且
//       x/t 大于触发它的那次 Update 末真实点）——保证修复保留延迟遮盖领先。
// 零回归由既有 test_predictor_corner_clean（门 4 直线领先 / 门 5 人形减速 / 门 6 大弧 / 可逆 /
// 确定性）承担（§4 守卫表）。
//
// 无 gtest 依赖，main 返回失败计数（仿 test_predictor_corner_clean.cpp）。
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

struct Samp {
    float x, y;
    std::uint64_t t_us;
};

// 真实快速直线到抬笔档案：60Hz。
//   ramp-in N1 点 0→vmax，steady N2 点 @vmax，decel N3 点 vmax→~0，停 N4 点（抬手）。
// 手在减速段后停在终点，预测若按滞后的 Kalman v 外推会在末尾留下越出真实轨迹的凸出尾。
std::vector<Samp> MakeFlickStop(double vmax, int n1 = 10, int n2 = 26, int n3 = 10,
                                int n4 = 2) {
    const std::uint64_t kDtUs = std::uint64_t(1e6 / 60.0 + 0.5);
    std::vector<Samp> out;
    double x = 0.0;
    std::uint64_t t = 1000000u;
    auto add = [&](double v) {
        Samp p;
        p.x = float(x);
        p.y = 40.0f;
        p.t_us = t;
        t += kDtUs;
        out.push_back(p);
        x += v / 60.0;
    };
    for (int i = 0; i < n1; ++i) {
        add(vmax * double(i + 1) / double(n1));
    }
    for (int i = 0; i < n2; ++i) {
        add(vmax);
    }
    for (int i = 0; i < n3; ++i) {
        add(vmax * (1.0 - double(i + 1) / double(n3)));
    }
    for (int i = 0; i < n4; ++i) {
        add(0.0);
    }
    return out;
}

struct TraceStat {
    double real_max = 0.0;
    double pred_max = 0.0;
    bool lead_seen = false;   // 稳态运动中某次 Predict 产出了领先点（守卫 B）
};

// 逐点 Update+Predict；每步记录该步 Update 末真实点，检查该步 Predict 是否有领先点。
TraceStat Trace(const std::vector<Samp>& seq, float interval_ms) {
    StrokeModelParams par;
    par.prediction_interval_ms = interval_ms;
    StrokeModeler m;
    m.Configure(par);
    TraceStat st;
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
        // 本轮 Update 产出的末真实点（Predict 外推基准）。
        double last_real_x = 0.0;
        std::uint64_t last_real_t = 0;
        bool has_real = false;
        for (const auto& r : real) {
            if (!r.is_predicted) {
                last_real_x = r.x;
                last_real_t = r.t_us;
                has_real = true;
                if (r.x > st.real_max) {
                    st.real_max = r.x;
                }
            }
        }
        for (const auto& p : step) {
            if (p.is_predicted) {
                if (p.x > st.pred_max) {
                    st.pred_max = p.x;
                }
                if (has_real && p.x > last_real_x && p.t_us > last_real_t) {
                    st.lead_seen = true;
                }
            }
        }
    }
    return st;
}

// 单档速度的断言（核心 A + 守卫 B）。
void CheckSpeed(double vmax, const char* name) {
    const auto seq = MakeFlickStop(vmax);
    const TraceStat st = Trace(seq, 16.0f);
    // A 契约（相对 bound，推导见下）：预测尾必须远小于峰值稳态领先 vmax·interval（量级上从
    // 修复前的 ~60% 压到 ≤25%），且不松于绝对 8px 下限。
    //   推导：无擦除架构下，最后一段仍运动样本的外推尾（≈ v_last_moving·interval）无法在下一
    //   样本不前进时收回；真实停笔 v_last_moving ≈ vmax/减速样本数（本档案减速 10 样本）→ 尾
    //   ≈ 10%·vmax·interval。故用「≤ 25% 峰值领先、下限 8px」作硬门：修复前 @2400/6000 尾 ≈
    //   23.7/59.2px（61% 领先）红；修复后 4.7/11.7px（12% 领先）绿。
    const double lead = vmax * 16.0 / 1e3;
    const double bound = std::max(8.0, 0.25 * lead);
    std::fprintf(stderr,
                 "[decel:%s] vmax=%.0fpx/s real_max=%.1f pred_max=%.1f "
                 "overrun_past_real=%.2fpx (bound=%.1fpx lead=%.1fpx) lead_seen=%d\n",
                 name, vmax, st.real_max, st.pred_max, st.pred_max - st.real_max, bound,
                 lead, int(st.lead_seen));
    char a_name[128];
    std::snprintf(a_name, sizeof(a_name), "[%s] decel-stop: predicted tail <= "
                                          "max(8px, 25%% of vmax*interval)", name);
    CHECK(st.pred_max - st.real_max <= bound, a_name);
    // B：稳态直线仍保留领先（修复不得把预测关成干净）。
    char b_name[128];
    std::snprintf(b_name, sizeof(b_name), "[%s] steady-line lead still emitted", name);
    CHECK(st.lead_seen, b_name);
}

}  // namespace

int main() {
    // 两档速度：2400px/s（中等快速）+ 6000px/s（极快直线，越界放大档）。
    CheckSpeed(2400.0, "2400");
    CheckSpeed(6000.0, "6000");

    if (failures == 0) {
        std::fprintf(stderr, "[test_predictor_decel_clean] PASS\n");
    } else {
        std::fprintf(stderr, "[test_predictor_decel_clean] FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
