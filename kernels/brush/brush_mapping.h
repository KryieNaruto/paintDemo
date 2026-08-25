#pragma once

#include <cmath>

// 速度映射（B3-1，对照 路线E §3a.3）：速度 → 传感器值 y = log(gamma + speed) * m + q。
// 锚点 fix1_x=45 / fix1_y=0.5 / fix2_x=45 / fix2_dy=0.015（libmypaint 固定校准点）。
//   m = fix2_dy * (gamma + fix2_x)
//   q = fix1_y - log(gamma + fix1_x) * m
// 使 y(45)=0.5 且 y'(45)=0.015。

namespace brush {

constexpr float kFix1X = 45.0f;
constexpr float kFix1Y = 0.5f;
constexpr float kFix2X = 45.0f;
constexpr float kFix2Dy = 0.015f;

struct SpeedMapping {
    float gamma = 1.0f;
    float m = 0.0f;
    float q = 0.0f;

    float map(float speed) const {
        return std::log(gamma + speed) * m + q;
    }
};

inline SpeedMapping make_speed_mapping(float gamma) {
    SpeedMapping sm;
    sm.gamma = gamma > 0.001f ? gamma : 0.001f;
    sm.m = kFix2Dy * (sm.gamma + kFix2X);
    sm.q = kFix1Y - std::log(sm.gamma + kFix1X) * sm.m;
    return sm;
}

}  // namespace brush
