// P7-4 真实时间戳入口回归测试：验证 modeler 在真实（60Hz）输入节奏下卡尔曼速度
// 校准正确、预测点不「抢跑回扯」（无 3x 级过度外推），以及 C API 层 dgcStrokeToAt
// 在 override（dgcSetFixedTime）模式下忽略真实 t_us、与 dgcStrokeTo 逐字节一致。
//
// 结构对齐 tests/test_stroke_predictor.cpp（白盒直接构造 StrokeModeler，不新增共享头）：
//   1) 60Hz 真实间隔（16667µs）喂点流 → 速度 ≈ 位移/16667µs（±30%，无 3x 高估）；
//   2) Predict 外推总长 ≈ velocity * prediction_interval_ms（±40%，无 3x 过度外推），
//      末点 is_predicted == true；
//   3) 同刻/乱序 t_us（dt<=0）边界 → 不崩溃、输出无 NaN（走既有防御分支）；
//   4) C API 层 override 等价：dgcSetFixedTime 后 dgcStrokeToAt（任意 t_us）与
//      dgcStrokeTo 渲染像素逐字节一致（真实 t_us 未渗入 override 路径）；Null 后端
//      （无 Vulkan 离屏）时跳过像素断言仍通过（同 test_c_api_headless 探测口径）。
//
// 无 gtest 依赖，main 返回失败计数。
#include "core/stroke_predictor.h"
#include "dgc_paint_c_api.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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

// 从真实输出点（is_predicted=false）尾部 window 个点区间推断速度（mm/s）。
// 输出点经 Resampler 固定周期布点，位置线性插值，故尾段速度 ≈ 真实输入速度。
double TailVelocity(const std::vector<StrokePoint>& pts, std::size_t window) {
    if (pts.size() <= window) {
        return 0.0;
    }
    const std::size_t n = pts.size();
    const StrokePoint& a = pts[n - 1 - window];
    const StrokePoint& b = pts[n - 1];
    const double dt_s = double(b.t_us - a.t_us) / 1e6;
    if (dt_s <= 0.0) {
        return 0.0;
    }
    return (double(b.x) - double(a.x)) / dt_s;
}

constexpr int kW = 200;
constexpr int kH = 120;
constexpr int kPoints = 24;

// C API 离屏渲染一次 modeler 笔画（override 固定时间步）。useAt 决定走 dgcStrokeToAt
// （传任意 t_us，验证 override 下被忽略）还是 dgcStrokeTo。返回 false 表示离屏后端
// 不可用（Null 后端），调用方跳过像素断言。
bool RenderStroke(std::vector<std::uint8_t>& buf, bool useAt, double fixed_step) {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return false;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) {
        return false;  // Null 后端：离屏不可用。
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    dgcSetRandomSeed(ctx.get(), 0);
    dgcSetBrushSetting(ctx.get(), DGC_DEFAULT_BRUSH,
                       DGC_SETTING_KALMAN_PROCESS_NOISE, 0.01);  // 激活 modeler
    dgcSetFixedTime(ctx.get(), fixed_step);                       // override 开
    dgcBeginStroke(ctx.get(), 20.0f, 60.0f, 0.5f, 0.0f, 0.0f);
    for (int i = 1; i < kPoints; ++i) {
        const float x = 20.0f + 160.0f * (float(i) / (float)(kPoints - 1));
        const float y = 60.0f + 15.0f * float(std::sin(2.0 * M_PI * (double(i) / (double)(kPoints - 1))));
        if (useAt) {
            dgcStrokeToAt(ctx.get(), x, y, 0.5f, 0.0f, 0.0f, 0, 123456789.0);
        } else {
            dgcStrokeTo(ctx.get(), x, y, 0.5f, 0.0f, 0.0f, 0);
        }
    }
    dgcEndStroke(ctx.get());
    dgcFlush(ctx.get());
    buf.assign((std::size_t)kW * kH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // ── 验收 1 + 2：60Hz 真实时间戳速度校准 + 预测不抢跑（白盒） ─────────────────
    // 以 60Hz 真实间隔（16667µs）喂一条沿 +X 匀速直线（每点位移对应 500mm/s）。
    // 若 t_us 被合成 180Hz 基准（5555µs）错误解释，模型器会 3x 高估速度 → 预测点
    // 外推 3x 过度 → 「抢跑回扯」。真实 t_us 下速度应 ≈ 位移/16667µs。
    {
        const std::uint64_t kDtUs  = 16667;   // 60Hz
        const double       kTrueV  = 500.0;   // mm/s
        const float        step_x  = float(kTrueV * double(kDtUs) / 1e6);  // 8.3335mm
        const int          k       = 40;

        StrokeModeler m;
        m.Configure(StrokeModelParams{});
        std::vector<StrokePoint> out;
        for (int i = 0; i < k; ++i) {
            StrokePoint p{};
            p.x = float(i) * step_x;
            p.y = 0.0f;
            p.pressure = 0.5f;
            p.tilt_x = 0.3f;
            p.tilt_y = -0.1f;
            p.t_us = std::uint64_t(i) * kDtUs;
            p.is_predicted = false;
            m.Update(p, &out);
        }
        CHECK(!out.empty(), "real-time: output non-empty");

        // 断言 A（速度校准）：尾段速度 ≈ 位移/16667µs，无 3x 高估。
        const double v = TailVelocity(out, 10);
        std::fprintf(stderr, "[test_stroke_predictor_real_time] tail velocity=%.2f mm/s (true=%.2f)\n", v, kTrueV);
        CHECK(v > 0.7 * kTrueV && v < 1.3 * kTrueV,
              "real-time: velocity calibrated to dx/16667us (no 3x overestimate)");

        // 断言 B（预测不抢跑）：外推总长 ≈ velocity * 16ms（±40%），末点 is_predicted。
        std::vector<StrokePoint> pred;
        m.Predict(&pred);
        CHECK(!pred.empty(), "real-time: predict non-empty");
        const StrokePoint& last_real = out.back();
        double max_x = last_real.x;
        for (const auto& p : pred) {
            if (p.x > max_x) {
                max_x = p.x;
            }
        }
        const double extent   = max_x - double(last_real.x);
        const double expected = kTrueV * 0.016;  // 8mm
        std::fprintf(stderr,
                     "[test_stroke_predictor_real_time] predict extent=%.3f mm (expected ~%.3f mm)\n",
                     extent, expected);
        CHECK(extent > 0.6 * expected && extent < 1.4 * expected,
              "real-time: predict extent ~ velocity*16ms (no 3x overshoot)");
        CHECK(pred.back().is_predicted, "real-time: last predicted point is_predicted=true");
    }

    // ── 验收 4（边界安全）：同刻/乱序 t_us（dt<=0）不崩溃、无 NaN ─────────────────
    {
        StrokeModeler m;
        m.Configure(StrokeModelParams{});
        std::vector<StrokePoint> out;
        auto feed = [&](std::uint64_t t, float x) {
            StrokePoint p{};
            p.x = x;
            p.y = 0.0f;
            p.pressure = 0.5f;
            p.t_us = t;
            p.is_predicted = false;
            m.Update(p, &out);
        };
        feed(1000, 0.0f);
        feed(1000, 1.0f);  // 同刻
        feed(500,  2.0f);  // 倒退
        feed(500,  3.0f);  // 与上一点同刻

        bool finite = true;
        for (const auto& p : out) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.pressure)) {
                finite = false;
                break;
            }
        }
        CHECK(finite, "dt<=0 boundary: real outputs all finite (no NaN)");

        std::vector<StrokePoint> pred;
        m.Predict(&pred);
        bool finite_pred = true;
        for (const auto& p : pred) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                finite_pred = false;
                break;
            }
        }
        CHECK(finite_pred, "dt<=0 boundary: predictions all finite (no NaN)");
    }

    // ── 验收 2（override 等价，C API 层）：override 时 dgcStrokeToAt 忽略真实 t_us ──
    {
        std::vector<std::uint8_t> a, b;
        const bool okA = RenderStroke(a, false, 16667.0);  // dgcStrokeTo
        const bool okB = RenderStroke(b, true,  16667.0);  // dgcStrokeToAt(任意 t_us)
        if (okA && okB) {
            CHECK(a.size() == b.size() &&
                      std::memcmp(a.data(), b.data(), a.size()) == 0,
                  "override: dgcStrokeToAt(arbitrary t_us) renders byte-identical to dgcStrokeTo");
        } else {
            std::fprintf(stderr,
                         "[test_stroke_predictor_real_time] skip override-equivalence "
                         "pixel compare (offscreen backend unavailable)\n");
        }
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_stroke_predictor_real_time] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
