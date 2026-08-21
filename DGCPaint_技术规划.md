# DGCamp Paint 原型 · 技术规划（v4.0）

> **版本说明**：
> - **v4.0**（2026-08-21）：仓库收敛为 SDK 基座（`libdgc_paint`），不含 UI 消费者。任务表迁至 [`docs/tasks/任务线.md`](docs/tasks/任务线.md)，先落地环境（E0 线）与最小 SDK 基座（B 线），并**并入 v3.0 的 §4.0.1-4.0.6 技术设计**（SDK C API / 离屏 / 确定性 / CLI 批处理），保持「AI 可编程外壳」主线。
> - **v3.0**（2026-08-21）：新增 **可编程外壳** 章节——SDK C API 边界提前到第一版、PC 离屏渲染（headless Vulkan）、CLI 批处理脚本模式、确定性渲染与图像输出。目标：**AI 能通过命令行完成界面上的所有操作，并输出图片自行对比结果**。
> - **v2.0**（2026-08-20）：基于 5 路线评审结论重写，主线改为路线 E（白盒移植 libmypaint → 自研 C++ 内核 + Vulkan Compute）。
> - **v1.0**：开发环境与测试环境搭建。
> **任务与仓库边界（2026-08-21）**：本 Git 仓库是 SDK 基座（C API + `core/`/`kernels/`/`render/`），不含 UI 消费者。任务 SOT → [`docs/tasks/任务线.md`](docs/tasks/任务线.md)；详情 → [`docs/tasks/detail/`](docs/tasks/detail/)；消费者 submodule → [`docs/git/README.md`](docs/git/README.md)；C API 分析 → [`docs/调研/路线整理.md`](docs/调研/路线整理.md) §7。

---

## Context（背景与目标）

DGCamp Paint 工具的完整需求中，绘画功能是核心。本阶段剔除社交/社区、项目管理、图层、文件读写，只做**绘画原型机**，用于验证一个关键技术假设：

> **libmypaint（笔刷引擎）+ Jetpack Ink（低延迟输入）+ Vulkan Compute Shader（GPU 笔刷合成）三者组合，能否达到 Procreate 级别的绘画手感。**

目标平台为 **Android 平板**。这是一个"技术验证型原型"，不是产品 MVP——核心交付物是「能画 + 能测延迟/帧率」的最小闭环，用于评估技术路线的可行性，而非完成产品功能。

### v3.0 新增目标：AI 可编程外壳

在绘画原型之上增加一条 **AI 可自主驱动的通道**：

> **通过命令行完成界面上的所有操作**（选笔刷 / 调参数 / 选颜色 / 画笔迹 / 清空 / 撤销 / 保存），并在 PC 下以**离屏渲染**（不弹窗口）产出 **PNG 图片结果**，供 AI 自行对比验证。

这使 `kernels/` + `render/` 从「只能被 UI 驱动」升级为「可以被任何宿主驱动」。CLI 是 SDK C API 的第一个宿主，与 Android/PC UI 调同一套 C API，因此**界面操作 ↔ CLI 命令 ↔ C API 三者一一对应**，不存在「CLI 做得到但 UI 做不到」或反向偏差。

**三个新支柱**：

| 支柱 | 内容 | 对应章节 |
|---|---|---|
| ① SDK C API 边界 | `sdk_api/` 提前到第一版，UI 与 CLI 统一走 C API | §4.0.2 |
| ② PC 离屏渲染 | `IRenderBackend` headless 模式，无窗口渲染到离屏 image + 导出 PNG | §4.0.5 |
| ③ 确定性渲染 | seed 注入 + 固定时间戳，同脚本同 seed 像素级可复现 | §4.0.3 |

### 技术选型（已与用户确认）

| 层 | 选型 | 备注 |
|---|---|---|
| 平台 | **Android 平板 + PC（双平台）** | 平台无关核心 + 平台层插拔 |
| UI（Android Pad） | Jetpack Compose | 平板优先，横向布局 |
| UI（PC） | **ImGui**（GLFW 窗口） | 轻量，直接贴 Vulkan 渲染结果 |
| 输入（Android） | Jetpack Ink（`androidx.ink` 1.0.0 stable） | 低延迟输入/预测，底层 graphics-core |
| 输入（PC） | GLFW 鼠标 / 数位笔 | 走同一输入点流 |
| 笔刷引擎 | **路线 E：白盒移植 libmypaint（自研 C++，CPU dab）** | 实现 `IPaintKernel`，**可插拔** |
| 渲染 | **Vulkan 原生** | 实现 `IRenderBackend`，**可插拔** |
| 笔刷合成 | Vulkan Compute Shader | 参考 Procreate（Metal Compute） |
| SDK 边界 | **C API（`sdk_api/`）** | 唯一对外 ABI，UI 与 CLI 统一消费（v3.0 提前） |
| CLI | **`cli/dgc_cli`（host-only）** | JSON 批处理脚本 → C API → 离屏 PNG |
| 离屏渲染 | **Vulkan headless** | 无 surface，离屏 storage image + readback（PC） |
| PNG 输出 | `stb_image_write`（header-only） | readback 后编码 PNG |
| 确定性 | seed 注入 + 固定时间戳 | 复用 ReplayRandom，同脚本同 seed 像素级一致 |
| C++ 编译 | CMake（多 toolchain） | Android 编 .so；PC 编可执行；host 跑单测 |

> **可扩展性设计原则（详见 §4.0）**：三个插拔点——`IPaintKernel`（绘画内核）、`IRenderBackend`（渲染后端）、`IPlatform`（平台/UI）。换技术路线 = 换某个接口的底层实现，引擎核心与其余层不动。

### 路线全景（5 路线评审结论）

| 路线 | 加权分 | 定位 | 决策 |
|------|--------|------|------|
| **E 白盒移植+Vulkan** | **4.18** | **主线起点** | ✅ **当前开发** |
| B 自研GPU+Vulkan | 4.03 | 终局形态 | ⏳ E 验证手感后演进 |
| C libmypaint+Skia | 3.65 | 快速验证垫脚石 | ⏸ 暂不开发 |
| D 自研+bgfx | 3.55 | iOS 扩展储备 | ⏸ 暂不开发 |
| A 链接libmypaint | 3.33 | 对照基准/兜底 | ⏸ 仅 host 测试用 |

详见 `docs/调研/路线整理.md`（路线分组与中间层复用分析）、`docs/调研/技术路线评审汇总.md`。

### 非目标（本原型明确不做）

文件读写、图层/混合模式/蒙版、项目管理、导出/导入、撤销重做栈（可留接口不实现）、社交社区。

---

## 1. 开发环境搭建

### 1.1 Android Studio（主 IDE，产出 .so + APK）

| 组件 | 版本要求 | 说明 |
|---|---|---|
| Android Studio | Ladybug (2024.3) 及以上 | 2026 年建议最新稳定版 |
| Gradle | 8.12+ | AGP 8.9+ |
| Kotlin | 2.1+ | Compose 编译器走 Kotlin 2.0 内置插件 |
| Android SDK | compileSdk 35+ | minSdk 30（Android 11，保证 Vulkan 1.1 覆盖） |
| Android NDK | r27+（建议 r28） | 自带 CMake、glslc（shaderc）、Vulkan 头 |
| CMake | 3.22+（NDK 内置）；独立装 3.31+ 供 host 用 | 双 toolchain 关键 |
| JDK | 21 | AGP 要求 |
| Compose BOM | 最新稳定 | Material3 |

**必需 SDK 组件**（SDK Manager）：
- `platform-tools`、`platforms;android-35`
- `ndk;28.x`、`cmake;3.31.x`
- `build-tools;35.x`

### 1.2 VS2026（接手 C++ 开发，host 快速验证）

VS2026 走 **host toolchain** 编译 C++ 核心（不含 JNI/swapchain），用于：
- 秒级编译 + IntelliSense 编辑 C++ 逻辑
- 跑核心算法单元测试（预测、笔刷数据结构、math）
- 不产出 Android `.so`（`.so` 仍由 Android Studio/Gradle 产出）

**VS2026 必需组件**（安装时勾选）：
- 「使用 C++ 的桌面开发」工作负载（MSVC + CMake 集成）
- 「C++ CMake tools for Windows」
- Ninja（VS 自带）

**host 侧额外依赖**（VS2026 验证 Vulkan compute 逻辑需要）：
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/)（Windows 版，提供 `vulkan-1.lib`、`glslangValidator`/`glslc`）
- 可选：主机 GPU + 显卡驱动（host 端可跑 headless compute 或 GLFW 窗口测试）

> **说明**：VS2026 用 `CMakePresets.json` 一键切换 host / android 配置。打开项目根目录即自动识别 presets，无需手动配 toolchain。

---

## 2. CMake 工程结构（双 toolchain + 多路线切换）

### 2.1 设计原则

1. **一份 CMakeLists，多 toolchain 分发**：通过 `CMAKE_SYSTEM_NAME STREQUAL "Android"` 区分 Android / host。
2. **平台无关核心与平台层严格分离**：`core/`、`kernels/`、`render/` 是平台无关的 → host 与 android 都编译。
3. **三个插拔接口（§4.0）决定编译边界**：换内核 / 渲染 / 平台 = 换一个子目录的实现，不动接口与引擎核心。
4. **路线切换 = CMake option 切换**：`DGCPAIN_KERNEL_{MYPAINT,BRUSH,GPU}` + `DGCPAIN_RENDER_{VULKAN,SKIA,BGFX}` 组合，实现多路线并存。

### 2.2 目录结构（分层·可插拔·多路线）

```
DGCPaintPrototype/
├── CMakeLists.txt                      # 顶层：路线分发 + 多 toolchain
├── CMakePresets.json                   # host-windows / host-linux / android-arm64
│
├── core/                               # L0-L1：平台无关核心（所有路线共享）
│   ├── CMakeLists.txt
│   ├── interfaces/
│   │   ├── i_paint_kernel.h            # 基础 IPaintKernel（组1/2/3 用）
│   │   ├── i_render_backend.h
│   │   └── i_platform.h
│   ├── types.h                         # StrokePoint / BrushParams / StampData
│   ├── engine.h/.cpp                   # 3 线程模型编排
│   ├── stroke_predictor.h/.cpp         # 速度外推预测
│   ├── ring_buffer.h                   # 无锁 SPSC 队列
│   └── determinism.h/.cpp              # v3.0：seed 注入 + 固定时间戳（确定性）
│
├── sdk_api/                            # ★ v3.0：C API 边界（唯一对外 ABI）
│   ├── CMakeLists.txt
│   ├── dgc_paint_c_api.h               # 唯一 SDK 头文件（C 链接，全平台兼容）
│   └── dgc_paint_c_api.cpp             # opaque handle 实现 → 内部接口
│
├── kernels/                            # L5：绘画内核（插拔）
│   ├── CMakeLists.txt
│   ├── brush/                          # ★ 路线 E：自研 C++ 笔刷内核（主线）
│   │   ├── brush.h/.cpp                # Brush 类（移植 mypaint-brush.c）
│   │   ├── brush_settings.h/.cpp       # 设置枚举 + 响应曲线
│   │   ├── brush_mapping.h/.cpp        # 映射求值
│   │   ├── sensors.h/.cpp              # 传感器滤波
│   │   ├── rng.h/.cpp                  # 随机数（mt19937）
│   │   ├── myb_parser.h/.cpp           # .myb 预设解析
│   │   ├── brush_kernel.h/.cpp         # IPaintKernel 实现
│   │   └── color.h                     # HSV↔RGB
│   ├── mypaint/                        # 路线 A：链接 libmypaint（对照基准）
│   │   ├── mypaint_kernel.h/.cpp
│   │   └── mypaint_surface.h/.cpp
│   └── gpu/                            # 路线 B：GPU compute 内核（终局）
│       └── ...（阶段 3 开发）
│
├── render/                             # L4：渲染后端（插拔）
│   ├── CMakeLists.txt
│   ├── vulkan/                         # ★ 路线 A/E/B：Vulkan Compute（主线）
│   │   ├── vk_backend.h/.cpp
│   │   ├── vk_canvas.h/.cpp
│   │   └── vk_composite.h/.cpp
│   ├── skia/                           # 路线 C：Skia GLES3（备选）
│   │   └── ...
│   └── bgfx/                           # 路线 D：bgfx 跨 API（储备）
│       └── ...
│
├── platform/                           # L3：平台层（所有路线共享）
│   ├── CMakeLists.txt
│   ├── android/
│   │   ├── android_platform.cpp
│   │   └── jni_bridge.cpp
│   └── pc/
│       └── glfw_platform.cpp
│
├── ui/                                 # L6：UI 壳（所有路线共享）
│   ├── android/                        # Compose UI（Kotlin）
│   └── pc/                             # ImGui UI（C++）
│       └── imgui_shell.cpp
│
├── cli/                                # ★ v3.0：CLI 宿主（host-only）
│   ├── CMakeLists.txt
│   ├── main.cpp                        # dgc_cli 入口：run <script.json>
│   └── script_runner.cpp               # JSON 脚本解释器 → C API
│
├── app/                                # Android 工程（Kotlin/Compose）
│   ├── build.gradle.kts
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── cpp/                        # CMake 入口
│       ├── java/com/dgcamp/paint/
│       │   ├── MainActivity.kt
│       │   ├── ui/                     # CanvasView / BrushPanel / ColorPicker
│       │   └── input/InkHandler.kt
│       └── res/
│
├── shaders/                            # 各路线 shader 来源
│   ├── brush_composite.comp            # A/E/B 共享：Vulkan compute GLSL
│   ├── clear_canvas.comp
│   ├── present.vert / present.frag
│   └── bgfx/                           # D 路线：.sc 文件
│
├── third_party/
│   ├── nlohmann/                       # 路线 E：header-only JSON（MIT）
│   ├── mypaint-brushes/                # 笔刷预设
│   ├── stb/                            # v3.0：stb_image_write（PNG 编码）
│   └── libmypaint/                     # 路线 A 对照测试用
│
├── tests/
│   ├── CMakeLists.txt
│   ├── test_brush_parity.cpp           # 路线 E：对照 libmypaint oracle
│   ├── test_rng.cpp                    # 随机数验证
│   ├── test_predictor.cpp
│   ├── test_c_api.cpp                  # v3.0：C API 层单测
│   ├── test_offscreen.cpp              # v3.0：离屏渲染 + PNG 导出
│   ├── test_determinism.cpp            # v3.0：同脚本同 seed 像素级一致
│   └── test_cli.cpp                    # v3.0：脚本解释器全操作映射
│
└── docs/
    └── 调研/                            # 路线技术方案 + 评审
```

### 2.3 顶层 CMakeLists.txt（路线分发核心）

```cmake
cmake_minimum_required(VERSION 3.22)
project(DGCPaintPrototype LANGUAGES C CXX)

# ── 双 toolchain 分发 ──
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(DGCPAIN_ANDROID ON)
else()
    set(DGCPAIN_ANDROID OFF)
endif()

# ── 路线选项 ──
# 绘画内核（L5）
option(DGCPAIN_KERNEL_BRUSH   "Route E: self-developed C++ kernel"  ON)   # 主线
option(DGCPAIN_KERNEL_MYPAINT "Route A: link libmypaint"            OFF)
option(DGCPAIN_KERNEL_GPU     "Route B: GPU compute kernel"         OFF)

# 渲染后端（L4）
option(DGCPAIN_RENDER_VULKAN  "Routes A/E/B: Vulkan compute"        ON)
option(DGCPAIN_RENDER_SKIA    "Route C: Skia GLES3"                 OFF)
option(DGCPAIN_RENDER_BGFX    "Route D: bgfx"                       OFF)

# 通用选项
option(DGCPAIN_BUILD_JNI    "Build JNI bridge (Android only)"  ${DGCPAIN_ANDROID})
option(DGCPAIN_BUILD_TESTS  "Build host unit tests"            ON)
option(DGCPAIN_BUILD_CLI    "Build CLI host (host only)"       ON)
option(DGCPAIN_BUILD_SDK    "Build SDK C API layer"            ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# L0-L1：平台无关核心（所有路线共享）
add_subdirectory(core)

# L5：绘画内核（按选项分发）
if(DGCPAIN_KERNEL_BRUSH)
    add_subdirectory(kernels/brush)
endif()
if(DGCPAIN_KERNEL_MYPAINT)
    add_subdirectory(kernels/mypaint)
endif()
if(DGCPAIN_KERNEL_GPU)
    add_subdirectory(kernels/gpu)
endif()

# L4：渲染后端（按选项分发）
if(DGCPAIN_RENDER_VULKAN)
    add_subdirectory(render/vulkan)
endif()
if(DGCPAIN_RENDER_SKIA)
    add_subdirectory(render/skia)
endif()
if(DGCPAIN_RENDER_BGFX)
    add_subdirectory(render/bgfx)
endif()

# v3.0：SDK C API 层（UI 与 CLI 的唯一对外边界，提前到第一版）
if(DGCPAIN_BUILD_SDK)
    add_subdirectory(sdk_api)
endif()

# L3：平台层
if(DGCPAIN_BUILD_JNI)
    add_subdirectory(platform/android)
endif()
if(NOT DGCPAIN_ANDROID)
    add_subdirectory(platform/pc)
    add_subdirectory(ui/pc)
endif()

# v3.0：CLI 宿主（host-only）
if(DGCPAIN_BUILD_CLI AND NOT DGCPAIN_ANDROID)
    add_subdirectory(cli)
endif()

# 测试
if(DGCPAIN_BUILD_TESTS AND NOT DGCPAIN_ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()
```

### 2.4 CMakePresets.json（VS2026 一键切换）

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 22, "patch": 0 },
  "configurePresets": [
    {
      "name": "host-windows",
      "displayName": "Host Windows/MSVC (Route E)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/host-windows",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "DGCPAIN_KERNEL_BRUSH": "ON",
        "DGCPAIN_RENDER_VULKAN": "ON",
        "DGCPAIN_BUILD_JNI": "OFF",
        "DGCPAIN_BUILD_TESTS": "ON",
        "DGCPAIN_BUILD_SDK": "ON",
        "DGCPAIN_BUILD_CLI": "ON"
      }
    },
    {
      "name": "host-linux",
      "displayName": "Host Linux/GCC",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/host-linux",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "DGCPAIN_KERNEL_BRUSH": "ON",
        "DGCPAIN_RENDER_VULKAN": "ON",
        "DGCPAIN_BUILD_JNI": "OFF",
        "DGCPAIN_BUILD_TESTS": "ON",
        "DGCPAIN_BUILD_SDK": "ON",
        "DGCPAIN_BUILD_CLI": "ON"
      }
    },
    {
      "name": "android-arm64",
      "displayName": "Android arm64-v8a (Route E)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/android-arm64",
      "toolchainFile": "$env{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake",
      "cacheVariables": {
        "ANDROID_ABI": "arm64-v8a",
        "ANDROID_PLATFORM": "android-30",
        "ANDROID_STL": "c++_static",
        "DGCPAIN_KERNEL_BRUSH": "ON",
        "DGCPAIN_RENDER_VULKAN": "ON",
        "DGCPAIN_BUILD_JNI": "ON",
        "DGCPAIN_BUILD_TESTS": "OFF",
        "DGCPAIN_BUILD_SDK": "ON"
      }
    }
  ],
  "buildPresets": [
    { "name": "host-windows",  "configurePreset": "host-windows" },
    { "name": "host-linux",    "configurePreset": "host-linux" },
    { "name": "android-arm64", "configurePreset": "android-arm64" }
  ]
}
```

### 2.5 Vulkan 库查找

```cmake
# render/vulkan/CMakeLists.txt
if(DGCPAIN_ANDROID)
    find_library(VULKAN_LIB vulkan)          # NDK 自带
else()
    find_package(Vulkan REQUIRED)            # LunarG SDK
    set(VULKAN_LIB Vulkan::Vulkan)
endif()
target_link_libraries(dgc_vulkan PUBLIC ${VULKAN_LIB})
```

### 2.6 Shader 编译

```cmake
find_program(GLSLC glslc REQUIRED)
set(SHADER_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SPV_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

add_custom_command(
    OUTPUT ${SPV_OUT_DIR}/brush_composite.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
    COMMAND ${GLSLC} -fshader-stage=compute ${SHADER_DIR}/brush_composite.comp
            -o ${SPV_OUT_DIR}/brush_composite.spv
    DEPENDS ${SHADER_DIR}/brush_composite.comp
    COMMENT "Compiling brush_composite.comp → SPIR-V")
```

---

## 3. 测试环境搭建（物理平板优先）

### 3.1 主测试机（物理平板）

| 项 | 推荐 | 说明 |
|---|---|---|
| 首选 | **Galaxy Tab S9+ / S10+**（Snapdragon 8 Gen 2 / Adreno 740） | S Pen 原生支持压力+tilt，Vulkan 1.3 |
| 备选 | 小米平板 6 Pro / 7 Pro（带触控笔） | 性价比高，Vulkan 1.1+ |
| 屏幕 | ≥120Hz 优先 | 低延迟观感依赖高刷 |

**必备配置**：
- 开发者模式 + USB 调试（或 `adb pair` 无线调试）
- 关闭系统省电/省电模式的 GPU 降频
- 固定横屏，关闭多窗口

### 3.2 辅助环境

| 用途 | 工具 | 说明 |
|---|---|---|
| 帧率/丢帧 | Android GPU Inspector (AGI) | 抓 Vulkan compute 耗时、barrier、带宽 |
| GPU 抓帧 | RenderDoc（Android Vulkan） | 逐 dispatch 分析 compute shader |
| 延迟测量 | 高速摄影（≥240fps 手机拍屏幕 + 触控笔接触帧对比） | 端到端显示延迟最可靠的测量 |
| 帧时间戳 | `dumpsys SurfaceFlinger --latency` | Surface 提交→显示 |
| 模拟器冒烟 | Android Emulator（宿主 GPU 直通 Vulkan） | 仅冒烟/CI，**不测手感** |

### 3.3 性能指标（Procreate 参考基准）

| 指标 | 目标 | 测量方法 |
|---|---|---|
| 端到端延迟（触控→显示） | < 30ms | 高速摄影 / SurfaceFlinger latency |
| 帧率（绘制中） | 稳定 60fps（120Hz 屏测 120fps） | AGI / Choreographer FrameTimeline |
| Compute 合成耗时 | < 2ms | Vulkan timestamp query |
| Stamp 上传耗时 | < 1ms | CPU 打点 |
| 自研内核单 stamp | < 3ms | CPU 打点 |

---

## 4. 技术路线详情

### 4.0 分层架构与插拔接口（可扩展核心）

三条插拔接口定义在 `core/interfaces/`，共享类型在 `core/types.h`：

```cpp
// ── 共享类型 ──
struct StrokePoint { float x, y, pressure, tilt_x, tilt_y; uint64_t t_us; bool is_predicted; };
struct BrushParams { float radius, hardness, opacity; /* 颜色/纹理按需扩 */ };
struct StampData   { float x, y, radius, hardness, opacity; /* alpha 形状位图 */ };

// ── IPaintKernel：绘画内核（笔迹输入 → StampData）──
class IPaintKernel {
public:
    virtual ~IPaintKernel() = default;
    virtual BrushHandle createBrush(const BrushParams&) = 0;
    virtual void beginStroke(BrushHandle, const StrokePoint&) = 0;
    virtual std::vector<StampData> strokeTo(BrushHandle, const StrokePoint&) = 0;
    virtual void endStroke(BrushHandle) = 0;
};

// ── IRenderBackend：渲染后端（StampData → 画布合成 → 上屏）──
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual void init(PlatformSurface, int w, int h) = 0;
    virtual void resize(int w, int h) = 0;
    virtual void beginFrame() = 0;
    virtual void composite(const std::vector<StampData>&) = 0;
    virtual void clearCanvas() = 0;
    virtual void present() = 0;
    virtual void shutdown() = 0;
};

// ── IPlatform：平台抽象（表面 / 输入 / 生命周期）──
class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual PlatformSurface createSurface() = 0;
    virtual std::vector<StrokePoint> pollInput() = 0;
    virtual void runLoop(std::function<void()> frame) = 0;
};
```

**路线切换矩阵**：

| 换什么 | 动哪 | 不动 |
|---|---|---|
| 换内核（E ↔ A） | 新增/切换 `IPaintKernel` 实现 | engine / 渲染后端 / UI |
| 换渲染后端（Vulkan → Skia/bgfx） | 新增/切换 `IRenderBackend` 实现 | engine / 内核 / UI |
| 换平台（Android ↔ PC） | 新增/切换 `IPlatform` 实现 + UI 壳 | engine / 内核 / 渲染后端 |

详见 `docs/调研/路线整理.md` 的完整分组分析。

---

### 4.0.1 v3.0 整体架构：可编程外壳（C API + CLI + 离屏）

v3.0 在 §4.0 之上加一层 **C API 边界**，UI 与 CLI 统一走 C API：

```
┌───────────────────────┐   ┌───────────────────────┐   ┌───────────────────────┐
│  Android UI           │   │  PC ImGui UI          │   │  cli/dgc_cli（新增）    │
│  Compose + Ink        │   │  （GLFW 窗口）         │   │  JSON 脚本 → C API      │
│  ↓ JNI 调 C API       │   │  ↓ 直调 C API         │   │  → 离屏 → PNG           │
├───────────────────────┴───┴───────────────────────┴───┴───────────────────────┤
│                      dgc_paint SDK（C API 边界）                               │
│                                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │  ★ sdk_api/（dgc_paint_c_api.h）——唯一对外 ABI                          │  │
│  │  生命周期 · 画布 · 输入 · 笔刷 · 参数/颜色 · 渲染 · 离屏 · 确定性 · 导出   │  │
│  └──────────────────────────────┬──────────────────────────────────────────┘  │
│                                 │                                             │
│  ┌──────────────────────────────▼──────────────────────────────────────────┐  │
│  │  core/ + kernels/ + render/（纯 C++ 内部，5 路线共享）                    │  │
│  │  engine → Brush::strokeTo → StampData → Vulkan compute → 窗口/离屏       │  │
│  └─────────────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────────────┘
             libdgc_paint.so / .a / .dll / .dylib
```

**为什么 UI 也要改走 C API**：UI 若直调 C++ 虚接口，则「CLI 完成所有界面操作」无法成立——CLI 无法复用 UI 的调用路径。统一走 C API 后，**界面操作 ↔ CLI 命令 ↔ C API 一一对应**（§4.0.4 映射表），任一操作都有等价命令行，任一命令行都有等价界面操作。

### 4.0.2 SDK C API 层（`sdk_api/`）

在 `docs/调研/路线整理.md` §七 的 20 函数基础上，扩展离屏 / 确定性 / 参数化 / 导出：

```c
// dgc_paint_c_api.h —— 唯一 SDK 头文件，C 链接，全平台/编译器兼容

// ── 生命周期 ──
DgcContext* dgcCreate(void);
void        dgcDestroy(DgcContext*);

// ── 画布 Surface（UI 层传入原生窗口句柄）──
void dgcSetSurface(DgcContext*, void* nativeWindow, int w, int h);
void dgcResize(    DgcContext*, int w, int h);

// ── 输入（标准化 StrokePoint）──
void dgcBeginStroke(DgcContext*, float x,float y,float pressure,float tiltX,float tiltY);
void dgcStrokeTo(   DgcContext*, float x,float y,float pressure,float tiltX,float tiltY);
void dgcEndStroke(  DgcContext*);

// ── 笔刷 ──
DgcBrush dgcCreateBrush(      DgcContext*, const DgcBrushParams*);
DgcBrush dgcLoadBrushFromMyb( DgcContext*, const char* mybJson);
void     dgcDestroyBrush(     DgcContext*, DgcBrush);
void     dgcSetBrush(         DgcContext*, DgcBrush);

// ── 渲染 ──
void dgcRender(DgcContext*);                        // 每帧/每次操作后调用
void dgcClear( DgcContext*, float r, float g, float b, float a);

// ── v3.0：离屏（PC CLI，无窗口）──
void dgcSetOffscreenSurface(DgcContext*, int w, int h);
void dgcExportPNG(          DgcContext*, const char* path);   // readback + 编码

// ── v3.0：确定性 ──
void dgcSetRandomSeed(      DgcContext*, uint64_t seed);      // → ReplayRandom
void dgcSetFixedTime(       DgcContext*, double t_us);        // 盖掉 t_us 依赖

// ── v3.0：参数化（对齐 BrushSetting 枚举）──
void dgcSetBrushSetting(    DgcContext*, DgcBrush, int settingId, double value);
void dgcSetBrushColor(      DgcContext*, DgcBrush, float r,float g,float b,float a);

// ── v3.0：撤销 ──
void dgcUndo(               DgcContext*);

// ── v3.0：像素级读回（供对比工具/测试）──
void dgcReadbackPixels(     DgcContext*, void* rgbaOut);
```

**为什么是 C API 不是 C++**：C 的 ABI 在所有平台/编译器间稳定。C++ 的 name mangling / RTTI / 异常在不同编译器间不兼容。C API 可被任何语言调用——C++ 直调，Kotlin 经 JNI，Swift 经桥接头，C# 经 P/Invoke。CLI、Android UI、PC UI 全走这一边界。

### 4.0.3 确定性渲染机制（`core/determinism.h`）

AI 对比需要「同一脚本两次运行像素级一致」：

```cpp
// core/determinism.h —— v3.0
struct DeterminismState {
    uint64_t    seed = 0;          // 注入内核 RNG（ReplayRandom）
    double      fixed_time_us = 0; // 覆盖点流的 t_us 依赖
    bool        override_time = false;
};
```

| 非确定性来源 | 处置 |
|---|---|
| 内核 RNG（dab 抖动） | `dgcSetRandomSeed(seed)` → 复用 T3-3 `ReplayRandom`，同 seed 同序列 |
| 点流 `t_us` 时间戳 | `dgcSetFixedTime(t)` → 固定时间步进，消除速度依赖抖动 |
| 多线程执行顺序 | 离屏单线程路径（CLI 无 3 线程模型，同步调用） |
| GPU 浮点非结合性 | 同后端同驱动可复现；跨 GPU 允许 ±1 LSB 容差（AI 对比时按需设阈值） |

CLI 的离屏路径是**同步单线程**的，天然规避 engine 的 3 线程时序；确定性在 host 上由单测 `test_determinism.cpp` 用 golden PNG 逐字节 diff 验证。

### 4.0.4 界面操作 ↔ CLI 命令 ↔ C API 映射表

| 界面操作 | CLI 脚本命令 | C API |
|---|---|---|
| 建/调画布（尺寸/底色） | `canvas {w,h,background}` | `dgcSetOffscreenSurface` + `dgcClear` |
| 选择笔刷 | `load-brush <myb路径>` / `set-brush <id>` | `dgcLoadBrushFromMyb` / `dgcSetBrush` |
| 调整笔刷参数 | `set-param <settingId> <value>` | `dgcSetBrushSetting` |
| 选择颜色 | `set-color r g b a` | `dgcSetBrushColor` |
| 绘制笔画 | `stroke {points:[...]}` | `dgcBeginStroke` / `dgcStrokeTo` / `dgcEndStroke` |
| 清空画布 | `clear` | `dgcClear` |
| 撤销 | `undo` | `dgcUndo` |
| 保存/导出 | `export <path>` | `dgcExportPNG` |
| 确定性配置 | `seed N` / `fixed-time T` | `dgcSetRandomSeed` / `dgcSetFixedTime` |

> 双向保证：Compose/ImGui 按钮与 `dgc_cli` 脚本调**同一函数**。不存在「CLI 做得到但 UI 做不到」或反向偏差。

### 4.0.5 PC 离屏渲染（Vulkan headless）

`IRenderBackend` 扩展：

```cpp
// render/vulkan/vk_backend.h —— v3.0 新增
class VkBackend : public IRenderBackend {
    // 窗口模式（保留）
    void init(PlatformSurface, int w, int h) override;
    void present() override;
    // v3.0：离屏模式（无窗口）
    void initOffscreen(int w, int h);                 // 无 surface 扩展
    void readback(void* rgbaOut);                     // CopyImageToBuffer + map
    void exportPNG(const char* path);                 // readback + stbi_write_png
};
```

```
dgc_cli run script.json
  → dgcSetOffscreenSurface(w,h)          // VkInstance 无 surface 扩展
  → ops → C API → 内核 strokeTo → StampData
  → vkCmdDispatch(brush_composite.comp)  // 合成到离屏 storage image
  → dgcExportPNG(path)                   // CopyImageToBuffer → map → stbi_write_png
```

离屏资源要点：
- Canvas storage image 常驻 `VK_IMAGE_LAYOUT_GENERAL`，usage 含 `STORAGE | SAMPLED | TRANSFER_DST | TRANSFER_SRC`（读回需要 `TRANSFER_SRC`）。
- 窗口模式的 `present()` 在离屏模式下为 no-op；离屏模式不创建 swapchain。
- `dgcExportPNG` 用 `stb_image_write.h`（header-only，入 `third_party/stb/`）。
- T2-2 的「offscreen 合成验证」直接复用此链路，成为离屏模式的验收基础。

### 4.0.6 CLI 宿主（`cli/dgc_cli`，host-only）

批处理脚本模式（**不含对比模式**，对比交给 AI 侧工具）：

```
dgc_cli run <script.json> [--out <result.png>]
```

JSON 脚本示例：

```json
{
  "canvas": { "w": 2048, "h": 2048, "background": [1, 1, 1, 1] },
  "seed": 42, "fixed-time": 1000000,
  "ops": [
    { "op": "load-brush", "id": "b1", "path": "brushes/basic_round.myb" },
    { "op": "set-brush",  "id": "b1" },
    { "op": "set-color",  "r": 0.2, "g": 0.3, "b": 0.8, "a": 1.0 },
    { "op": "set-param",  "setting": "radius_logarithmic", "value": 0.9 },
    { "op": "stroke", "points": [
      { "x": 100, "y": 100, "p": 0.2, "tiltX": 0.0, "tiltY": 0.0 },
      { "x": 400, "y": 100, "p": 0.8, "tiltX": 0.3, "tiltY": -0.2 }
    ] },
    { "op": "export", "path": "out/result_a.png" }
  ]
}
```

**CLI 定位**：
- 是 SDK C API 的第一个宿主（host-only，不参与 Android 构建）。
- 批处理脚本描述整段会话（建画布 → 选笔刷 → 画笔迹 → 清空 → 撤销 → 导出），一次执行产出 PNG。
- AI 用同一脚本换 seed / 换笔刷 / 换渲染后端，产出多图自行对比；确定性保证同脚本同 seed 像素级一致。
- **对比模式不在 CLI 范围内**，像素对比交给 AI 侧工具（外部 diff）。

---

### 4.1 整体架构（路线 E 主线）

```
┌───────────────────────────────────────────────────────────────┐
│                     Compose UI Layer（Main Thread）            │
│   BrushPanel · ColorPicker · TopBar（叠加层，非画布）           │
└──────────────────────────┬────────────────────────────────────┘
                           │ AndroidView 嵌入
┌──────────────────────────▼────────────────────────────────────┐
│              TextureView + SurfaceTexture → ANativeWindow       │
└──────────────────────────┬────────────────────────────────────┘
                           │ JNI
┌──────────────────────────▼────────────────────────────────────┐
│                   Native 层（3 线程模型）                       │
│  ┌────────────────┐  ┌────────────────┐  ┌─────────────────┐  │
│  │ Input Thread   │  │ Brush Thread   │  │ Render Thread   │  │
│  │ Jetpack Ink    │→│ 自研 C++ 内核   │→│ Vulkan          │  │
│  │ 预测点流        │  │ strokeTo       │  │ compute 合成     │  │
│  │ (ring buffer)  │  │ (ring buffer)  │  │ → present       │  │
│  └────────────────┘  └────────────────┘  └─────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

### 4.2 数据流（一条笔迹的生命周期）

```
触控笔按下 (MotionEvent)
  → InkHandler 接收 → Jetpack Ink 建模 InkStroke
  → 预测点流 push 到 ring_buffer（含预测点，标 isPredicted）
  → Brush Thread 取点 → 自研内核 Brush::strokeTo(x,y,pressure,tilt)
  → 生成 StampData {x,y,radius,opacity,color,hardness}
  → StampData push 到 GPU 上传队列（staging buffer 池）
  → Render Thread: vkCmdCopyBufferToImage（stamp → stamp texture）
  → vkCmdDispatch（brush_composite.comp 合成到 Canvas storage image）
  → vkCmdDraw（present.vert/frag 把 Canvas 画到 swapchain）
  → vkQueuePresent → 屏幕
```

### 4.3 Compose 集成 Vulkan

- **TextureView**（非 SurfaceView）：与 Compose 通过 SurfaceFlinger 合成，集成简单，原型够用。
- `onSurfaceTextureAvailable` 时创建 Render Thread，`onSurfaceTextureDestroyed` 时销毁。
- Vulkan surface 用 `VK_KHR_android_surface`（从 `ANativeWindow_fromSurface` 获取）。

### 4.4 Vulkan 初始化与资源

```
Vulkan 1.1 baseline（minSdk 30 覆盖）
  instance → physicalDevice（选独立 GPU，Adreno）→ device
  queue families：graphics + compute（优先独立 compute queue）
  扩展：VK_KHR_swapchain, VK_KHR_android_surface, VK_KHR_synchronization2

资源：
  Canvas storage image   : VK_IMAGE_USAGE_STORAGE_BIT | SAMPLED_BIT | TRANSFER_DST_BIT
                           布局常驻 VK_IMAGE_LAYOUT_GENERAL
                           v3.0 离屏模式追加 TRANSFER_SRC_BIT（readback 读回）
  Stamp 纹理池           : TRANSFER_DST | SAMPLED，host staging buffer 上传
  Staging buffer 池      : HOST_VISIBLE | HOST_COHERENT，环形复用
  Descriptor sets        : compute 用（canvas + stamp + sampler）
  Command buffers        : 每帧重录 compute + graphics
  同步                   : semaphore（acquire→compute→draw→present）+ fence
  离屏（v3.0，§4.0.5）：无 swapchain，无 present，export 前 fence 等待 + readback
```

### 4.5 Compute Shader 合成（`brush_composite.comp`）

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform image2D u_Canvas;
layout(set = 0, binding = 1) uniform sampler2D  u_Stamp;

layout(push_constant) uniform PC {
    vec2  stampPos;      // stamp 在画布上的像素坐标（左上）
    vec2  stampSize;     // stamp 纹理像素尺寸
    float opacity;
    float alphaLock;     // 预留：阿尔法锁定
} pc;

void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_Canvas);
    if (c.x >= sz.x || c.y >= sz.y) return;

    vec2 uv = (vec2(c) - pc.stampPos) / pc.stampSize;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return;

    vec4 stamp = texture(u_Stamp, uv);
    float a = stamp.a * pc.opacity;
    if (a <= 0.001) return;

    vec4 canvas = imageLoad(u_Canvas, c);
    // premultiplied over：dst = src + dst*(1-src.a)
    vec4 outColor = vec4(stamp.rgb * a + canvas.rgb * (1.0 - a),
                         a + canvas.a * (1.0 - a));
    imageStore(u_Canvas, c, outColor);
}
```

**合成策略**：
- alpha 统一用 premultiplied 存储，合成走 `over` 运算。
- 一次笔迹移动可能产生多个 stamp，累积到 scratch 再批量 dispatch。
- workgroup 8×8，覆盖 stamp 包围盒；只 dispatch 包围盒范围。
- 大 canvas（如 4K）时分 tile dispatch。

### 4.6 Jetpack Ink 集成

| 模块 | 包名 | 是否使用 | 说明 |
|---|---|---|---|
| strokes | `androidx.ink.strokes` | ✅ 核心 | **纯输入数据层**：`StrokeInputBatch` |
| geometry | `androidx.ink.geometry` | ✅ 依赖 | `PartitionedMesh` 等几何 |
| brush | `androidx.ink.brush` | ✅ 仅类型 | `Brush`/`BrushFamily` 类型定义 |
| authoring | `androidx.ink.authoring` | ⚠️ 可选 | `InProgressStrokesView` |
| rendering | `androidx.ink.rendering` | ❌ **不用** | 由我们的 Vulkan 渲染替代 |

### 4.7 线程模型与同步

| 线程 | 职责 | 同步 |
|---|---|---|
| Main Thread | Compose UI、Ink 事件接收、UI 状态 | — |
| Brush Thread | 自研内核 strokeTo、stamp 生成 | SPSC ring buffer（lock-free） |
| Render Thread | Vulkan compute 合成 + present | 每帧 fence + semaphore |

- 点流用无锁 SPSC ring buffer（`core/ring_buffer.h`）。
- 预测点用后会被真实点覆盖：预测 stamp 标 `isPredicted`，真实点到达时以真实 stamp 重合成该段。

---

## 5. 路线分组与中间层复用（核心改动）

> 基于 5 路线评审结论，重新划分路线组。详见 `docs/调研/路线整理.md`。

### 5.1 四组路线划分

| 组 | 路线 | 共享中间层 | 差异点 | 切换成本 |
|----|------|-----------|--------|---------|
| **组1** | **A ↔ E** | L0-L4+L6 全共享（含 render/vulkan/） | 仅 L5 内核：`kernels/mypaint/` ↔ `kernels/brush/` | **极低**：CMake option |
| 组2 | C | L0-L3+L6 共享 | L4: `render/skia/` + L5: `kernels/mypaint/` | 低 |
| 组3 | D | L0-L3+L6 共享 | L4: `render/bgfx/` + L5: `kernels/brush/` | 低 |
| 组4 | B | L0-L3+L6 共享 | L4 适配 + L5: `kernels/gpu/` + **需 IGpuContext 中间层** | 高 |

### 5.2 组1 详细复用（A 与 E 的共享关系）

```
组 A/E 共享：L0-L3 + L4(render/vulkan/) + L6(UI)
  ┌── core/（接口 + engine + 预测 + ring buffer）
  ├── platform/（Android + PC）
  ├── render/vulkan/（vk_backend + vk_canvas + vk_composite + shader）
  └── ui/（Compose + ImGui）

组 A 独有：L5 = kernels/mypaint/（链接 libmypaint C 库）
组 E 独有：L5 = kernels/brush/（自研 C++ 移植算法）
  └── CMake 选项：DGCPAIN_KERNEL_BRUSH=ON（E）vs DGCPAIN_KERNEL_MYPAINT=ON（A）
```

### 5.3 推荐开发路径

```
第一期（主线）：
  共享底座（L0-L3+L6） + 路线 E（kernels/brush/ + render/vulkan/）
  → 验证「libmypaint 算法 + Ink + Vulkan compute」能否达 Procreate 级别手感

第二期（备选）：
  路线 A 的 host 版作对照测试工具（仅用于 E 的移植正确性验证）
  路线 C/D 渲染后端替换（可选，按需切换）

第三期（演进）：
  路线 B 的 IGpuContext 中间层 + GPU 内核
  → 在 E 验证手感后启动，把 CPU dab 逐段搬进 compute shader
```

---

## 6. 分阶段实施计划（基于路线 E 主线）

### 阶段 0 · 技术风险 spike（最高优先，先做）

**目标**：验证两大项——(a) Jetpack Ink 预测点获取方式；(b) 白盒移植核心 `stroke_to` 算法的手感基线。

- 交付：结论文档（预测点来源：自研外推 vs MotionEvent 历史）+ **host oracle 对照测试框架**（libmypaint host 版 dump dab 参数序列，与自研 C++ 内核 diff）
- **验收**：两篇结论各自成立；单 dab / 单段 / 整条 stroke 三级对照测试 diff ≤ 1e-5 全绿（`ctest`）

### 阶段 1 · 接口层 + SDK C API + 多平台骨架（全平台地基）

**目标**：先立三根插拔桩 + **v3.0 SDK C API 边界** + 路线切换 CMake 结构，让内核 / 渲染 / 平台 / CLI 四线可以并行。

- 定义三个插拔接口 + 共享类型
- **v3.0：`sdk_api/` C API 层**（生命周期 / 画布 / 输入 / 笔刷 / 参数 / 渲染 / 离屏 / 确定性 / 导出，§4.0.2）——提前到第一版，UI 与 CLI 统一走 C API
- CMake 多 toolchain 骨架 + 多路线选项（`DGCPAIN_KERNEL_*` + `DGCPAIN_RENDER_*`）+ `DGCPAIN_BUILD_SDK/CLI`
- **验收**：`host-windows` / `host-linux` / `android-arm64` 三 preset 通过；PC 可执行 + Android `.so` 都能编出空壳；C API 层单测（`test_c_api`）通过

### 阶段 2 · 渲染后端（Vulkan，实现 `IRenderBackend`）

- `vk_backend` / `vk_canvas` / `vk_composite` + staging buffer 池
- shader 编译 + `brush_composite.comp` + 批量 dispatch + 包围盒优化
- **验收**：offscreen 用固定 stamp 合成出笔刷痕迹（不依赖内核与平台）

### 阶段 3 · 绘画内核（自研 C++，实现 `IPaintKernel`）

- 完成 `Brush::strokeTo` 移植（传感器滤波 + dab 数量计算 + 核心设置映射 + 颜色调制）
- `.myb` 预设解析器（nlohmann/json）+ 打包 mypaint-brushes 预设
- host oracle 对照测试全量通过
- **验收**：JNI 调 `loadBrush` + `strokeTo` 生成 StampData → 送入 `vk_composite` 可画（offscreen）

### 阶段 4 · 平台层 + UI（双平台）

- **Android**：TextureView + ANativeWindow + swapchain/present + JNI；Compose UI
- **PC**：GLFW 窗口 + Vulkan surface + swapchain/present；ImGui UI
- **验收**：双平台都看到 Vulkan 画布，UI 能切换笔刷/颜色

### 阶段 5 · 输入集成

- Android Jetpack Ink 点流 + PC 鼠标/数位笔输入，统一进 ring buffer
- 预测点覆盖策略（`isPredicted` 标记 + 真实点重合成）
- **验收**：双平台笔迹跟随良好，无明显可感知延迟

### 阶段 6 · 全链路 + 性能测试

- 全链路联调压测（双平台：大 canvas / 连续快速笔触）
- AGI + RenderDoc + 高速摄影测 §3.3 全部指标
- **验收**：满足 §3.3 指标，产出性能报告 + 技术路线结论

### 阶段 7 · 可编程外壳（v3.0：CLI + 离屏 + 确定性 + 图像输出）

**目标**：让 AI 能通过命令行完成界面上的所有操作，并输出图片对比结果。

- **离屏渲染**：`IRenderBackend` 增加 `initOffscreen` / `readback` / `exportPNG`（Vulkan headless，§4.0.5）
- **确定性**：`core/determinism.h` + `dgcSetRandomSeed` / `dgcSetFixedTime`（§4.0.3）
- **CLI 宿主**：`cli/dgc_cli` JSON 批处理脚本解释器，覆盖 §4.0.4 全操作映射（**不含对比模式**）
- PNG 编码：`stb_image_write`（header-only）
- **验收**：
  - `dgc_cli run script.json` 产出 PNG，与窗口渲染结果一致（`test_offscreen`）
  - 同脚本同 seed 两次运行 PNG 像素级一致，逐字节 diff = 0（`test_determinism`）
  - §4.0.4 映射表中每个 UI 操作都有 CLI 命令且输出可对比（`test_cli`）
  - 全操作闭环：`dgc_cli` 可从无到有完成建画布 → 选笔刷 → 调参 → 选色 → 画笔迹 → 撤销 → 导出

### 里程碑

| 里程碑 | 触发 | 交付 |
|---|---|---|
| M1 | 阶段 4 末 | 双平台 Vulkan 画布上屏（PC + Android） |
| M2 | 阶段 3 末 | 笔刷内核经 compute 合成可画（offscreen 验证） |
| M3 | 阶段 6 末 | 全链路 + 性能报告 |
| M4 | 阶段 7 末 | **CLI 全操作闭环，AI 图像对比通道可用**（离屏 PNG + 确定性可复现） |

---

## 7. 关键技术风险与缓解汇总

| 风险 | 影响 | 缓解 |
|---|---|---|
| 移植正确性（浮点/随机偏差）——路线 E 最大风险 | 中 | host oracle 对照 diff dab 参数序列，CI 自动回归 |
| CPU dab 性能不达 P5 <3ms | 中 | LUT 预计算 / 冗余 dab 合并 / SIMD / 帧预算上限 |
| Jetpack Ink 预测点未暴露独立 API | 低 | 自研速度外推或 MotionEvent 历史 |
| TextureView 多一次合成拷贝导致延迟超标 | 中 | 阶段 5 评估 SurfaceView 独立 surface |
| Compute 在低端 Mali GPU 性能不足 | 中 | 包围盒 dispatch + tile 化；baseline 先锁 Adreno |
| 预测点与真实点合成冲突产生拖影 | 中 | 预测 stamp 标记 isPredicted，真实点重合成覆盖 |
| Compose 输入事件有额外延迟 | 中 | Ink 用 `PointerEventPass.Initial`；必要时绕过 Compose 直连 MotionEvent |
| v3.0：GPU 离屏 readback 在不同 GPU 上浮点不一致 | 低 | 确定性限同后端同驱动；跨 GPU 对比按需设 ±1 LSB 容差（§4.0.3） |
| v3.0：headless Vulkan 在无 GPU/驱动环境不可用 | 中 | 离屏链路必须可降级到窗口模式调试；CI 冒烟用 GPU 直通模拟器 |
| v3.0：确定性被非确定性来源（多线程/未注入 RNG）破坏 | 低 | CLI 走同步单线程路径；单测 golden PNG 逐字节 diff 兜底（§4.0.3） |

---

## 8. 路线切换闸门（关键决策点）

| 闸门 | 触发条件 | 动作 |
|------|---------|------|
| A → E | 阶段 0 交叉编译 2 天未通（已默认 E，此闸门已过） | 已默认走 E，A 仅作对照基准 |
| E → A（降级） | 对照测试连续 2 周无法收敛到 1e-5 | 降级为链接 libmypaint（A） |
| E → B（演进） | CPU dab 成为帧率瓶颈，或 4K 大画布压测不过 | 逐段搬进 compute shader |
| E → C（备选） | 团队明确不碰 GPU 细节，或 2D 特效需求压倒性能 | 切换 render/skia/ |
| D 激活 | iOS/Metal 需求坐实 | 激活 render/bgfx/ |

---

## 9. 参考资源

- [Jetpack Ink 官方文档（Android Developers）](https://developer.android.com/develop/ui/compose/touch-input/stylus-input/about-ink-api)
- [mypaint_ffi（libmypaint Android 移植参考实现）](https://pub.dev/packages/mypaint_ffi)
- [MyPaint 社区：How to make it for Android](https://community.mypaint.app/t/how-to-make-it-for-android/3965)
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/)
- `docs/tasks/任务线.md` —— SDK 任务状态（脚本申领）
- `docs/git/README.md` —— 消费者以 submodule 引用本 SDK
- `docs/调研/路线整理.md` —— 路线分组、中间层复用、§7 C API / SDK 化
- `docs/调研/技术路线评审汇总.md` —— 5 路线评审结论
- `docs/调研/路线E-白盒移植libmypaint-技术方案.md` —— 路线 E 详细技术方案
- [stb_image_write（PNG 编码，header-only，MIT）](https://github.com/nothings/stb) —— v3.0 离屏导出
- `docs/调研/笔刷渲染技术路线评审.md` —— 评审框架与评分依据
- `docs/调研/绘画内核功能清单.md` —— 功能优先级