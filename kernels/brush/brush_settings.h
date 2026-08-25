#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// 笔刷设置（B3-1，对照 路线E §3a.3）。
// SettingId 为 MVP 子集（~20 个），响应曲线由控制点 (x,y,slope) 分段线性求值（slope 默认 0）。

namespace brush {

// 输入传感器 ID（对照 MYPAINT_BRUSH_INPUT_* 的 MVP 子集）。
enum class Input : std::uint8_t {
    Pressure,
    Speed1,
    Speed2,
    Random,
    Stroke,
    Direction,
    DirectionAngle,
    TiltDeclination,
    TiltAscension,
    ViewZoom,
    Count
};

enum class SettingId : std::uint8_t {
    RadiusLogarithmic,
    Hardness,
    Softness,
    Opaque,
    OpaqueMultiply,
    OpaqueLinearize,
    DabsPerBasicRadius,
    DabsPerActualRadius,
    DabsPerSecond,
    ColorH,
    ColorS,
    ColorV,
    Speed1Gamma,
    Speed2Gamma,
    Speed1Slowness,
    Speed2Slowness,
    PressureGainLog,
    DirectionFilter,
    OffsetByRandom,
    DabRatio,
    DabAngle,
    Count
};

// 曲线控制点：slope 默认 0 = 分段线性（libmypaint 语义）。
struct MappingPoint {
    float x = 0.0f;
    float y = 0.0f;
    float slope = 0.0f;
};

// 响应曲线求值：给定输入 x，分段线性插值输出 y（控制点需按 x 递增）。
// slope 为 0 时退化为纯分段线性；非零 slope 时相邻段做 Hermite 平滑（MVP 仅支持 slope=0 语义，
// 保留参数位供后续）。
inline float eval_curve(const std::vector<MappingPoint>& pts, float x) {
    if (pts.empty()) {
        return 0.0f;
    }
    if (pts.size() == 1 || x <= pts.front().x) {
        return pts.front().y;
    }
    const auto& last = pts.back();
    if (x >= last.x) {
        return last.y;
    }
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const MappingPoint& a = pts[i - 1];
        const MappingPoint& b = pts[i];
        if (x <= b.x) {
            const float span = b.x - a.x;
            if (span <= 0.0f) {
                return b.y;
            }
            const float t = (x - a.x) / span;
            // slope=0 时线性；slope 非零用 Catmull-Rom 风格的切线平滑（MVP 默认 0）。
            if (a.slope == 0.0f && b.slope == 0.0f) {
                return a.y + (b.y - a.y) * t;
            }
            const float t2 = t * t;
            const float t3 = t2 * t;
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            const float h10 = t3 - 2.0f * t2 + t;
            const float h01 = -2.0f * t3 + 3.0f * t2;
            const float h11 = t3 - t2;
            const float m0 = a.slope * span;
            const float m1 = b.slope * span;
            return h00 * a.y + h10 * m0 + h01 * b.y + h11 * m1;
        }
    }
    return last.y;
}

// 单个设置：base_value + 各输入的一条响应曲线（MVP 通常只填 0~1 条）。
struct BrushSetting {
    float base_value = 0.0f;
    std::array<std::vector<MappingPoint>, static_cast<std::size_t>(Input::Count)> curves;

    // 求某输入下的曲线贡献（不含 base_value）。
    float eval(Input in, float x) const {
        return eval_curve(curves[static_cast<std::size_t>(in)], x);
    }
};

}  // namespace brush
