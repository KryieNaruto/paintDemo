#pragma once

// B5-2 CLI 宿主：JSON 批处理脚本解释器（§4.0.6）。
//
// CLI 是 SDK C API 的第一个 host-only 宿主，编译期只 #include 公开头
// sdk_api/dgc_paint_c_api.h，不 include core/render/kernels 任何内部头。
// 本头只暴露 runScript 入口，op 数据结构留在 script_runner.cpp 内部。

// 运行 JSON 批处理脚本：
//   jsonPath    脚本文件路径（JSON）
//   outOverride --out 覆盖脚本内 export 路径；nullptr 表示不覆盖
//   strict      true 时任一 op 失败即中止（非零退出）；false 为 best-effort 继续（默认）
// 返回进程退出码：0 成功；非 0 失败（见 script_runner.cpp 的 ExitCode）。
int runScript(const char* jsonPath, const char* outOverride, bool strict);
