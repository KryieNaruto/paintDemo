/* B5-2 host ctest：CLI 集成测试。
 *
 * 按 plan-review 反馈 2，测试目标「shell 调 dgc_cli 二进制」，不把 cli/script_runner.cpp
 * 编译进本目标（避免 include 路径口径漂移）。本目标只 #include 公开头 dgc_paint_c_api.h
 * 链 dgc_paint，用于离屏能力探测与确定性接线直接断言；真正对 CLI 的断言全部经 dgc_cli
 * 二进制退出码 / stderr / 产出文件。
 *
 * 覆盖（对齐 B5-2 验收标准 9 条 + test-review 反馈）：
 *   1) --help 退出码 0；缺参 / 缺脚本 / 脚本不存在 / 非法 JSON / 未知 op 非零退出 + stderr 非空。
 *   2) 全操作闭环脚本（script_basic.json）→ 退出码 0 + 产出 PNG（stb 魔数 0x89 P N G），
 *      并逐 op 断言 stderr 错误行（§4.0.4 映射：每个 op 确实派发到对应 C API）。
 *   3) 确定性接线：直接 C API 断言 dgcSetRandomSeed/dgcSetFixedTime == DGC_OK（验收标准 4），
 *      加 seed+fixed-time-only 脚本 → 退出 0（CLI 透传证据）。
 *   4) 参数化接线：四种 setting 名（radius/hardness/opacity/radius_logarithmic）各喂一次，
 *      断言无「未知 setting」错误行（验收标准 5）。
 *   5) 离屏能力探测：Null 后端（无 Vulkan）时跳过离屏相关断言，其余仍测。
 *   6) 测试卫生：所有脚本统一 --out /tmp/...，测试结束清理，不在 worktree 根留 result.png。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>

#include "dgc_paint_c_api.h"

#ifndef DGC_CLI_PATH
#error "DGC_CLI_PATH 未定义（tests/CMakeLists.txt 应经 target_compile_definitions 注入）"
#endif
#ifndef DGC_FIXTURE_DIR
#error "DGC_FIXTURE_DIR 未定义"
#endif

static int failures = 0;
#define CHECK(cond, name)                                   \
    do {                                                    \
        if (!(cond)) {                                      \
            std::fprintf(stderr, "FAIL: %s\n", name);       \
            ++failures;                                     \
        }                                                   \
    } while (0)

namespace {

std::string fixture(const char* name) {
    return std::string(DGC_FIXTURE_DIR) + "/" + name;
}

// shell 调 dgc_cli 二进制；stderr 重定向到固定临时文件，stderrOut 非空则回读。
// 返回进程退出码（-1 = 启动失败）。
int runCli(const std::string& args, std::string* stderrOut) {
    const std::string errFile = "/tmp/b5_2_cli_stderr.txt";
    const std::string cmd = "\"" + std::string(DGC_CLI_PATH) + "\" " + args +
                            " 2> " + errFile;
    int status = std::system(cmd.c_str());
    if (stderrOut) {
        std::ifstream in(errFile, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        *stderrOut = ss.str();
    }
    if (status == -1 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << content;
    return true;
}

// 校验 PNG 魔数（前 8 字节）。
bool isPng(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    unsigned char magic[8] = {0};
    in.read(reinterpret_cast<char*>(magic), 8);
    return magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G';
}

// 子串出现次数（逐 op 断言「每个 op 确实派发到对应 C API」时用）。
size_t countSubstr(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// 离屏能力探测（Null 后端 → false）。
bool offscreenAvailable() {
    DgcContext* ctx = dgcCreate();
    if (!ctx) {
        return false;
    }
    int rc = dgcSetOffscreenSurface(ctx, 64, 64);
    dgcDestroy(ctx);
    return rc == DGC_OK;
}

}  // namespace

int main() {
    const bool offscreen = offscreenAvailable();

    // 1) --help 退出码 0。
    CHECK(runCli("--help", nullptr) == 0, "--help exits 0");

    // 2) 缺参数（无任何子命令）非零退出。
    CHECK(runCli("", nullptr) != 0, "no args exits non-zero");

    // 3) run 但缺脚本路径非零退出。
    CHECK(runCli("run", nullptr) != 0, "run without script exits non-zero");

    // 4) 脚本文件不存在：非零退出 + stderr 非空。
    std::string err;
    int rc = runCli("run \"" + fixture("nonexistent.json") + "\"", &err);
    CHECK(rc != 0, "missing script file exits non-zero");
    CHECK(!err.empty(), "missing script file writes stderr");

    // 5) 非法 JSON：非零退出 + stderr 非空。
    const std::string badJson = "/tmp/b5_2_bad.json";
    CHECK(writeFile(badJson, "{ not valid json "), "write bad JSON fixture");
    rc = runCli("run " + badJson, &err);
    CHECK(rc != 0, "bad JSON exits non-zero");
    CHECK(!err.empty(), "bad JSON writes stderr");

    // 6) 未知 op：非零退出 + stderr 非空。--out 指向 /tmp（卫生，避免 result.png 落地）。
    const std::string unknownOp = "/tmp/b5_2_unknown_op.json";
    const std::string unknownOpOut = "/tmp/b5_2_unknown_op_out.png";
    ::remove(unknownOpOut.c_str());
    CHECK(writeFile(unknownOp,
                    "{ \"canvas\": {\"w\":64,\"h\":64,\"background\":[1,1,1,1]}, "
                    "\"ops\": [{\"op\":\"bogus\"}] }"),
          "write unknown-op fixture");
    rc = runCli("run " + unknownOp + " --out " + unknownOpOut, &err);
    CHECK(rc != 0, "unknown op exits non-zero");
    CHECK(!err.empty(), "unknown op writes stderr");
    if (offscreen) {
        CHECK(err.find("未知 op") != std::string::npos,
              "unknown op reports unknown-op error");
    }
    ::remove(unknownOpOut.c_str());

    // 验收标准 4：确定性接线「返回 DGC_OK」的直接 C API 证据（无条件，Null 后端也成立）。
    {
        DgcContext* ctx = dgcCreate();
        CHECK(ctx != nullptr, "dgcCreate for determinism wiring");
        if (ctx) {
            CHECK(dgcSetRandomSeed(ctx, 42) == DGC_OK,
                  "dgcSetRandomSeed returns DGC_OK");
            CHECK(dgcSetFixedTime(ctx, 1.0e6) == DGC_OK,
                  "dgcSetFixedTime returns DGC_OK");
            dgcDestroy(ctx);
        }
    }

    if (offscreen) {
        // 验收标准 4（CLI 透传证据）：只含 seed+fixed-time 的脚本 → 退出 0、stderr 无失败行。
        const std::string detScript = "/tmp/b5_2_det.json";
        const std::string detOut = "/tmp/b5_2_det_out.png";
        ::remove(detOut.c_str());
        CHECK(writeFile(detScript,
                        "{ \"canvas\": {\"w\":64,\"h\":64,\"background\":[1,1,1,1]}, "
                        "\"seed\": 42, \"fixed-time\": 1000000, \"ops\": [] }"),
              "write seed/fixed-time-only fixture");
        rc = runCli("run " + detScript + " --out " + detOut, &err);
        CHECK(rc == 0, "seed/fixed-time-only script exits 0");
        CHECK(err.find("失败") == std::string::npos,
              "seed/fixed-time no setter failure in stderr");
        ::remove(detOut.c_str());

        // 验收标准 5 + 3：四种 setting 名 → 无「未知 setting」；每 op 均派发到 dgcSetBrushSetting
        // （当前无有效 brush → 各得 invalid brush handle），证明 string→枚举映射正确。
        const std::string paramScript = "/tmp/b5_2_param.json";
        const std::string paramOut = "/tmp/b5_2_param_out.png";
        ::remove(paramOut.c_str());
        CHECK(writeFile(paramScript,
                        "{ \"canvas\": {\"w\":64,\"h\":64,\"background\":[1,1,1,1]}, "
                        "\"ops\": ["
                        "{\"op\":\"set-param\",\"setting\":\"radius\",\"value\":1.0},"
                        "{\"op\":\"set-param\",\"setting\":\"hardness\",\"value\":0.5},"
                        "{\"op\":\"set-param\",\"setting\":\"opacity\",\"value\":0.8},"
                        "{\"op\":\"set-param\",\"setting\":\"radius_logarithmic\",\"value\":0.9}"
                        "]}"),
              "write 4-settings fixture");
        rc = runCli("run " + paramScript + " --out " + paramOut, &err);
        CHECK(rc == 0, "4-settings script exits 0");
        CHECK(err.find("未知 setting") == std::string::npos,
              "4 settings produce no unknown-setting error");
        CHECK(countSubstr(err, "invalid brush handle") == 4,
              "4 set-param ops each dispatched to dgcSetBrushSetting");
        ::remove(paramOut.c_str());

        // 验收标准 6 + 3：全操作闭环脚本 → 退出 0 + 产出 PNG + 逐 op 错误行断言。
        const std::string outPng = "/tmp/b5_2_test_out.png";
        ::remove(outPng.c_str());
        rc = runCli("run \"" + fixture("script_basic.json") + "\" --out " + outPng,
                    &err);
        CHECK(rc == 0, "full script exits 0");
        CHECK(fileExists(outPng), "PNG file produced");
        CHECK(isPng(outPng), "PNG magic valid");

        // §4.0.4 映射逐条断言：每个 op 的错误行精确匹配，证明派发到对应 C API。
        CHECK(err.find("op 'load-brush' 失败(4): not implemented") != std::string::npos,
              "load-brush dispatched to dgcLoadBrushFromMyb");
        CHECK(err.find("op 'set-brush' 失败(4): not implemented") != std::string::npos,
              "set-brush dispatched to dgcSetBrush");
        CHECK(err.find("op 'undo' 失败(4): not implemented") != std::string::npos,
              "undo dispatched to dgcUndo");
        CHECK(err.find("op 'set-color' 失败(3): invalid brush handle") != std::string::npos,
              "set-color dispatched to dgcSetBrushColor");
        CHECK(err.find("op 'set-param' 失败(3): invalid brush handle") != std::string::npos,
              "set-param dispatched to dgcSetBrushSetting");
        CHECK(err.find("未知 setting") == std::string::npos,
              "full script radius_logarithmic mapped (no unknown-setting)");
        ::remove(outPng.c_str());
    } else {
        std::fprintf(stderr,
                     "[test_cli] SKIP offscreen-dependent assertions (offscreen unsupported)\n");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_cli] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
