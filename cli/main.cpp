// dgc_cli 入口（B5-2，§4.0.6）：JSON 批处理脚本解释器 + 离屏渲染 → PNG 导出。
// host-only，只 #include 公开头，不 include core/render/kernels 内部头。

#include <cstdio>
#include <string>

#include "script_runner.h"

namespace {

void printUsage() {
    std::fprintf(stdout,
        "用法:\n"
        "  dgc_cli run <script.json> [--out <result.png>] [--strict]\n"
        "  dgc_cli --help\n"
        "\n"
        "run 子命令解释执行 JSON 批处理脚本（§4.0.6）：\n"
        "  建画布 → 选笔刷 → 调参 → 选色 → 画笔迹 → 清空 → 撤销 → 导出 PNG。\n"
        "\n"
        "顶层字段：\n"
        "  canvas {w,h,background}   建离屏画布 + 清底色\n"
        "  seed N / fixed-time T     确定性接线（dgcSetRandomSeed / dgcSetFixedTime）\n"
        "\n"
        "支持 op（§4.0.4 映射）：\n"
        "  load-brush {id,path}      加载 myb 笔刷（当前 NOT_IMPLEMENTED）\n"
        "  set-brush {id}            选中笔刷\n"
        "  set-color {r,g,b,a}       选色\n"
        "  set-param {setting,value} 调参（radius/hardness/opacity/radius_logarithmic）\n"
        "  stroke {points:[...]}     画笔迹\n"
        "  clear                     清空画布\n"
        "  undo                      撤销（当前 NOT_IMPLEMENTED）\n"
        "  export {path}             导出 PNG（--out 覆盖路径）\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;  // EXIT_USAGE
    }

    const std::string sub = argv[1];
    if (sub == "--help" || sub == "-h" || sub == "help") {
        printUsage();
        return 0;
    }
    if (sub != "run") {
        std::fprintf(stderr, "dgc_cli: 未知子命令 '%s'（可用: run / --help）\n", sub.c_str());
        printUsage();
        return 1;
    }

    // run <script.json> [--out <png>] [--strict]
    const char* scriptPath  = nullptr;
    const char* outOverride = nullptr;
    bool strict = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dgc_cli: --out 缺少参数\n");
                return 1;
            }
            outOverride = argv[++i];
        } else if (a == "--strict") {
            strict = true;
        } else if (!scriptPath) {
            scriptPath = argv[i];
        } else {
            std::fprintf(stderr, "dgc_cli: 未知参数 '%s'\n", a.c_str());
            return 1;
        }
    }

    if (!scriptPath) {
        std::fprintf(stderr, "dgc_cli: 缺少脚本路径\n");
        printUsage();
        return 1;
    }

    return runScript(scriptPath, outOverride, strict);
}
