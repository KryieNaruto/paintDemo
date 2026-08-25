#pragma once

#include <cstdint>

// L0 共享内部类型。本文件只供 SDK 内部使用，不属于对外 C API。

struct StrokePoint { float x, y, pressure, tilt_x, tilt_y; std::uint64_t t_us; bool is_predicted; };
struct BrushParams { float radius, hardness, opacity; /* 颜色/纹理按需扩 */ };
// B3-1 扩展：r/g/b 为 straight RGB（0..1，颜色调制产物），softness 为外缘软化斜坡（0..1）。
// 均带默认值，兼容既有 5 字段聚合初始化（test_offscreen 等按旧 5 字段构造时 r/g/b/softness 归 0）。
struct StampData   {
    float x, y, radius, hardness, opacity;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float softness = 0.0f;
};

// 接口签名（§4.0）引用但未定义的两个不透明类型，本任务落在 core/types.h，
// 不引入平台抽象接口。
//   BrushHandle      = 不透明整型句柄，Null 实现返回 0。
//   PlatformSurface  = 不透明原生窗口句柄；Null/headless 下 init 可接受 nullptr。
using BrushHandle = std::uint64_t;
using PlatformSurface = void*;
