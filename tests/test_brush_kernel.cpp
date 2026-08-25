// B3-1 host 单测：自研 C++ 笔刷内核主链。
// 覆盖：createBrush 非零句柄 / strokeTo 非空 / dab 数·半径·不透明度取值范围 /
// countDabsTo 求和语义 / 颜色调制（HSV→RGB）/ 同 seed 确定性 / 传感器滤波结构断言 /
// 多轮 create/stroke 无泄漏循环。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "core/determinism.h"
#include "core/types.h"
#include "kernels/brush/brush.h"
#include "kernels/brush/brush_kernel.h"
#include "kernels/brush/brush_mapping.h"
#include "kernels/brush/color.h"
#include "kernels/brush/sensors.h"

static int failures = 0;
#define CHECK(cond, name)                       \
    do {                                        \
        if (!(cond)) {                          \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                         \
        }                                       \
    } while (0)

namespace {

StrokePoint MakePoint(float x, float y, float pressure, std::uint64_t t_us) {
    StrokePoint p{};
    p.x = x;
    p.y = y;
    p.pressure = pressure;
    p.t_us = t_us;
    return p;
}

// 喂一条直线笔迹（N 段，每段 +dx px、+dt_us 微秒），返回总 dab 数。
std::size_t StrokeCount(Brush& b, float dx, std::uint64_t dt_us, int segments) {
    b.beginStroke(MakePoint(0.0f, 0.0f, 0.5f, 0));
    std::size_t n = 0;
    for (int i = 1; i <= segments; ++i) {
        auto out = b.strokeTo(MakePoint(dx * (float)i, 0.0f, 0.5f, dt_us * (std::uint64_t)i));
        n += out.size();
    }
    b.endStroke();
    return n;
}

}  // namespace

int main() {
    // ── 1. BrushKernel：createBrush 非零句柄 + strokeTo 非空 ──
    {
        BrushKernel k(42);
        BrushHandle h = k.createBrush(BrushParams{});
        CHECK(h != 0, "createBrush non-zero handle");

        k.beginStroke(h, MakePoint(0.0f, 0.0f, 0.5f, 0));
        std::size_t total = 0;
        std::vector<StampData> all;
        for (int i = 1; i <= 20; ++i) {
            auto v = k.strokeTo(h, MakePoint(5.0f * (float)i, 0.0f, 0.5f,
                                             (std::uint64_t)i * 16666));
            total += v.size();
            all.insert(all.end(), v.begin(), v.end());
        }
        k.endStroke(h);
        CHECK(total > 0, "strokeTo non-empty (dabs emitted)");

        for (const StampData& s : all) {
            CHECK(s.radius > 0.0f && s.radius <= 512.0f, "radius in (0,512]");
            CHECK(s.opacity >= 0.0f && s.opacity <= 1.0f, "opacity in [0,1]");
            CHECK(s.hardness >= 0.0f && s.hardness <= 1.0f, "hardness in [0,1]");
        }
    }

    // ── 2. countDabsTo 求和语义（res4 = res1+res2+res3，非取最大）──
    {
        // 全距 100px、总时长 1s、半径 10。
        constexpr int kSegments = 40;
        constexpr float kDx = 2.5f;         // 每段 2.5px → 总距 100px
        constexpr std::uint64_t kDtUs = 25000;  // 每段 0.025s → 总时 1s

        // S：距离 + 时间双开（求和）。dabsPerBasicRadius=4 → 40，dabsPerSecond=10 → 10，共 ≈50。
        Brush s(std::make_unique<Mt19937Random>(1));
        s.setBase(brush::SettingId::DabsPerBasicRadius, 4.0f);
        s.setBase(brush::SettingId::DabsPerSecond, 10.0f);
        const std::size_t nS = StrokeCount(s, kDx, kDtUs, kSegments);

        // B：仅距离（≈40）。
        Brush b(std::make_unique<Mt19937Random>(1));
        b.setBase(brush::SettingId::DabsPerBasicRadius, 4.0f);
        b.setBase(brush::SettingId::DabsPerSecond, 0.0f);
        const std::size_t nB = StrokeCount(b, kDx, kDtUs, kSegments);

        // C：仅时间（≈10）。
        Brush c(std::make_unique<Mt19937Random>(1));
        c.setBase(brush::SettingId::DabsPerBasicRadius, 0.0f);
        c.setBase(brush::SettingId::DabsPerSecond, 10.0f);
        const std::size_t nC = StrokeCount(c, kDx, kDtUs, kSegments);

        CHECK(nS > nB && nS > nC, "sum > max (res4 = res1+res2+res3)");
        // 求和语义的二次校验：S 明显多于仅距离的 B（时间项确有贡献），
        // 且本配置下距离项（≈40）占主导、远大于时间项（≈10）。
        CHECK(nB > nC, "distance term dominates time term in this setup");
        CHECK(nS > nB + 5, "time term adds observable dabs on top of distance");
    }

    // ── 3. 颜色调制：HSV→RGB + 逐 dab r/g/b ──
    {
        float r, g, b;
        brush::hsv_to_rgb_float(0.0f, 1.0f, 1.0f, &r, &g, &b);
        CHECK(r > 0.99f && g < 0.01f && b < 0.01f, "hsv(0,1,1) -> red");
        brush::hsv_to_rgb_float(120.0f, 1.0f, 1.0f, &r, &g, &b);
        CHECK(g > 0.99f && r < 0.01f && b < 0.01f, "hsv(120,1,1) -> green");
        brush::hsv_to_rgb_float(240.0f, 1.0f, 1.0f, &r, &g, &b);
        CHECK(b > 0.99f && r < 0.01f && g < 0.01f, "hsv(240,1,1) -> blue");

        // Brush 产出带颜色的 dab。
        Brush br(std::make_unique<Mt19937Random>(7));
        br.setColor(0.0f, 1.0f, 0.8f);  // 亮红
        br.beginStroke(MakePoint(0, 0, 0.5f, 0));
        auto out = br.strokeTo(MakePoint(10, 0, 0.5f, 16666));
        br.endStroke();
        CHECK(!out.empty(), "color stroke non-empty");
        if (!out.empty()) {
            CHECK(out[0].r > 0.7f && out[0].g < 0.05f && out[0].b < 0.05f,
                  "dab carries red color (HSV->RGB)");
        }
    }

    // ── 4. 同 seed 确定性（含 RNG 抖动，逐字段一致）──
    {
        auto run = [](std::uint64_t seed, std::vector<StampData>& dst) {
            Brush b(std::make_unique<Mt19937Random>(seed));
            b.setBase(brush::SettingId::OffsetByRandom, 0.1f);  // 打开高斯抖动，让 RNG 可观察
            b.beginStroke(MakePoint(0, 0, 0.5f, 0));
            for (int i = 1; i <= 20; ++i) {
                auto v = b.strokeTo(MakePoint(5.0f * (float)i, 0.0f, 0.5f,
                                              (std::uint64_t)i * 16666));
                dst.insert(dst.end(), v.begin(), v.end());
            }
            b.endStroke();
        };
        std::vector<StampData> a1, a2, a3;
        run(42, a1);
        run(42, a2);
        run(43, a3);
        CHECK(a1.size() == a2.size(), "same seed -> same dab count");
        bool same = a1.size() == a2.size();
        for (std::size_t i = 0; same && i < a1.size(); ++i) {
            same = (a1[i].x == a2[i].x && a1[i].y == a2[i].y &&
                    a1[i].radius == a2[i].radius && a1[i].r == a2[i].r);
        }
        CHECK(same, "same seed -> identical dab sequence (field-wise)");
        bool diff = false;
        for (std::size_t i = 0; i < a1.size() && i < a3.size(); ++i) {
            if (a1[i].x != a3[i].x || a1[i].y != a3[i].y) {
                diff = true;
                break;
            }
        }
        CHECK(diff, "different seed -> different jittered sequence");
    }

    // ── 5. 传感器滤波结构断言 ──
    {
        // 低通步进系数：dtime=0 → 0；大 dtime → 1。
        CHECK(brush::lowpass_fac(1.0f, 0.0f) == 0.0f, "lowpass_fac(0)=0");
        CHECK(brush::lowpass_fac(1.0f, 100.0f) > 0.999f, "lowpass_fac(inf)->1");
        // 速度归一化：dx=10,dtime=0.1,viewzoom=1 → 100 px/s。
        const float sp = brush::norm_speed(10.0f, 0.0f, 0.1f, 1.0f);
        CHECK(std::fabs(sp - 100.0f) < 1e-3f, "norm_speed = hypot(dx,dy)/dtime*viewzoom");
        // tilt 转换。
        const float decl = brush::tilt_declination(0.0f, 1.0f);
        CHECK(std::fabs(decl - 45.0f) < 1e-3f, "tilt_declination(0,1)=45");
        const float asc = brush::tilt_ascension(0.0f, 1.0f);
        CHECK(std::fabs(asc - 0.0f) < 1e-3f, "tilt_ascension(0,1)=0");
        // 压力 gain：gain=0 → 原值。
        CHECK(brush::apply_pressure_gain(0.5f, 0.0f) == 0.5f, "pressure gain 0 -> identity");
        // 速度映射锚点：y(45)=0.5、斜率 0.015。
        brush::SpeedMapping sm = brush::make_speed_mapping(4.0f);
        CHECK(std::fabs(sm.map(45.0f) - 0.5f) < 1e-4f, "speed mapping y(45)=0.5");
        CHECK(std::fabs(sm.map(46.0f) - sm.map(45.0f) - 0.015f) < 1e-3f,
              "speed mapping slope ~0.015");
        // 传感器状态机：等速输入 → speed1_slow 单调逼近。
        brush::SensorState st;
        for (int i = 0; i < 20; ++i) {
            st.update(1.0f, 0.0f, 0.01f, 1.0f, 0.04f, 0.8f, 2.0f);
        }
        CHECK(st.norm_speed1_slow > 50.0f, "speed1 slow converges toward norm_speed");
    }

    // ── 6. 多轮 create/stroke/destroy 无泄漏（ASan/LSan 兜底）──
    {
        for (int i = 0; i < 100; ++i) {
            BrushKernel k(123 + (std::uint64_t)i);
            BrushHandle h = k.createBrush(BrushParams{});
            k.beginStroke(h, MakePoint(0, 0, 0.5f, 0));
            for (int j = 1; j <= 8; ++j) {
                (void)k.strokeTo(h, MakePoint(3.0f * (float)j, 0.0f, 0.5f,
                                              (std::uint64_t)j * 10000));
            }
            k.endStroke(h);
            // k 析构自动释放 registry + Brush + RNG。
        }
        CHECK(true, "create/stroke/destroy loop completes");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_brush_kernel] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
