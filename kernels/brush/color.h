#pragma once

#include <cmath>

// 颜色调制（B3-1，MVP）：HSV → straight RGB（浮点 0..1）。
// 对照 libmypaint hsv_to_rgb_float；sRGB 线性化留后续任务（非目标）。

namespace brush {

inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// h ∈ [0,360)，s/v ∈ [0,1]。返回 straight RGB（r/g/b 各 0..1）。
inline void hsv_to_rgb_float(float h, float s, float v, float* r, float* g, float* b) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    s = clamp01(s);
    v = clamp01(v);

    const float c = v * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));

    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (hp < 1.0f) {
        r1 = c; g1 = x; b1 = 0.0f;
    } else if (hp < 2.0f) {
        r1 = x; g1 = c; b1 = 0.0f;
    } else if (hp < 3.0f) {
        r1 = 0.0f; g1 = c; b1 = x;
    } else if (hp < 4.0f) {
        r1 = 0.0f; g1 = x; b1 = c;
    } else if (hp < 5.0f) {
        r1 = x; g1 = 0.0f; b1 = c;
    } else {
        r1 = c; g1 = 0.0f; b1 = x;
    }

    const float m = v - c;
    *r = r1 + m;
    *g = g1 + m;
    *b = b1 + m;
}

}  // namespace brush
