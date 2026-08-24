#pragma once

#include <cstdint>

// L0 共享内部类型。本文件只供 SDK 内部使用，不属于对外 C API。

struct StrokePoint { float x, y, pressure, tilt_x, tilt_y; std::uint64_t t_us; bool is_predicted; };
struct BrushParams { float radius, hardness, opacity; /* 颜色/纹理按需扩 */ };
struct StampData   { float x, y, radius, hardness, opacity; /* alpha 形状位图 */ };

// 接口签名（§4.0）引用但未定义的两个不透明类型，本任务落在 core/types.h，
// 不引入平台抽象接口。
//   BrushHandle      = 不透明整型句柄，Null 实现返回 0。
//   PlatformSurface  = 不透明原生窗口句柄；Null/headless 下 init 可接受 nullptr。
using BrushHandle = std::uint64_t;
using PlatformSurface = void*;
