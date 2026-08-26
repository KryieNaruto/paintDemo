# paint-pc Windows MSVC Debug 链接失败：shaderc_combined.lib CRT 不匹配（LNK2038）

> - 任务线：线5-闭环（消费者构建闭环）/ bugfix-pipeline
> - Bug 报告：paint-pc 自动构建脚本（`scripts/setup.sh` / `scripts/setup.ps1`）在 Windows MSVC **Debug** 下报
>   `FAILED: [code=4294967295] paint_pc.exe`，`LINK Pass 1 ... failed (exit code 1319)`。
> - 状态：计划

---

## ⚠️ 复现（① 问题查找结论）

### 复现命令（用户 Windows 真机，已执行并报错）

```
scripts/setup.sh   （或 setup.ps1；等价构建命令）
  → build/_setup_msvc.bat：
      call "<vs>\VC\Auxiliary\Build\vcvars64.bat"
      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF
      cmake --build build -j
```

### 期望 vs 实际

| | 内容 |
|---|---|
| 期望 | `build/paint_pc.exe` 链接成功 |
| 实际 | `LNK2038: 检测到“_ITERATOR_DEBUG_LEVEL”的不匹配项: 值“0”不匹配值“2”(app.cpp.obj 中)`；`LNK2038: 检测到“RuntimeLibrary”的不匹配项: 值“MD_DynamicRelease”不匹配值“MDd_DynamicDebug”(app.cpp.obj 中)` |

### 根因（实证）

- paint-pc（`app.cpp.obj` 等）与 SDK `dgc_paint` 全部对象按 `-DCMAKE_BUILD_TYPE=Debug` 编译 → MSVC 默认 `/MDd`
  （`MDd_DynamicDebug`，`_ITERATOR_DEBUG_LEVEL=2`）。
- `render/vulkan/CMakeLists.txt` Windows 分支 `find_library(SHADERC_LIBRARY NAMES shaderc_combined ...)`
  找到 LunarG Vulkan SDK 的 **Release-only** `shaderc_combined.lib`（MSVC `/MD`，`MD_DynamicRelease`，
  `_ITERATOR_DEBUG_LEVEL=0`）。
- MSVC 链接器对进入同一 exe 的每个对象强校验 CRT/迭代器调试级别 → 三者（Debug /MDd 对象 + Release /MD 静态库）
  互相冲突 → LNK2038，链接 pass 退出码 1319。
- **确认无既有处理**：`grep CMAKE_MSVC_RUNTIME_LIBRARY` 于 consumer CMakeLists / SDK 顶层 / render/vulkan
  三处均无命中——该守卫从未存在，属新增缺陷（非回归）。

### 影响面（共用出错代码路径）

1. **consumer paint-pc**（本 bug 现场）：`add_subdirectory(sdk)` → `dgc_paint` 链接 `shaderc_combined.lib` →
   `paint_pc.exe` 链接失败。
2. **SDK standalone host-windows**（`demo/CMakePresets.json` 的 `host-windows` preset，`CMAKE_BUILD_TYPE=Debug`）：
   同样的 `dgc_paint` + shaderc_combined → 其自身 tests 链接同样 LNK2038。**同一潜在缺陷**，应一并修复。
3. Linux host / Android：不受影响（非 MSVC，且 Android 走 glslc 预编译 SPIR-V，不链 shaderc）。

### 为什么是「MSVC 默认多出 Debug 不匹配」而非「改 Release」？

改 `-DCMAKE_BUILD_TYPE=Release` 会让 paint-pc / dgc_paint 全部对象切 /MD，绕过本错，但：
- 丢失 Debug（断言/符号/确定性排障）能力，且把「全链一套 CRT」的约束押在调用方（每个调用方都要记得），
  与「自动化脚本默认 Debug」的设计（`make_msvc_build_bat` 硬编码 Debug）相冲突；
- 不能消除「只要有人用 Debug 建 → 立刻 LNK2038」的根本脆弱性。
故修复选**正路**：MSVC 下显式钉 Release /MD 运行时（与 LunarG 预编译 Release 库匹配）。

---

## 修复方案

### 根因（一句话）

MSVC Debug 构建下，`shaderc_combined.lib`（LunarG Release /MD）与 paint-pc/SDK Debug /MDd 对象 CRT 不匹配 → LNK2038。

### 修复（两处，同一机制）

**机制**：若编译器等 MSVC 且未显式覆盖，则把 `CMAKE_MSVC_RUNTIME_LIBRARY` 设为
`MultiThreadedDLL`（即 `/MD`，Release CRT），与 LunarG 预编译 shaderc_combined.lib 匹配。
**守卫块抽成共享模块 `cmake/msvc_runtime.cmake`**，SDK 顶层 `demo/CMakeLists.txt` 经 `include()` 引入；
consumer 经 `add_subdirectory(sdk)` 继承，一处生效、两仓受益。模块同时被回归用例 include
（见「回归用例设计」），保证 **ctest 测的是真实修复逻辑，而非副本**。

```cmake
# cmake/msvc_runtime.cmake
# render/vulkan 的 shaderc_combined.lib（LunarG SDK）是 MSVC Release(/MD) 预编译，
# 而 Debug 默认 /MDd → LNK2038。MSVC 下显式钉 Release /MD 运行时与之一致。
# 仅当用户未显式设置 CMAKE_MSVC_RUNTIME_LIBRARY 时生效（尊重调用方显式覆盖）。
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
endif()
```

放置位置：`demo/CMakeLists.txt` 在 `project()` 之后、`add_library(dgc_paint ...)` 之前 `include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/msvc_runtime.cmake)`。

**consumer 侧**（paint-pc）**不需要改 CMake**——consumer `add_subdirectory(sdk)` 已继承；只需
**前移 SDK submodule 钉到含修复的 commit**。两处改动均落在 **SDK（demo）仓库**，consumer 仅 bump submodule。

### 为什么作用域放 SDK 顶层而非 render/vulkan/CMakeLists.txt

- `CMAKE_MSVC_RUNTIME_LIBRARY` 是目录作用域缓存变量；`target_compile_options(/MD)` 只在 dgc_paint 一个 target 上，
  但 consumer 的 `paint_pc`、`paint_imgui`、glfw 也是同一 exe 的一部分——它们若继续 /MDd，LNK2038 依旧。
  放顶层 = 全构建（consumer 的全部 targets）统一，根治。
- SDK 是 submodule，是「三处调用方（paint-pc / paint-android / standalone）」的共同底座，放 SDK 顶层一处生效。

### 边界与不变量

- 仅 `MSVC` 判定下生效；Linux/Android/AppleClang 路径完全不变（host-linux 与 android-arm64 构建零改动）。
- 若调用方显式 `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL`（真有 Debug shaderc），
  尊重之，不强改（`NOT DEFINED` 守卫）。
- 不改 `DGCPAIN_BUILD_TYPE`、不动 shaderc 链接路径（保持 LunarG SDK 布局）、不引入回退/兜底路径。

---

## 回归用例设计（先红后绿）

**约束**：本机（Linux）无 MSVC，无法直接编译/链接复现 LNK2038；回归用例必须在**无头**下可验证修复逻辑。

### 方案 A（配置期断言，可无头执行）—— 主回归用例

在 SDK `tests/` 新增 ctest：`test_msvc_runtime_debug_guard`（host 可跑，无编译器依赖）。

**做法**：回归脚本 `tests/test_msvc_runtime_guard.cmake` 用 `cmake -P` 脚本模式，
`set(MSVC ON)` 模拟 MSVC 判定，然后 **include 真实修复模块** `cmake/msvc_runtime.cmake`，
断言执行后 `CMAKE_MSVC_RUNTIME_LIBRARY` 等于 `MultiThreadedDLL`（否则 `FATAL_ERROR`）。

- **红（修复前）**：修复模块不存在/守卫未生效 → include 失败或变量未被置为 `MultiThreadedDLL` → 断言 FATAL_ERROR（失败）。
- **绿（修复后）**：模块生效 → 断言通过。

**关键**：测试的是**真实修复逻辑**（同一份 `cmake/msvc_runtime.cmake` 被顶层 CMakeLists 与回归脚本
共同 include），不是副本；且可在无头环境执行（cmake -P，无 GUI、无编译器、无 MSVC 真机）。

`tests/CMakeLists.txt` 注册：

```cmake
add_test(NAME test_msvc_runtime_debug_guard
         COMMAND ${CMAKE_COMMAND}
                 -DGCPAIN_FIX_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/msvc_runtime.cmake
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/test_msvc_runtime_guard.cmake)
```

### 方案 B（真实 MSVC 链接回归）—— 真机可执行、本机标记 skip

提供 `tests/smoke-msvc-debug.sh`（Windows/Git Bash）或脚本内分支：真机跑
`cmake -B build -DCMAKE_BUILD_TYPE=Debug` + `cmake --build`，断言 `paint_pc.exe` 链接成功（无 LNK2038）。
本机无 MSVC → 自动 skip（记录为 skip，不阻塞），但在 Windows 真机上全绿。

### 影响面核对（修改后全量回归）

- `host-linux`：CMake 全量 configure + ctest 全绿（shaderc 运行时编译路径不动，CPU/GPU 对照 B4-1 保持绿）。
- `android-arm64`：CMake configure + 编 .so（不链 shaderc，不受影响）。
- consumer：Linux smoke.sh（离屏 PNG + 含笔迹像素断言）保持绿。

---

## 验证方式（无头 CLI + 离屏）

| 项 | 命令 | 通过标准 |
|---|---|---|
| 回归用例（方案 A） | `ctest -R msvc_runtime`（host） | 修复后绿；记录修复前红（先红后绿） |
| 全量回归 | `ctest --test-dir build/host-linux` | 0 失败 0 跳过（真实路径） |
| SDK standalone host-windows | 用户在 Windows 跑 SDK `cmake --preset host-windows` + `cmake --build` | dgc_paint 及 tests 链接成功、无 LNK2038 |
| consumer Linux | `cd paint-pc && bash tests/smoke.sh` | 离屏 PNG 导出 + 含笔迹像素断言 PASS |
| 真机 MSVC（方案 B） | 用户在 Windows 跑 `scripts/setup.sh` | `paint_pc.exe` 链接成功、LNK2038 消失 |

---

## 变更清单（预计）

| 文件 | 变更 |
|---|---|
| `demo/cmake/msvc_runtime.cmake` | 新增：共享 MSVC runtime 守卫模块（根因修复本体，顶层与回归用例共同 include） |
| `demo/CMakeLists.txt` | +`include(cmake/msvc_runtime.cmake)`（project() 后、add_library 前） |
| `demo/tests/CMakeLists.txt` | +ctest `test_msvc_runtime_debug_guard`（方案 A 回归，cmake -P + 共享模块） |
| `demo/tests/test_msvc_runtime_guard.cmake` | 新增：回归脚本（set(MSVC ON) 模拟 + include 真实模块 + 断言） |
| `paint-pc` | 仅 bump `sdk` submodule 到含修复 commit（不直接改 CMake） |

## 遗留项 / 风险

- R1：本机无法执行真实 MSVC 链接（方案 B）——真机验证交用户复跑 `scripts/setup.sh`，修复块逻辑已由方案 A 无头覆盖。
- R2：`shaderc_combined.lib` 未来若更新为 Debug 变体，本守卫不破坏（尊重显式覆盖）。
- 不引入任何回退/兜底路径（如「缺 shaderc 就改用预编译 SPIR-V」）。
