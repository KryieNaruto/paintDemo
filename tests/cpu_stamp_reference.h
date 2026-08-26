#pragma once

// B4-1 CPU 参考 oracle：独立于 vk_backend.cpp 的 dab 光栅化 + over 合成，
// 供 test_gpu_dab_raster 做 CPU/GPU 像素对照。仅测试侧链接，不进 dgc_paint 库。
//
// 与 brush_composite.comp 的覆盖公式逐像素一致：
//   - 像素中心对齐：distance((c+0.5) - pos) / max(radius, 1e-3)（不复用旧 d×d stamp 网格）。
//   - hardness/softness 软硬边：edge = hardness*(1-softness)，dist<=edge 全覆盖，
//     edge<dist<1 走 smoothstep 斜坡。
//   - 外缘 AA 用解析宽度 ww = 1/max(radius,1e-3)（等价 GPU 无 derivative 的兜底路径）。
//   - over 合成用 float r/g/b 直算（不先 to8 再合成），最后一次性量化到 RGBA8
//     （等价 GPU rgba8 UNORM imageStore 的舍入）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

#include "core/types.h"

namespace cpu_ref {

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// GLSL smoothstep 的 CPU 等价（t = clamp((x-e0)/(e1-e0),0,1) → t*t*(3-2t)）。
inline float Smoothstep(float edge0, float edge1, float x) {
    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

inline std::uint8_t To8(float v) {
    return (std::uint8_t)(Clamp01(v) * 255.0f + 0.5f);
}

// 对单个 StampData 在不透明白底 RGBA8 画布上做一次 over 合成（对齐 GPU shader）。
inline void CompositeOverCpu(std::uint8_t* canvas, int w, int h, const StampData& s) {
    const float denom = std::max(s.radius, 1e-3f);
    const float edge = s.hardness * (1.0f - s.softness);
    const float srcR = s.r;
    const float srcG = s.g;
    const float srcB = s.b;
    const float opacity = s.opacity;
    // 解析 AA 宽度（R1 兜底）：归一化下 1px 带宽，与 GPU 无 derivative 路径一致。
    const float ww = 1.0f / denom;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float px = (float)x + 0.5f - s.x;
            const float py = (float)y + 0.5f - s.y;
            const float dist = std::sqrt(px * px + py * py) / denom;
            float coverage = 0.0f;
            if (dist <= edge) {
                coverage = 1.0f;
            } else if (dist < 1.0f) {
                coverage = 1.0f - Smoothstep(edge, 1.0f, dist);
            }
            coverage *= 1.0f - Smoothstep(1.0f - ww, 1.0f + ww, dist);
            const float a = coverage * opacity;
            if (a <= 0.001f) {
                continue;
            }
            const std::size_t i = ((std::size_t)y * (std::size_t)w + (std::size_t)x) * 4;
            const float dstR = (float)canvas[i + 0] / 255.0f;
            const float dstG = (float)canvas[i + 1] / 255.0f;
            const float dstB = (float)canvas[i + 2] / 255.0f;
            const float dstA = (float)canvas[i + 3] / 255.0f;
            // premultiplied over（float 直算，与 GPU 一致）。
            canvas[i + 0] = To8(srcR * a + dstR * (1.0f - a));
            canvas[i + 1] = To8(srcG * a + dstG * (1.0f - a));
            canvas[i + 2] = To8(srcB * a + dstB * (1.0f - a));
            canvas[i + 3] = To8(a + dstA * (1.0f - a));
        }
    }
}

}  // namespace cpu_ref
