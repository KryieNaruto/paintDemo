// B1-5 白盒移植 Ink Stroke Modeler → core/stroke_predictor 的 host 单测。
// 覆盖验收 5 条：平滑 / 预测 + is_predicted / 覆盖 / 确定性 / 泄漏循环。
// 无 gtest 依赖，main 返回失败计数（仿 test_determinism.cpp）。
#include "core/stroke_predictor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, name) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; } } while (0)

namespace {

// 生成一条沿 +x 匀速、y 方向叠加正弦手抖的原始点流（mm、us）。
// t_us 从 0 起按 dt_us 递增；y = amp * sin(2π * freq_hz * t_s)。
std::vector<StrokePoint> MakeWobblyLine(int n, std::uint64_t dt_us,
                                        float step_x_mm, float amp_mm, float freq_hz) {
    std::vector<StrokePoint> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double t_s = double(i) * double(dt_us) / 1e6;
        StrokePoint p{};
        p.x = float(i) * step_x_mm;
        p.y = amp_mm * float(std::sin(2.0 * M_PI * freq_hz * t_s));
        p.pressure = 0.5f;
        p.tilt_x = 0.3f;
        p.tilt_y = -0.1f;
        p.t_us = std::uint64_t(i) * dt_us;
        p.is_predicted = false;
        pts.push_back(p);
    }
    return pts;
}

// 一列点在 y 方向的方差（均值近似为 0）。
double VarianceY(const std::vector<StrokePoint>& pts, std::size_t begin) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = begin; i < pts.size(); ++i) {
        sum += double(pts[i].y) * double(pts[i].y);
        ++n;
    }
    return n > 0 ? sum / double(n) : 0.0;
}

bool SamePoint(const StrokePoint& a, const StrokePoint& b) {
    return a.x == b.x && a.y == b.y && a.pressure == b.pressure &&
           a.tilt_x == b.tilt_x && a.tilt_y == b.tilt_y &&
           a.t_us == b.t_us && a.is_predicted == b.is_predicted;
}

}  // namespace

int main() {
    // ── 验收 1：Update 平滑 ──────────────────────────────────────────────────
    {
        StrokeModeler m;
        const int k = 100;
        const auto in = MakeWobblyLine(k, 1000 /*1ms*/, 0.5f /*500mm/s*/, 5.0f, 100.0f);
        std::vector<StrokePoint> out;
        for (const auto& p : in) {
            m.Update(p, &out);
        }
        CHECK(!out.empty(), "smooth: output non-empty");
        bool all_real = true;
        for (const auto& p : out) {
            if (p.is_predicted) { all_real = false; break; }
        }
        CHECK(all_real, "smooth: all outputs is_predicted=false");

        const std::size_t begin = 10;
        const double var_in = VarianceY(in, begin);
        const double var_out = VarianceY(out, begin);
        CHECK(var_out < var_in, "smooth: output wobble variance < input variance");
        CHECK(var_out < 0.5, "smooth: output wobble strongly attenuated (var < 0.5)");
    }

    // ── 验收 2：Predict 产出预测点带 is_predicted ────────────────────────────
    {
        StrokeModeler m;
        const auto in = MakeWobblyLine(30, 1000, 0.5f, 0.0f, 0.0f);
        std::vector<StrokePoint> real;
        for (const auto& p : in) {
            m.Update(p, &real);
        }
        std::vector<StrokePoint> pred;
        m.Predict(&pred);

        CHECK(!pred.empty(), "predict: non-empty");
        bool all_pred = true;
        std::uint64_t prev = real.back().t_us;
        bool after_last = true;
        bool increasing = true;
        for (const auto& p : pred) {
            if (!p.is_predicted) { all_pred = false; }
            if (p.t_us <= real.back().t_us) { after_last = false; }
            if (p.t_us <= prev) { increasing = false; }
            prev = p.t_us;
        }
        CHECK(all_pred, "predict: all predicted points is_predicted=true");
        CHECK(after_last, "predict: predicted t_us > last real t_us");
        CHECK(increasing, "predict: predicted t_us strictly increasing");
    }

    // ── 验收 3：真实点覆盖预测点 ────────────────────────────────────────────
    {
        StrokeModeler m;
        const auto in = MakeWobblyLine(30, 1000, 0.5f, 0.0f, 0.0f);
        std::vector<StrokePoint> real;
        for (const auto& p : in) {
            m.Update(p, &real);
        }
        std::vector<StrokePoint> pred;
        m.Predict(&pred);

        // 下一真实点（t_us 继续推进）到达：Update 输出不应再含上一轮预测点。
        StrokePoint next{};
        next.x = float(in.size()) * 0.5f;
        next.y = 0.0f;
        next.pressure = 0.5f;
        next.t_us = std::uint64_t(in.size()) * 1000;
        next.is_predicted = false;

        std::vector<StrokePoint> after;
        m.Update(next, &after);

        bool any_overlap = false;
        for (const auto& p : after) {
            if (p.is_predicted) { any_overlap = true; break; }
            for (const auto& q : pred) {
                if (p.t_us == q.t_us) { any_overlap = true; break; }
            }
        }
        CHECK(!any_overlap, "coverage: predicted points overwritten by real point");

        // 再次 Predict：应基于最新真实点外推（t_us 全部大于新真实点）。
        std::vector<StrokePoint> pred2;
        m.Predict(&pred2);
        bool fresh = !pred2.empty();
        for (const auto& p : pred2) {
            if (p.t_us <= next.t_us) { fresh = false; }
        }
        CHECK(fresh, "coverage: re-predict extrapolates beyond new real point");
    }

    // ── 验收 4：固定步长欧拉积分（无随机）确定性 ────────────────────────────
    {
        const auto in = MakeWobblyLine(50, 1000, 0.5f, 3.0f, 60.0f);
        StrokeModeler a, b;
        std::vector<StrokePoint> oa, ob;
        for (const auto& p : in) {
            a.Update(p, &oa);
            b.Update(p, &ob);
        }
        bool same = (oa.size() == ob.size());
        for (std::size_t i = 0; same && i < oa.size(); ++i) {
            same = SamePoint(oa[i], ob[i]);
        }
        CHECK(same, "determinism: two instances produce identical real outputs");

        std::vector<StrokePoint> pa, pb;
        a.Predict(&pa);
        b.Predict(&pb);
        bool same_pred = (pa.size() == pb.size());
        for (std::size_t i = 0; same_pred && i < pa.size(); ++i) {
            same_pred = SamePoint(pa[i], pb[i]);
        }
        CHECK(same_pred, "determinism: two instances produce identical predictions");
    }

    // ── 工程约束：构造/Update/Predict/析构泄漏循环（ASan/LSan 兜底） ────────
    {
        const auto in = MakeWobblyLine(8, 1000, 0.5f, 2.0f, 50.0f);
        for (int r = 0; r < 2000; ++r) {
            auto m = std::make_unique<StrokeModeler>();
            std::vector<StrokePoint> real, pred;
            for (const auto& p : in) {
                m->Update(p, &real);
            }
            m->Predict(&pred);
        }
        CHECK(true, "leak loop: 2000 rounds create/update/predict/destroy");
    }

    return failures == 0 ? 0 : 1;
}
