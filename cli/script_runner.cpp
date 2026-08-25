#include "script_runner.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "dgc_paint_c_api.h"     // 唯一对外公开头（Pimpl 边界：不 include 内部头）
#include "nlohmann/json.hpp"     // header-only JSON（third_party/nlohmann/，MIT）

using json = nlohmann::json;

namespace {

// 进程退出码（main.cpp 与 runScript 共享约定）。
enum ExitCode {
    EXIT_OK      = 0,  // 成功
    EXIT_USAGE   = 1,  // 参数错误（缺脚本路径等，由 main.cpp 处理）
    EXIT_IO      = 2,  // 脚本文件读取失败
    EXIT_JSON    = 3,  // JSON 解析失败 / 结构非法
    EXIT_RUNTIME = 4,  // 运行时错误（未知 op / 离屏不可用 / export 失败 / strict 首错）
};

// DgcContext 生命周期 RAII 守卫：异常/提前返回均自动 dgcDestroy。
// 禁止裸 delete / 手动 dgcDestroy（SDK 所有权约束）。
using CtxGuard = std::unique_ptr<DgcContext, decltype(&dgcDestroy)>;

CtxGuard makeContext() {
    return CtxGuard(dgcCreate(), &dgcDestroy);
}

// setting 字符串 → DgcBrushSetting 枚举（§4.0.4 映射；§4.0.6 示例用 radius_logarithmic）。
// 返回 -1 表示未知 setting。
int parseSetting(const std::string& name) {
    if (name == "radius")             return DGC_SETTING_RADIUS;
    if (name == "hardness")           return DGC_SETTING_HARDNESS;
    if (name == "opacity")            return DGC_SETTING_OPACITY;
    if (name == "radius_logarithmic") return DGC_SETTING_RADIUS_LOG;
    return -1;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 画布底色（顶层 canvas.background，缺省白）。
struct CanvasSpec {
    int   w  = 64;
    int   h  = 64;
    float bg[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

bool parseCanvas(const json& doc, CanvasSpec& spec) {
    if (!doc.contains("canvas") || !doc["canvas"].is_object()) {
        return true;  // 无 canvas 用缺省 64x64 白底（best-effort）
    }
    const json& c = doc["canvas"];
    if (c.contains("w")) spec.w = c["w"].get<int>();
    if (c.contains("h")) spec.h = c["h"].get<int>();
    if (c.contains("background") && c["background"].is_array() &&
        c["background"].size() >= 4) {
        for (int i = 0; i < 4; ++i) {
            spec.bg[i] = c["background"][i].get<float>();
        }
    }
    return true;
}

}  // namespace

int runScript(const char* jsonPath, const char* outOverride, bool strict) {
    std::string text;
    if (!readFile(jsonPath, text)) {
        std::fprintf(stderr, "dgc_cli: 无法读取脚本文件: %s\n", jsonPath);
        return EXIT_IO;
    }

    json doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dgc_cli: JSON 解析失败: %s\n", e.what());
        return EXIT_JSON;
    }
    if (!doc.is_object()) {
        std::fprintf(stderr, "dgc_cli: 脚本根必须是 JSON 对象\n");
        return EXIT_JSON;
    }

    CanvasSpec canvas;
    parseCanvas(doc, canvas);

    // 离屏渲染调用链：dgcCreate → SetOffscreenSurface → Clear → [seed/fixed-time]
    // → [ops] → dgcFlush → dgcExportPNG →（RAII 析构 dgcDestroy）。
    CtxGuard ctx = makeContext();
    if (!ctx) {
        std::fprintf(stderr, "dgc_cli: dgcCreate 失败\n");
        return EXIT_RUNTIME;
    }

    int rc = dgcSetOffscreenSurface(ctx.get(), canvas.w, canvas.h);
    if (rc != DGC_OK) {
        // 风险 R4：supportsOffscreen()==false（Null 后端）时优雅失败，不崩溃。
        const char* msg = dgcGetLastError();
        std::fprintf(stderr, "dgc_cli: dgcSetOffscreenSurface 失败: %s\n",
                     msg ? msg : "unknown");
        return EXIT_RUNTIME;
    }
    rc = dgcClear(ctx.get(), canvas.bg[0], canvas.bg[1], canvas.bg[2], canvas.bg[3]);
    if (rc != DGC_OK) {
        const char* msg = dgcGetLastError();
        std::fprintf(stderr, "dgc_cli: dgcClear 失败: %s\n", msg ? msg : "unknown");
        return EXIT_RUNTIME;
    }

    // 确定性透传（B1-6/B1-7 已接线 setter 语义，CLI 不加逻辑；无字段则不调用）。
    if (doc.contains("seed")) {
        dgcSetRandomSeed(ctx.get(), doc["seed"].get<uint64_t>());
    }
    if (doc.contains("fixed-time")) {
        dgcSetFixedTime(ctx.get(), doc["fixed-time"].get<double>());
    }

    // ops 按序解释执行（§4.0.4 全映射）。
    bool hadFatal = false;
    std::string exportPath;                       // "export" op 捕获的路径（末尾统一导出）
    DgcBrush currentBrush = DGC_INVALID_BRUSH;     // 最近选中的笔刷句柄（无 L5 恒 INVALID）
    std::unordered_map<std::string, DgcBrush> brushById;

    std::vector<json> ops;
    if (doc.contains("ops") && doc["ops"].is_array()) {
        ops = doc["ops"].get<std::vector<json>>();
    }

    for (const json& op : ops) {
        if (!op.is_object()) {
            continue;
        }
        const std::string opName = op.value("op", "");
        int  opRc   = DGC_OK;
        bool opFatal = false;

        if (opName == "load-brush") {
            const std::string id   = op.value("id", "");
            const std::string path = op.value("path", "");
            DgcBrush b = dgcLoadBrushFromMyb(ctx.get(), path.c_str());
            brushById[id] = b;
            currentBrush = b;
            opRc = (b == DGC_INVALID_BRUSH) ? DGC_ERR_NOT_IMPLEMENTED : DGC_OK;
        } else if (opName == "set-brush") {
            const std::string id = op.value("id", "");
            auto it = brushById.find(id);
            DgcBrush b = (it != brushById.end()) ? it->second : DGC_INVALID_BRUSH;
            opRc = dgcSetBrush(ctx.get(), b);
            currentBrush = b;
        } else if (opName == "set-color") {
            const float r = op.value("r", 0.0f);
            const float g = op.value("g", 0.0f);
            const float b = op.value("b", 0.0f);
            const float a = op.value("a", 1.0f);
            opRc = dgcSetBrushColor(ctx.get(), currentBrush, r, g, b, a);
        } else if (opName == "set-param") {
            const std::string setting = op.value("setting", "");
            const double value = op.value("value", 0.0);
            const int settingId = parseSetting(setting);
            if (settingId < 0) {
                std::fprintf(stderr, "dgc_cli: op 'set-param' 未知 setting: '%s'\n",
                             setting.c_str());
                opRc = DGC_ERR_INVALID_ARG;
            } else {
                opRc = dgcSetBrushSetting(ctx.get(), currentBrush, settingId, value);
            }
        } else if (opName == "stroke") {
            // 首点 BeginStroke，后续 StrokeTo(isPredicted=0)，末 EndStroke。
            const json& points = op.value("points", json::array());
            if (points.is_array() && !points.empty()) {
                bool first = true;
                for (const json& p : points) {
                    const float x = p.value("x", 0.0f);
                    const float y = p.value("y", 0.0f);
                    const float pressure = p.value("p", 1.0f);
                    const float tiltX = p.value("tiltX", 0.0f);
                    const float tiltY = p.value("tiltY", 0.0f);
                    opRc = first ? dgcBeginStroke(ctx.get(), x, y, pressure, tiltX, tiltY)
                                 : dgcStrokeTo(ctx.get(), x, y, pressure, tiltX, tiltY, 0);
                    if (opRc != DGC_OK) {
                        break;
                    }
                    first = false;
                }
                if (opRc == DGC_OK) {
                    opRc = dgcEndStroke(ctx.get());
                }
            }
        } else if (opName == "clear") {
            opRc = dgcClear(ctx.get(), canvas.bg[0], canvas.bg[1], canvas.bg[2],
                            canvas.bg[3]);
        } else if (opName == "undo") {
            opRc = dgcUndo(ctx.get());
        } else if (opName == "export") {
            // 捕获路径，末尾 flush 后统一导出（保证 flush 先于 export）。
            exportPath = op.value("path", "");
            opRc = DGC_OK;
        } else {
            std::fprintf(stderr, "dgc_cli: 未知 op: '%s'\n", opName.c_str());
            opFatal = true;
        }

        if (opFatal) {
            hadFatal = true;
            if (strict) {
                return EXIT_RUNTIME;  // CtxGuard 析构自动 dgcDestroy
            }
            continue;
        }
        if (opRc != DGC_OK) {
            const char* msg = dgcGetLastError();
            std::fprintf(stderr, "dgc_cli: op '%s' 失败(%d): %s\n", opName.c_str(),
                         opRc, msg ? msg : "unknown");
            if (strict) {
                return EXIT_RUNTIME;
            }
            // best-effort 继续：NOT_IMPLEMENTED / INVALID_HANDLE 属预期（无 L5），不视为 fatal。
        }
    }

    // drain 屏障：把「入队（三线程异步）→ 同步读回」压成确定性 drain（§4.0.3 CLI 离屏同步）。
    dgcFlush(ctx.get());

    const std::string finalPath =
        outOverride ? std::string(outOverride)
                    : (exportPath.empty() ? std::string("result.png") : exportPath);
    const int exportRc = dgcExportPNG(ctx.get(), finalPath.c_str());
    if (exportRc != DGC_OK) {
        const char* msg = dgcGetLastError();
        std::fprintf(stderr, "dgc_cli: dgcExportPNG 失败: %s\n", msg ? msg : "unknown");
        return EXIT_RUNTIME;
    }

    // CtxGuard 析构 → dgcDestroy → engine stop/join（create/destroy 配对，零泄漏）。
    return hadFatal ? EXIT_RUNTIME : EXIT_OK;
}
