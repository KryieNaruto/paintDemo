#include "kernels/brush/brush.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/determinism.h"
#include "kernels/brush/brush_mapping.h"
#include "kernels/brush/brush_settings.h"
#include "kernels/brush/color.h"
#include "kernels/brush/sensors.h"

namespace {

constexpr float kRadiusMin = 0.1f;
constexpr float kRadiusMax = 512.0f;
constexpr float kDefaultRadius = 10.0f;
constexpr std::size_t kMaxDabsPerSegment = 4096;  // 防御：防病态设置导致死循环

}  // namespace

struct Brush::Impl {
    // 设置 base values（index = SettingId）。
    std::array<float, static_cast<std::size_t>(brush::SettingId::Count)> base_values_{};

    // 压力 → 半径响应曲线（x=归一化压力，y=radius_log 增量），默认空 = 半径不随压力变。
    std::vector<brush::MappingPoint> pressure_radius_curve;

    // 速度映射（speed1/speed2），默认仅预计算、不叠加到半径（速度曲线为空）。
    brush::SpeedMapping speed1_map;
    brush::SpeedMapping speed2_map;

    // 基半径（= exp(base[RadiusLogarithmic])），countDabsTo 的 res2 分母。
    float base_radius = kDefaultRadius;

    // 状态机
    float last_x = 0.0f;
    float last_y = 0.0f;
    std::uint64_t last_t_us = 0;
    bool has_last = false;
    float dabs_moved = 0.0f;   // PartialDabs：累计小数 dab 余量
    float flip = 1.0f;         // 镜像抖动交替（±1）
    float radius_log = 0.0f;   // 当前（含压力调制）radius_log
    float actual_radius = 0.0f;
    brush::SensorState sensors;

    std::unique_ptr<IRandomSource> rng;

    float base(brush::SettingId id) const {
        return base_values_[static_cast<std::size_t>(id)];
    }
    float& base(brush::SettingId id) {
        return base_values_[static_cast<std::size_t>(id)];
    }

    void applyDefaults(const BrushParams& params) {
        // 默认预设（引擎 createBrush(BrushParams{}) 落这里，避免半径 0 产不可见 dab，风险 R3）。
        float radius = params.radius;
        float hardness = params.hardness;
        float opacity = params.opacity;
        if (radius <= 0.0f) {
            radius = kDefaultRadius;
        }
        if (hardness <= 0.0f) {
            hardness = 0.7f;
        }
        if (opacity <= 0.0f) {
            opacity = 1.0f;
        }

        base(brush::SettingId::RadiusLogarithmic) = std::log(radius);
        base(brush::SettingId::Hardness) = hardness;
        base(brush::SettingId::Softness) = 0.0f;
        base(brush::SettingId::Opaque) = opacity;
        base(brush::SettingId::OpaqueMultiply) = 1.0f;
        base(brush::SettingId::OpaqueLinearize) = 0.0f;
        base(brush::SettingId::DabsPerBasicRadius) = 4.0f;
        base(brush::SettingId::DabsPerActualRadius) = 0.0f;
        base(brush::SettingId::DabsPerSecond) = 0.0f;
        base(brush::SettingId::ColorH) = 0.0f;  // 黑色基础色
        base(brush::SettingId::ColorS) = 0.0f;
        base(brush::SettingId::ColorV) = 0.0f;
        base(brush::SettingId::Speed1Gamma) = 4.0f;
        base(brush::SettingId::Speed2Gamma) = 4.0f;
        base(brush::SettingId::Speed1Slowness) = 0.04f;
        base(brush::SettingId::Speed2Slowness) = 0.8f;
        base(brush::SettingId::PressureGainLog) = 0.0f;
        base(brush::SettingId::DirectionFilter) = 2.0f;
        base(brush::SettingId::OffsetByRandom) = 0.0f;
        base(brush::SettingId::DabRatio) = 1.0f;
        base(brush::SettingId::DabAngle) = 0.0f;

        base_radius = std::exp(base(brush::SettingId::RadiusLogarithmic));
        speed1_map = brush::make_speed_mapping(base(brush::SettingId::Speed1Gamma));
        speed2_map = brush::make_speed_mapping(base(brush::SettingId::Speed2Gamma));
        radius_log = base(brush::SettingId::RadiusLogarithmic);
        actual_radius = clampRadius(std::exp(radius_log));
    }

    static float clampRadius(float r) {
        if (r < kRadiusMin) {
            return kRadiusMin;
        }
        if (r > kRadiusMax) {
            return kRadiusMax;
        }
        return r;
    }

    static float clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // 传感器状态 + 半径调制推进（对照 update_states_and_setting_values 的 MVP 子集）。
    void updateStates(float x, float y, float dtime, const StrokePoint& p) {
        const float dx = x - last_x;
        const float dy = y - last_y;
        sensors.update(dx, dy, dtime, /*viewzoom=*/1.0f, base(brush::SettingId::Speed1Slowness),
                       base(brush::SettingId::Speed2Slowness),
                       base(brush::SettingId::DirectionFilter));

        const float p_gain =
            brush::apply_pressure_gain(p.pressure, base(brush::SettingId::PressureGainLog));
        const float curve = brush::eval_curve(pressure_radius_curve, p_gain);
        radius_log = base(brush::SettingId::RadiusLogarithmic) + curve;
        actual_radius = clampRadius(std::exp(radius_log));

        last_x = x;
        last_y = y;
    }

    // dab 数量（含小数）：三设置求和，res4<0 或 NaN 归 0（对照 count_dabs_to，源码确认求和）。
    float countDabsTo(float x, float y, float dt) {
        const float dx = x - last_x;
        const float dy = y - last_y;
        // MVP：actualEllipticalDabRatio=1（圆形 dab），dist = hypot(dx,dy)。
        const float dist = std::hypot(dx, dy);

        const float denom_a = actual_radius > 0.0f ? actual_radius : 1.0f;
        const float denom_b = base_radius > 0.0f ? base_radius : 1.0f;
        const float res1 = dist / denom_a * base(brush::SettingId::DabsPerActualRadius);
        const float res2 = dist / denom_b * base(brush::SettingId::DabsPerBasicRadius);
        const float res3 = dt * base(brush::SettingId::DabsPerSecond);
        float res4 = res1 + res2 + res3;
        if (std::isnan(res4) || res4 < 0.0f) {
            res4 = 0.0f;
        }
        return res4;
    }

    // 单个 dab 参数产出（对照 prepare_and_draw_dab）：半径/硬度/不透明度/HSV→RGB 颜色调制。
    void prepareAndDrawDab(float x, float y, const StrokePoint& p, std::vector<StampData>& out) {
        const float opaque = clamp01(base(brush::SettingId::Opaque) *
                                     base(brush::SettingId::OpaqueMultiply));
        const float radius = clampRadius(std::exp(radius_log));
        const float hardness = clamp01(base(brush::SettingId::Hardness));
        const float softness = clamp01(base(brush::SettingId::Softness));

        float r = 0.0f, g = 0.0f, b = 0.0f;
        brush::hsv_to_rgb_float(base(brush::SettingId::ColorH), base(brush::SettingId::ColorS),
                                base(brush::SettingId::ColorV), &r, &g, &b);

        // 位置抖动（offset_by_random）：高斯随机偏移，可观察 RNG 消耗（默认 0 = 无抖动）。
        float ox = 0.0f;
        float oy = 0.0f;
        const float jitter = base(brush::SettingId::OffsetByRandom);
        if (jitter > 0.0f) {
            ox = static_cast<float>(rng->nextGauss()) * jitter * radius;
            oy = static_cast<float>(rng->nextGauss()) * jitter * radius;
        }

        StampData s{};
        s.x = x + ox;
        s.y = y + oy;
        s.radius = radius;
        s.hardness = hardness;
        s.opacity = opaque;
        s.r = r;
        s.g = g;
        s.b = b;
        s.softness = softness;
        out.push_back(s);
    }
};

Brush::Brush(std::unique_ptr<IRandomSource> rng, const BrushParams& params)
    : impl_(std::make_unique<Impl>()) {
    impl_->rng = std::move(rng);
    impl_->applyDefaults(params);
}

Brush::~Brush() = default;

void Brush::beginStroke(const StrokePoint& p) {
    impl_->dabs_moved = 0.0f;
    impl_->flip = 1.0f;
    impl_->sensors.reset();
    impl_->last_x = p.x;
    impl_->last_y = p.y;
    impl_->last_t_us = p.t_us;
    impl_->has_last = true;
    impl_->radius_log = impl_->base(brush::SettingId::RadiusLogarithmic);
    impl_->actual_radius = Impl::clampRadius(std::exp(impl_->radius_log));
}

std::vector<StampData> Brush::strokeTo(const StrokePoint& p) {
    std::vector<StampData> out;

    // dtime 单位换算（plan-review 提醒 1）：t_us 为 uint64 微秒，dtime 为秒。
    double dtime = 0.0;
    if (impl_->has_last) {
        const double dt_us = static_cast<double>(p.t_us) - static_cast<double>(impl_->last_t_us);
        dtime = dt_us / 1e6;  // 微秒 → 秒
    }
    if (dtime < 0.0) {
        dtime = 0.0;
    }

    float dabs_moved = impl_->dabs_moved;
    float dabs_todo = impl_->countDabsTo(p.x, p.y, static_cast<float>(dtime));

    std::size_t guard = 0;
    while (dabs_moved + dabs_todo >= 1.0f && guard < kMaxDabsPerSegment) {
        const float frac = (1.0f - dabs_moved) / dabs_todo;
        const float ix = impl_->last_x + (p.x - impl_->last_x) * frac;
        const float iy = impl_->last_y + (p.y - impl_->last_y) * frac;
        impl_->updateStates(ix, iy, static_cast<float>(dtime * static_cast<double>(frac)), p);
        impl_->flip = -impl_->flip;
        impl_->prepareAndDrawDab(ix, iy, p, out);
        impl_->rng->nextUniform();  // random_input（对照 rng_double_next）
        dabs_moved = 0.0f;
        dabs_todo = impl_->countDabsTo(p.x, p.y, static_cast<float>(dtime));
        ++guard;
    }
    impl_->dabs_moved = dabs_moved + dabs_todo;

    // 推进位置到真实点（速度滤波用整段 dtime；MVP 速度不影响 dab，无精度敏感）。
    impl_->updateStates(p.x, p.y, static_cast<float>(dtime), p);
    impl_->last_t_us = p.t_us;
    return out;
}

void Brush::endStroke() {
    // MVP：无收尾 dab（小数余量 flush 归后续任务）。
    impl_->has_last = false;
}

void Brush::setColor(float h, float s, float v) {
    impl_->base(brush::SettingId::ColorH) = h;
    impl_->base(brush::SettingId::ColorS) = s;
    impl_->base(brush::SettingId::ColorV) = v;
}

void Brush::reseed(std::uint64_t seed) {
    if (auto* mt = dynamic_cast<Mt19937Random*>(impl_->rng.get())) {
        mt->seed(seed);
    } else {
        impl_->rng = std::make_unique<Mt19937Random>(seed);
    }
}

void Brush::setPressureRadiusCurve(const std::vector<brush::MappingPoint>& curve) {
    impl_->pressure_radius_curve = curve;
}

void Brush::setBase(brush::SettingId id, float value) {
    impl_->base(id) = value;
    if (id == brush::SettingId::RadiusLogarithmic) {
        impl_->base_radius = std::exp(value);
        impl_->radius_log = value;
        impl_->actual_radius = Impl::clampRadius(std::exp(value));
    } else if (id == brush::SettingId::Speed1Gamma) {
        impl_->speed1_map = brush::make_speed_mapping(value);
    } else if (id == brush::SettingId::Speed2Gamma) {
        impl_->speed2_map = brush::make_speed_mapping(value);
    }
}
