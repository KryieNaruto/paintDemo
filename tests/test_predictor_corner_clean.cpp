// bugfix-prediction-clean 正式回归（TDD 先红后绿）：预测「开/关」两态转角干净可逆。
//
// 目标（见 docs/plans/bugfix-prediction-clean.md ③）：
//   - 关（interval=0）：真·无预测外推、转角干净（机制 B）。
//   - 开（interval=16）：直线/低曲率段仍产 is_predicted 领先点，但高曲率/转向处
//     （卡尔曼 v 滞后于最近真实位移 > 50°）不外推 → 转角无凸点（机制 A）。
//   - 两态互切可逆、无状态残留（ON1 ≡ ON2 字节级）。
//
// 断言分层：
//   (b) 白盒几何硬门（恒跑，纯 CPU、确定性、无 GPU）：本测试的核心，见
//       CheckCanonical16 / CheckCanonical0 / CheckReversible / CheckStraightLead /
//       CheckHumanProfile / CheckLargeArcLead + 双实例确定性。
//   (a) PNG 黑盒（离屏后端可用才跑）：五态 P/ON1/OFF/ON2/O0 越界墨 ≈ P，证据落盘。
//
// 无 gtest 依赖，main 返回失败计数（仿 test_stroke_predictor.cpp 结构）。
#include "core/stroke_predictor.h"
#include "dgc_paint_c_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s\n", name);                      \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

using CtxGuard = std::unique_ptr<DgcContext, decltype(&dgcDestroy)>;

namespace {

constexpr double kPi = std::acos(-1.0);

// ── 几何：闭合法框（4 个 90° 转角，640x640 画布）────────────────────────
struct Box {
    double xL, yT, xR, yB;  // 理想框（笔迹中心线贴边）
    Box() : xL(160), yT(160), xR(480), yB(480) {}
};

struct Samp {
    float x, y;
    std::uint64_t t_us;
};

// 60Hz 恒定步长 + 精确落在各顶点（canonical const 档案）。
std::vector<Samp> MakeClosedSquare(const Box& b, double speed_px_s, double hz) {
    const std::uint64_t kDtUs = std::uint64_t(1e6 / hz + 0.5);  // 16667
    const double step = speed_px_s / hz;
    const std::array<std::array<double, 2>, 5> vert = {{
        {b.xL, b.yT}, {b.xR, b.yT}, {b.xR, b.yB}, {b.xL, b.yB}, {b.xL, b.yT},
    }};
    std::array<double, 4> seg;
    std::array<double, 5> cum;  // cum[i] = 第 i 个顶点起点的累计弧长
    cum[0] = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double dx = vert[i + 1][0] - vert[i][0];
        const double dy = vert[i + 1][1] - vert[i][1];
        seg[i] = std::sqrt(dx * dx + dy * dy);
        cum[i + 1] = cum[i] + seg[i];
    }
    const double total = cum[4];

    std::set<double> arcs;
    for (int i = 0; i < 5; ++i) {
        arcs.insert(cum[i]);
    }
    for (double s = step; s < total; s += step) {
        arcs.insert(s);
    }
    arcs.erase(total);

    std::vector<Samp> out;
    std::uint64_t t = 1000000u;
    for (double s : arcs) {
        int e = 0;
        while (e < 4 && s > cum[e + 1]) {
            ++e;
        }
        const double local = s - cum[e];
        const double f = (seg[e] > 0.0) ? (local / seg[e]) : 0.0;
        Samp p;
        p.x = float(vert[e][0] + (vert[e + 1][0] - vert[e][0]) * f);
        p.y = float(vert[e][1] + (vert[e + 1][1] - vert[e][1]) * f);
        p.t_us = t;
        t += kDtUs;
        out.push_back(p);
    }
    return out;
}

// 人手减速档案：60Hz 采样、角点前 D px 减速到 vmin、过角后回 vmax。
std::vector<Samp> MakeHumanSquare(const Box& b, double vmax, double vmin, double D,
                                  double hz) {
    const std::uint64_t kDtUs = std::uint64_t(1e6 / hz + 0.5);
    const std::array<std::array<double, 2>, 5> vert = {{
        {b.xL, b.yT}, {b.xR, b.yT}, {b.xR, b.yB}, {b.xL, b.yB}, {b.xL, b.yT},
    }};
    std::array<double, 4> seg;
    std::array<double, 5> cum;
    cum[0] = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double dx = vert[i + 1][0] - vert[i][0];
        const double dy = vert[i + 1][1] - vert[i][1];
        seg[i] = std::sqrt(dx * dx + dy * dy);
        cum[i + 1] = cum[i] + seg[i];
    }
    const double total = cum[4];
    auto nextVertex = [&](double u) {
        for (int i = 1; i <= 4; ++i) {
            if (u < cum[i]) {
                return cum[i] - u;
            }
        }
        return total - u;
    };
    std::vector<Samp> out;
    double u = 0.0;
    std::uint64_t t = 1000000u;
    while (u < total - 1e-6) {
        const double dv = nextVertex(u);
        double speed = vmax;
        if (dv < D) {
            const double f = dv / D;
            speed = vmin + (vmax - vmin) * f;
        }
        int e = 0;
        while (e < 4 && u > cum[e + 1]) {
            ++e;
        }
        const double local = u - cum[e];
        const double f = (seg[e] > 0.0) ? (local / seg[e]) : 0.0;
        Samp p;
        p.x = float(vert[e][0] + (vert[e + 1][0] - vert[e][0]) * f);
        p.y = float(vert[e][1] + (vert[e + 1][1] - vert[e][1]) * f);
        p.t_us = t;
        t += kDtUs;
        out.push_back(p);
        u += speed / hz;
    }
    return out;
}

// 低曲率大半径弧（R=1500px 下部弧，审阅建议 3）：x 单调递进、每样本方向变化 ~1°。
std::vector<Samp> MakeLargeArc(double radius, double x0, double x1, double speed_px_s,
                               double hz) {
    const std::uint64_t kDtUs = std::uint64_t(1e6 / hz + 0.5);
    const double cy = radius;  // 圆心在 (0, radius)，画下部弧（最低点 y=0）
    const int n = std::max(3, int(std::ceil((x1 - x0) / (speed_px_s / hz))) + 1);
    const double dx = (x1 - x0) / double(n - 1);
    std::vector<Samp> out;
    std::uint64_t t = 1000000u;
    for (int i = 0; i < n; ++i) {
        const double x = x0 + dx * i;
        const double r2 = radius * radius - x * x;
        const double y = (r2 > 0.0) ? (cy - std::sqrt(std::max(r2, 0.0))) : 0.0;
        Samp p;
        p.x = float(x);
        p.y = float(y);
        p.t_us = t;
        t += kDtUs;
        out.push_back(p);
    }
    return out;
}

// ── 白盒统计：同序列直接喂 StrokeModeler（每真实点 Update+Predict 各一次，
//    同 engine inputLoop），统计越出理想框边界的输出点（盘心越界即凸点盘心）。──
struct WbCounts {
    std::size_t out_points = 0;   // 全部输出点（真实 + 预测）
    std::size_t pred_total = 0;   // is_predicted 点数
    std::size_t pred_out = 0;     // is_predicted 盘心越框数
    double      pred_max = 0.0;   // is_predicted 越框最大距离
    std::size_t real_out = 0;     // 真实点盘心越框数（诊断）
};

WbCounts WhiteboxTrace(const std::vector<Samp>& seq, const Box& b, float interval_ms) {
    StrokeModelParams par;
    par.prediction_interval_ms = interval_ms;
    StrokeModeler m;
    m.Configure(par);
    std::vector<StrokePoint> out;
    for (const Samp& s : seq) {
        StrokePoint p{};
        p.x = s.x;
        p.y = s.y;
        p.pressure = 0.5f;
        p.t_us = s.t_us;
        p.is_predicted = false;
        m.Update(p, &out);
        m.Predict(&out);
    }
    WbCounts c;
    c.out_points = out.size();
    for (const auto& o : out) {
        if (o.is_predicted) {
            ++c.pred_total;
        }
        const double dx = (double(o.x) > b.xR) ? (double(o.x) - b.xR)
                          : (double(o.x) < b.xL) ? (b.xL - double(o.x)) : 0.0;
        const double dy = (double(o.y) > b.yB) ? (double(o.y) - b.yB)
                          : (double(o.y) < b.yT) ? (b.yT - double(o.y)) : 0.0;
        const double d = std::max(dx, dy);
        if (d <= 0.0) {
            continue;
        }
        if (o.is_predicted) {
            ++c.pred_out;
            c.pred_max = std::max(c.pred_max, d);
        } else {
            ++c.real_out;
        }
    }
    return c;
}

// 白盒「interval16→0→16」三连跑（同一实例，每段前 Configure 换 interval → Reset），
// 返回第一段与第三段的完整输出，供字节级可逆断言。
bool RunReversible(const std::vector<Samp>& seq, std::vector<StrokePoint>* on1,
                   std::vector<StrokePoint>* on2, std::size_t* off_pred_total,
                   std::size_t* off_out_points) {
    StrokeModelParams p16;
    p16.prediction_interval_ms = 16.0f;
    StrokeModelParams p0;
    p0.prediction_interval_ms = 0.0f;
    StrokeModeler m;
    auto feed = [&](const StrokeModelParams& par, std::vector<StrokePoint>* acc) {
        m.Configure(par);
        for (const Samp& s : seq) {
            StrokePoint p{};
            p.x = s.x;
            p.y = s.y;
            p.pressure = 0.5f;
            p.t_us = s.t_us;
            p.is_predicted = false;
            m.Update(p, acc);
            m.Predict(acc);
        }
    };
    feed(p16, on1);
    std::vector<StrokePoint> off;
    feed(p0, &off);
    feed(p16, on2);
    *off_pred_total = 0;
    for (const auto& o : off) {
        if (o.is_predicted) {
            ++(*off_pred_total);
        }
    }
    *off_out_points = off.size();
    return true;
}

bool SamePoint(const StrokePoint& a, const StrokePoint& b) {
    return a.x == b.x && a.y == b.y && a.pressure == b.pressure &&
           a.tilt_x == b.tilt_x && a.tilt_y == b.tilt_y &&
           a.t_us == b.t_us && a.is_predicted == b.is_predicted;
}

// ── PNG 黑盒（五态越界墨 ≈ P）────────────────────────────────────────
bool IsInk(const std::uint8_t* p) {
    return p[0] < 200 && p[1] < 200 && p[2] < 200;
}

struct RenderResult {
    bool ok = false;
    std::string png;
    std::vector<std::uint8_t> buf;
    int W = 0, H = 0;
};

// 单态渲染：modeler 已在调用前设好（interval<0 = 不激活/passthrough）。
RenderResult RenderSquare(DgcContext* ctx, const char* pngRel,
                          const std::vector<Samp>& seq, const Box& b) {
    RenderResult r;
    const int W = 640, H = 640;
    r.W = W;
    r.H = H;
    r.png = pngRel;
    if (dgcSetOffscreenSurface(ctx, W, H) != DGC_OK) {
        return r;
    }
    dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f);
    dgcBeginStroke(ctx, seq[0].x, seq[0].y, 0.5f, 0.0f, 0.0f);
    for (std::size_t i = 1; i < seq.size(); ++i) {
        dgcStrokeToAt(ctx, seq[i].x, seq[i].y, 0.5f, 0.0f, 0.0f, 0,
                      double(seq[i].t_us));
    }
    dgcEndStroke(ctx);
    dgcFlush(ctx);
    r.buf.assign((std::size_t)W * H * 4, 0);
    if (dgcReadbackPixels(ctx, r.buf.data()) != DGC_OK) {
        return r;
    }
    dgcExportPNG(ctx, pngRel);
    r.ok = true;
    return r;
}

void SetupBrush(DgcContext* ctx, double radius) {
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_RADIUS, radius);
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_HARDNESS, 0.9);
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_OPACITY, 1.0);
    dgcSetBrushColor(ctx, DGC_DEFAULT_BRUSH, 0.0f, 0.0f, 0.0f, 1.0f);
}

// 统计「越出理想框 (box 外扩 margin) 的暗像素」。
struct ProtrudeStat {
    long count = 0;
    double maxpx = 0.0;
};
ProtrudeStat Protrude(const RenderResult& rr, const Box& b, double margin) {
    ProtrudeStat st;
    const double xLo = b.xL - margin, xHi = b.xR + margin;
    const double yLo = b.yT - margin, yHi = b.yB + margin;
    for (int y = 0; y < rr.H; ++y) {
        for (int x = 0; x < rr.W; ++x) {
            const std::uint8_t* p = &rr.buf[((std::size_t)y * rr.W + (std::size_t)x) * 4];
            if (!IsInk(p)) {
                continue;
            }
            const double dx = (double(x) > b.xR) ? (double(x) - b.xR)
                              : (double(x) < b.xL) ? (b.xL - double(x)) : 0.0;
            const double dy = (double(y) > b.yB) ? (double(y) - b.yB)
                              : (double(y) < b.yT) ? (b.yT - double(y)) : 0.0;
            const double d = std::max(dx, dy);
            if (d <= margin) {
                continue;
            }
            ++st.count;
            st.maxpx = std::max(st.maxpx, d - margin);
        }
    }
    return st;
}

const char* kNames[5] = {"P", "ON1", "OFF", "ON2", "O0"};
const int    kIntv[5]  = {-1, 16, 0, 16, 0};

// 跑五态渲染 + Protrude 统计；后端不可用时返回 false（跳过 PNG 断言）。
bool RenderFiveStates(const std::vector<Samp>& seq, const Box& b, double radius,
                      ProtrudeStat stats[5], const std::string& pngBase) {
    CtxGuard shared(dgcCreate(), &dgcDestroy);
    if (!shared) {
        return false;
    }
    const double margin = radius + 3.0;
    for (int st = 0; st < 5; ++st) {
        bool fresh;
        DgcContext* c;
        if (st == 0 || st == 4) {  // P、O0 全新 ctx
            fresh = true;
            c = dgcCreate();
        } else {  // ON1/OFF/ON2 复用 shared
            fresh = false;
            c = shared.get();
        }
        if (!c) {
            return false;
        }
        SetupBrush(c, radius);
        if (kIntv[st] >= 0) {
            if (dgcSetBrushSetting(c, DGC_DEFAULT_BRUSH,
                                   DGC_SETTING_PREDICTION_INTERVAL_MS,
                                   double(kIntv[st])) != DGC_OK) {
                if (fresh) {
                    dgcDestroy(c);
                }
                return false;
            }
        }
        const std::string png = pngBase + kNames[st] + ".png";
        RenderResult rr = RenderSquare(c, png.c_str(), seq, b);
        if (!rr.ok) {
            if (fresh) {
                dgcDestroy(c);
            }
            return false;
        }
        stats[st] = Protrude(rr, b, margin);
        if (fresh) {
            dgcDestroy(c);
        }
    }
    return true;
}

}  // namespace

int main() {
    const double kSpeed = 1500.0;
    const double kRadius = 5.0;
    const double kMargin = kRadius + 3.0;
    const Box b;
    std::fprintf(stderr,
                 "[predictor_clean] canonical const 1500px/s r5 60Hz, margin=%.0fpx\n",
                 kMargin);

    // ══ 白盒硬门 1（canonical const 闭合框 · interval16）：越框预测数 == 0 ══
    {
        const auto seq = MakeClosedSquare(b, kSpeed, 60.0);
        const WbCounts c = WhiteboxTrace(seq, b, 16.0f);
        std::fprintf(stderr,
                     "[wb:intv16-canon] out_points=%zu pred_total=%zu "
                     "pred_outside_box=%zu(max=%.2fpx) real_outside_box=%zu\n",
                     c.out_points, c.pred_total, c.pred_out, c.pred_max, c.real_out);
        CHECK(c.pred_out == 0, "canon16: #predicted protruding out of box == 0 (was 50)");
        CHECK(c.pred_max == 0.0,
              "canon16: max predicted protrusion == 0px (was 9.0px)");
        CHECK(c.pred_total > 0,
              "canon16: straight segments still emit predicted lead (not passthrough)");
    }

    // ══ 白盒硬门 2（canonical const 闭合框 · interval0）：预测总数 == 0 ══
    {
        const auto seq = MakeClosedSquare(b, kSpeed, 60.0);
        const WbCounts c = WhiteboxTrace(seq, b, 0.0f);
        std::fprintf(stderr,
                     "[wb:intv0-canon] out_points=%zu pred_total=%zu "
                     "pred_outside_box=%zu(max=%.2fpx) real_outside_box=%zu\n",
                     c.out_points, c.pred_total, c.pred_out, c.pred_max, c.real_out);
        CHECK(c.pred_total == 0,
              "canon0: total predicted points == 0 (was ~110, 27 protruding)");
        CHECK(c.pred_out == 0, "canon0: no predicted protrusion (subset of pred_total==0)");
        CHECK(c.out_points > 0, "canon0: OFF still emits real smoothed points");
    }

    // ══ 白盒硬门 3（interval16→0→16 字节级可逆，无状态残留）══
    {
        const auto seq = MakeClosedSquare(b, kSpeed, 60.0);
        std::vector<StrokePoint> on1, on2;
        std::size_t off_pred_total = 0, off_out_points = 0;
        RunReversible(seq, &on1, &on2, &off_pred_total, &off_out_points);
        bool same = (on1.size() == on2.size());
        for (std::size_t i = 0; same && i < on1.size(); ++i) {
            same = SamePoint(on1[i], on2[i]);
        }
        std::fprintf(stderr,
                     "[wb:reversible] ON1 points=%zu, OFF(pred_total=%zu out=%zu), "
                     "ON2 points=%zu, byte-equal=%s\n",
                     on1.size(), off_pred_total, off_out_points, on2.size(),
                     same ? "yes" : "no");
        CHECK(same, "reversible: interval16->0->16 full output byte-identical (ON1==ON2)");
        CHECK(off_pred_total == 0, "reversible: interval0 pass adds no predicted points");
    }

    // ══ 白盒硬门 4（interval16 直线快速段领先仍在 · 防「关掉预测装干净」）══
    {
        const std::uint64_t kDtUs = std::uint64_t(1e6 / 60.0 + 0.5);
        const int k = 34;
        StrokeModeler m;
        m.Configure(StrokeModelParams{});
        std::vector<StrokePoint> out;
        for (int i = 0; i < k; ++i) {
            StrokePoint p{};
            p.x = float(i) * float(kSpeed * double(kDtUs) / 1e6);
            p.y = 0.0f;
            p.pressure = 0.5f;
            p.t_us = std::uint64_t(i) * kDtUs;
            p.is_predicted = false;
            m.Update(p, &out);
        }
        std::vector<StrokePoint> pred;
        m.Predict(&pred);
        CHECK(!pred.empty(), "lead: straight quick line tail Predict non-empty");
        const StrokePoint& last_real = out.back();
        bool ahead = false;
        double max_x = last_real.x;
        for (const auto& p : pred) {
            if (p.is_predicted && p.x > last_real.x && p.t_us > last_real.t_us) {
                ahead = true;
            }
            if (p.x > max_x) {
                max_x = p.x;
            }
        }
        CHECK(ahead, "lead: exists is_predicted point ahead of last real (x/t)");
        // 尾段真实速度（px/s）≈ 输入速度。
        const std::size_t n = out.size();
        const StrokePoint& a = out[n - 1 - 8];
        const StrokePoint& z = out[n - 1];
        const double v = (z.t_us > a.t_us)
            ? (double(z.x) - double(a.x)) / (double(z.t_us - a.t_us) / 1e6)
            : 0.0;
        const double extent = max_x - double(last_real.x);
        const double expected = v * 16.0 / 1e3;
        std::fprintf(stderr,
                     "[wb:lead] tail_v=%.0fpx/s extent=%.2fpx expected(v*16ms)=%.2fpx\n",
                     v, extent, expected);
        CHECK(extent > 0.4 * expected && extent < 1.2 * expected,
              "lead: predicted extent within [0.4,1.2]*v*interval (lead preserved)");
    }

    // ══ 白盒硬门 5（审阅建议 2：human 减速档案进正式断言，不只诊断）══
    {
        const auto seq = MakeHumanSquare(b, kSpeed, kSpeed * 0.2, kSpeed * 0.1, 60.0);
        const WbCounts c16 = WhiteboxTrace(seq, b, 16.0f);
        const WbCounts c0 = WhiteboxTrace(seq, b, 0.0f);
        std::fprintf(stderr,
                     "[wb:human16] pts=%zu pred_total=%zu pred_out=%zu(max=%.2f) real_out=%zu\n",
                     c16.out_points, c16.pred_total, c16.pred_out, c16.pred_max,
                     c16.real_out);
        std::fprintf(stderr,
                     "[wb:human0 ] pts=%zu pred_total=%zu pred_out=%zu(max=%.2f)\n",
                     c0.out_points, c0.pred_total, c0.pred_out, c0.pred_max);
        CHECK(c16.pred_out == 0,
              "human: interval16 corner protrusion == 0 (real hand-speed profile)");
        CHECK(c0.pred_total == 0, "human: interval0 produces zero predicted points");
    }

    // ══ 白盒硬门 6（审阅建议 3：低曲率大半径弧仍保持预测领先）══
    {
        const auto seq = MakeLargeArc(1500.0, -400.0, 400.0, kSpeed, 60.0);
        std::size_t emit_steps = 0, total_steps = 0;
        StrokeModeler m;
        StrokeModelParams par;
        par.prediction_interval_ms = 16.0f;
        m.Configure(par);
        std::vector<StrokePoint> real;
        for (const Samp& s : seq) {
            StrokePoint p{};
            p.x = s.x;
            p.y = s.y;
            p.pressure = 0.5f;
            p.t_us = s.t_us;
            p.is_predicted = false;
            m.Update(p, &real);
            std::vector<StrokePoint> step;
            m.Predict(&step);
            ++total_steps;
            if (!step.empty()) {
                ++emit_steps;
            }
        }
        std::fprintf(stderr,
                     "[wb:arc-R1500] samples=%zu predict-emit-steps=%zu/%zu\n",
                     seq.size(), emit_steps, total_steps);
        CHECK(emit_steps * 2 >= total_steps,
              "arc: low-curvature R=1500 arc keeps predicted lead (>=half of steps emit)");
        // 尾部单发 Predict 仍有领先点。
        std::vector<StrokePoint> pred;
        m.Predict(&pred);
        bool ahead = !pred.empty();
        if (ahead) {
            ahead = pred.back().is_predicted && pred.back().t_us > real.back().t_us;
        }
        CHECK(ahead, "arc: final Predict emits is_predicted lead past last real");
    }

    // ══ 双实例确定性：canonical interval16 与 interval0 逐点一致 ══
    {
        const auto seq = MakeClosedSquare(b, kSpeed, 60.0);
        StrokeModelParams p16;
        p16.prediction_interval_ms = 16.0f;
        StrokeModelParams p0;
        p0.prediction_interval_ms = 0.0f;
        auto feed = [&](const StrokeModelParams& par, std::vector<StrokePoint>* acc) {
            StrokeModeler m;
            m.Configure(par);
            for (const Samp& s : seq) {
                StrokePoint p{};
                p.x = s.x;
                p.y = s.y;
                p.pressure = 0.5f;
                p.t_us = s.t_us;
                p.is_predicted = false;
                m.Update(p, acc);
                m.Predict(acc);
            }
        };
        std::vector<StrokePoint> a16, b16, a0, b0;
        feed(p16, &a16);
        feed(p16, &b16);
        feed(p0, &a0);
        feed(p0, &b0);
        bool same16 = (a16.size() == b16.size());
        for (std::size_t i = 0; same16 && i < a16.size(); ++i) {
            same16 = SamePoint(a16[i], b16[i]);
        }
        bool same0 = (a0.size() == b0.size());
        for (std::size_t i = 0; same0 && i < a0.size(); ++i) {
            same0 = SamePoint(a0[i], b0[i]);
        }
        CHECK(same16, "determinism: two instances interval16 byte-identical");
        CHECK(same0, "determinism: two instances interval0 byte-identical");
    }

    // ══ PNG 黑盒（a）：五态越界墨 ≈ P（离屏后端可用才跑，证据落盘）══
    {
        const auto seq = MakeClosedSquare(b, kSpeed, 60.0);
        ProtrudeStat stats[5];
        const std::string pngBase = "predictor_clean_corner_";
        const bool ok = RenderFiveStates(seq, b, kRadius, stats, pngBase);
        if (ok) {
            std::fprintf(stderr, "\n=== corner protrusion table (margin=%.0fpx) ===\n",
                         kMargin);
            std::fprintf(stderr, "%-4s %-8s %-8s\n", "state", "count", "maxpx");
            for (int st = 0; st < 5; ++st) {
                std::fprintf(stderr, "%-4s %-8ld %-8.1f\n", kNames[st],
                             stats[st].count, stats[st].maxpx);
            }
            const ProtrudeStat& p0 = stats[0];
            for (int st = 1; st < 5; ++st) {
                const std::string name = std::string("png: ") + kNames[st];
                CHECK(stats[st].count <= p0.count + 16,
                      (name + " protrusion count <= P.count+16").c_str());
                CHECK(stats[st].maxpx <= p0.maxpx + 1.0,
                      (name + " protrusion maxpx <= P.maxpx+1.0").c_str());
            }
            std::fprintf(stderr, "PNG evidence -> %s{P,ON1,OFF,ON2,O0}.png (cwd)\n",
                         pngBase.c_str());
        } else {
            std::fprintf(stderr,
                         "[predictor_clean] skip PNG 5-state assertions "
                         "(offscreen backend unavailable); whitebox still ran\n");
        }
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_predictor_corner_clean] PASS\n");
    } else {
        std::fprintf(stderr, "[test_predictor_corner_clean] FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
