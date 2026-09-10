#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// A8-5b 诊断插桩（临时）：DGCPAIN_PERF 构建下把诊断输出送进真机 logcat。
//
// 真机 user 固件不转发 native stderr（fprintf 无输出），Android 平台改走
// __android_log_print（系统日志服务，不受 stdio 重定向策略影响）。与
// core/engine.cpp 顶部既有定义同源；此处抽成头文件供 stroke_predictor.cpp
// 复用，engine.cpp 保持原样不动。非 Android / 未开 DGCPAIN_PERF 时回落
// fprintf(stderr)，host 测试行为不变。
//
// 清理方式：本文件 + 所有 DGCPAIN_PERF_LOG 调用点均为诊断专用，删除不影响功能。
// ─────────────────────────────────────────────────────────────────────────────
#if defined(DGCPAIN_PERF) && defined(__ANDROID__)
#include <android/log.h>
#define DGCPAIN_PERF_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "DGCPAIN_PERF", __VA_ARGS__)
#else
#include <cstdio>
#define DGCPAIN_PERF_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif
