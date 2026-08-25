#pragma once

#include <cmath>

// 传感器滤波（B3-1，对照 路线E §3a.1）。
// 纯函数 / 小状态机，header-only 便于单测。角度单位均为「度」。

namespace brush {

constexpr float kPi = 3.14159265358979323846f;

// 压力 gain：pressure * exp(pressure_gain_log)（对照 INPUT(Pressure)）。
inline float apply_pressure_gain(float pressure, float pressure_gain_log) {
    return pressure * std::exp(pressure_gain_log);
}

// 指数低通步进系数：fac = 1 - exp(-dtime/tau)；tau <= 0.001 时立即收敛（fac=1）。
inline float lowpass_fac(float tau, float dtime) {
    if (tau <= 0.001f) {
        return 1.0f;
    }
    return 1.0f - std::exp(-dtime / tau);
}

// 速度归一化（对照 norm_dx/norm_dy/norm_speed）：先除以 viewzoom，再取模长。
inline float norm_speed(float dx, float dy, float dtime, float viewzoom) {
    if (dtime <= 0.0f) {
        return 0.0f;
    }
    const float nx = dx / dtime * viewzoom;
    const float ny = dy / dtime * viewzoom;
    return std::hypot(nx, ny);
}

// tilt → declination：90 - rad2deg(atan2(hypot(xtilt, ytilt), 1))。
inline float tilt_declination(float xtilt, float ytilt) {
    return 90.0f - std::atan2(std::hypot(xtilt, ytilt), 1.0f) * (180.0f / kPi);
}

// tilt → ascension：DEGREES(atan2(-xtilt, ytilt))。
inline float tilt_ascension(float xtilt, float ytilt) {
    return std::atan2(-xtilt, ytilt) * (180.0f / kPi);
}

// direction 距离低通步进系数（对照 180° 对称向量滤波）：fac = 1 - exp(-(exp(filter*0.5)-1)*dtime)。
inline float direction_fac(float filter, float dtime) {
    return 1.0f - std::exp(-(std::exp(filter * 0.5f) - 1.0f) * dtime);
}

// 一阶方向向量低通（维护单位向量方向的平滑）。
inline void update_direction(float& dx, float& dy, float step_dx, float step_dy,
                             float filter, float dtime) {
    const float fac = direction_fac(filter, dtime);
    dx += (step_dx - dx) * fac;
    dy += (step_dy - dy) * fac;
}

// 传感器状态机（速度 slow1/slow2 指数低通 + 180°/360° 方向向量）。
// 由 Brush 每步持有与更新，独立成结构便于单测「结构断言」。
struct SensorState {
    float norm_speed1_slow = 0.0f;
    float norm_speed2_slow = 0.0f;
    float direction_dx = 0.0f;   // 180° 对称（方向，无朝向）
    float direction_dy = 0.0f;
    float direction_angle_dx = 0.0f;  // 360°（含朝向）
    float direction_angle_dy = 0.0f;

    void reset() {
        norm_speed1_slow = 0.0f;
        norm_speed2_slow = 0.0f;
        direction_dx = 0.0f;
        direction_dy = 0.0f;
        direction_angle_dx = 0.0f;
        direction_angle_dy = 0.0f;
    }

    // 更新：dx/dy 为本步位移（px），dtime 为步长时间（秒）。
    void update(float dx, float dy, float dtime, float viewzoom,
                float speed1_slowness, float speed2_slowness, float direction_filter) {
        const float speed = norm_speed(dx, dy, dtime, viewzoom);

        const float fac1 = lowpass_fac(speed1_slowness, dtime);
        norm_speed1_slow += (speed - norm_speed1_slow) * fac1;
        const float fac2 = lowpass_fac(speed2_slowness, dtime);
        norm_speed2_slow += (speed - norm_speed2_slow) * fac2;

        const float len = std::hypot(dx, dy);
        if (len > 1e-6f) {
            const float nx = dx / len;
            const float ny = dy / len;
            // 180° 对称：方向向量折叠到同一象限（取绝对值分量再按主方向定向）。
            update_direction(direction_dx, direction_dy, nx, ny, direction_filter, dtime);
            // 360°：直接按朝向。
            update_direction(direction_angle_dx, direction_angle_dy, nx, ny, direction_filter, dtime);
        }
    }
};

}  // namespace brush
