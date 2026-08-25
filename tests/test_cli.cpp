/* B5-2 host ctest：CLI 集成测试。
 *
 * 按 plan-review 反馈 2，测试目标「shell 调 dgc_cli 二进制」，不把 cli/script_runner.cpp
 * 编译进本目标（避免 include 路径口径漂移）。本目标只 #include 公开头 dgc_paint_c_api.h
 * 链 dgc_paint，用于离屏能力探测；真正对 CLI 的断言全部经 dgc_cli 二进制退出码 / 产出文件。
 *
 * 覆盖：
 *   1) --help 退出码 0；缺参 / 缺脚本 / 脚本不存在 / 非法 JSON / 未知 op 非零退出 + stderr 非空。
 *   2) 全操作闭环脚本（script_basic.json）→ 退出码 0 + 产出 PNG（stb 魔数 0x89 P N G）。
 *   3) 离屏能力探测：Null 后端（无 Vulkan）时跳过 PNG 断言，其余仍测。
 */
#include <cstdio>
#include <cstdlib>
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

    // 6) 未知 op：非零退出 + stderr 非空。
    const std::string unknownOp = "/tmp/b5_2_unknown_op.json";
    CHECK(writeFile(unknownOp,
                    "{ \"canvas\": {\"w\":64,\"h\":64,\"background\":[1,1,1,1]}, "
                    "\"ops\": [{\"op\":\"bogus\"}] }"),
          "write unknown-op fixture");
    rc = runCli("run " + unknownOp, &err);
    CHECK(rc != 0, "unknown op exits non-zero");
    CHECK(!err.empty(), "unknown op writes stderr");

    // 7) 全操作闭环 + 离屏 PNG 产出（Vulkan 后端才成立；Null 后端跳过）。
    if (offscreen) {
        const std::string outPng = "/tmp/b5_2_test_out.png";
        ::remove(outPng.c_str());
        rc = runCli("run \"" + fixture("script_basic.json") + "\" --out " + outPng,
                    nullptr);
        CHECK(rc == 0, "full script exits 0");
        CHECK(fileExists(outPng), "PNG file produced");
        CHECK(isPng(outPng), "PNG magic valid");
        ::remove(outPng.c_str());
    } else {
        std::fprintf(stderr,
                     "[test_cli] SKIP PNG assertion (offscreen unsupported)\n");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_cli] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
