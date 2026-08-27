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

// RGB → HSV 反变换（r/g/b ∈ [0,1] → h ∈ [0,360)、s/v ∈ [0,1]）。
// 供 dgcSetBrushColor 桥接用：C API 收 straight RGBA，内核 dab 用 HSV 调制。
// 边界：max==min（灰度，含纯黑/纯白）→ s=0、h=0，避免除零 / fmod 噪声（风险 R5）。
inline void rgb_to_hsv_float(float r, float g, float b, float* h, float* s, float* v) {
    r = clamp01(r);
    g = clamp01(g);
    b = clamp01(b);
    const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const float delta = mx - mn;
    *v = mx;
    if (delta <= 0.0f) {
        *h = 0.0f;
        *s = 0.0f;
        return;
    }
    *s = delta / mx;
    float hh = 0.0f;
    if (mx == r) {
        hh = (g - b) / delta;
    } else if (mx == g) {
        hh = (b - r) / delta + 2.0f;
    } else {
        hh = (r - g) / delta + 4.0f;
    }
    hh *= 60.0f;
    if (hh < 0.0f) {
        hh += 360.0f;
    }
    *h = hh;
}

}  // namespace brush
