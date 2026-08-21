# DGCamp Paint 原型 · 技术规划

## Context（背景与目标）

DGCamp Paint 工具的完整需求中，绘画功能是核心。本阶段剔除社交/社区、项目管理、图层、文件读写，只做**绘画原型机**，用于验证一个关键技术假设：

> **libmypaint（笔刷引擎）+ Jetpack Ink（低延迟输入）+ Vulkan Compute Shader（GPU 笔刷合成）三者组合，能否达到 Procreate 级别的绘画手感。**

目标平台为 **Android 平板**。这是一个"技术验证型原型"，不是产品 MVP——核心交付物是「能画 + 能测延迟/帧率」的最小闭环，用于评估技术路线的可行性，而非完成产品功能。

### 技术选型（已与用户确认）

| 层 | 选型 | 备注 |
|---|---|---|
| 平台 | **Android 平板 + PC（双平台）** | 平台无关核心 + 平台层插拔 |
| UI（Android Pad） | Jetpack Compose | 平板优先，横向布局 |
| UI（PC） | **ImGui**（GLFW 窗口） | 轻量，直接贴 Vulkan 渲染结果 |
| 输入（Android） | Jetpack Ink（`androidx.ink` 1.0.0 stable） | 低延迟输入/预测，底层 graphics-core |
| 输入（PC） | GLFW 鼠标 / 数位笔 | 走同一输入点流 |
| 笔刷引擎 | libmypaint（C，CPU 端） | 实现 `IPaintKernel`，**可插拔** |
| 渲染 | **Vulkan 原生** | 实现 `IRenderBackend`，**可插拔** |
| 笔刷合成 | Vulkan Compute Shader | 参考 Procreate（Metal Compute） |
| C++ 编译 | CMake（多 toolchain） | Android 编 .so；PC 编可执行；host 跑单测 |

> **可扩展性设计原则（详见 §4.0）**：三个插拔点——`IPaintKernel`（绘画内核）、`IRenderBackend`（渲染后端）、`IPlatform`（平台/UI）。换技术路线 = 换某个接口的底层实现，引擎核心与其余层不动。

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

## 2. CMake 工程结构（双 toolchain，重点）

### 2.1 设计原则

1. **一份 CMakeLists，多 toolchain 分发**：通过 `CMAKE_SYSTEM_NAME STREQUAL "Android"` 区分 Android / host（host 再分 Windows / Linux）。
2. **平台无关核心与平台层严格分离**：
   - `core/`、`kernels/`、`render/`（compute 部分）是平台无关的 → host 与 android 都编译。
   - `platform/android/`（JNI、swapchain、surface）只在 Android 编译；`platform/pc/`、`ui/pc/`（GLFW、ImGui）只在 host 编译。
3. **三个插拔接口（§4.0）决定编译边界**：换内核 / 渲染 / 平台 = 换一个子目录的实现，不动接口与引擎核心。
4. **VS2026 打开根目录即得 host 配置**，Android 产物由 Gradle 调同一份顶层 CMake 产出；PC 可执行由 host preset 直接产出。

### 2.2 目录结构（分层·可插拔）

```
DGCPaintPrototype/
├── CMakeLists.txt                      # 顶层：多 toolchain 分发（见 2.3）
├── CMakePresets.json                   # host-windows / host-linux / android-arm64（见 2.4）
├── core/                               # ★ 平台无关核心 + 接口定义（双平台共享）
│   ├── CMakeLists.txt
│   ├── interfaces/                     # 三个插拔接口（见 §4.0）
│   │   ├── i_paint_kernel.h            # 绘画内核接口
│   │   ├── i_render_backend.h          # 渲染后端接口
│   │   └── i_platform.h                # 平台抽象接口（surface/input/lifecycle）
│   ├── types.h                         # StrokePoint / BrushParams / StampData
│   ├── engine.h/.cpp                   # 引擎核心（3 线程模型，编排三接口）
│   ├── stroke_predictor.h/.cpp         # 预测算法（host 可单测）
│   └── ring_buffer.h                   # 线程间无锁队列
├── kernels/                            # ★ 绘画内核（插拔·实现 IPaintKernel）
│   ├── CMakeLists.txt
│   └── mypaint/
│       ├── mypaint_kernel.h/.cpp       # IPaintKernel 实现
│       └── mypaint_surface.h/.cpp      # MyPaintSurface → StampData
├── render/                             # ★ 渲染后端（插拔·实现 IRenderBackend）
│   ├── CMakeLists.txt
│   └── vulkan/
│       ├── vk_backend.h/.cpp           # IRenderBackend 实现
│       ├── vk_canvas.h/.cpp            # Canvas storage image 管理
│       └── vk_composite.h/.cpp         # compute 合成管线
├── platform/                           # ★ 平台层（插拔·实现 IPlatform）
│   ├── CMakeLists.txt
│   ├── android/                        # Android：ANativeWindow + MotionEvent + swapchain/present
│   │   ├── android_platform.cpp
│   │   └── jni_bridge.cpp              # JNI 入口
│   └── pc/                             # PC：GLFW 窗口 + 鼠标/数位笔
│       └── glfw_platform.cpp
├── ui/                                 # ★ UI 壳（插拔·编译期选）
│   ├── android/                        # Compose UI（Kotlin，见 app/）
│   └── pc/                             # ImGui UI（C++）
│       └── imgui_shell.cpp
├── app/                                # Android 工程（Kotlin/Compose）
│   ├── build.gradle.kts                # externalNativeBuild 指向 ../CMakeLists.txt
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── cpp/                        # → 顶层 core/kernels/render/platform 的 CMake 入口
│       ├── java/com/dgcamp/paint/
│       │   ├── MainActivity.kt
│       │   ├── ui/                     # Compose：CanvasView / BrushPanel / ColorPicker
│       │   └── input/InkHandler.kt     # Jetpack Ink 集成
│       └── res/
├── shaders/                            # GLSL → SPIR-V
│   ├── brush_composite.comp
│   ├── clear_canvas.comp
│   └── present.vert / present.frag
├── third_party/
│   ├── libmypaint/                     # arm64-v8a / x86_64 静态库 + include
│   └── json-c/  mypaint-brushes/
├── tools/
│   ├── build_mypaint_android.sh        # 交叉编译 libmypaint（见 2.7）
│   └── build_mypaint_host.sh
├── tests/                              # CTest host 单测（接口 + 预测 + math）
│   ├── CMakeLists.txt
│   └── test_predictor.cpp
└── docs/
    └── 技术规划.md                     # ★ 本计划留痕
```

### 2.3 顶层 CMakeLists.txt（核心分发逻辑）

```cmake
cmake_minimum_required(VERSION 3.22)
project(DGCPaintPrototype LANGUAGES C CXX)

# ── 双 toolchain 分发 ─────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(DGCPAIN_ANDROID ON)
else()
    set(DGCPAIN_ANDROID OFF)
endif()

# host 编译平台无关核心；android 编译全部
option(DGCPAIN_BUILD_JNI    "Build JNI bridge (Android only)"  ${DGCPAIN_ANDROID})
option(DGCPAIN_BUILD_TESTS  "Build host unit tests"            ON)
option(DGCPAIN_USE_VULKAN   "Enable Vulkan renderer"           ON)
option(DGCPAIN_BUILD_PC_APP "Build PC executable (GLFW+ImGui)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 平台无关：接口 + 引擎核心（host/android 都产）
add_subdirectory(core)

# 插拔实现：绘画内核 / 渲染后端（平台无关，host/android 都产）
add_subdirectory(kernels/mypaint)
if(DGCPAIN_USE_VULKAN)
    add_subdirectory(render/vulkan)
endif()

# Android 专属：平台层 + JNI → libdgc_paint.so
if(DGCPAIN_BUILD_JNI)
    add_subdirectory(platform/android)
endif()

# PC 专属：平台层 + ImGui UI → 可执行文件
if(DGCPAIN_BUILD_PC_APP AND NOT DGCPAIN_ANDROID)
    add_subdirectory(platform/pc)
    add_subdirectory(ui/pc)
endif()

# host 专属：单元测试
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
      "displayName": "Host Windows/MSVC (VS2026 快速验证)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/host-windows",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "DGCPAIN_BUILD_JNI": "OFF",
        "DGCPAIN_BUILD_TESTS": "ON",
        "DGCPAIN_USE_VULKAN": "ON"
      }
    },
    {
      "name": "host-linux",
      "displayName": "Host Linux/GCC (CI 或 Linux 开发机)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/host-linux",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "DGCPAIN_BUILD_JNI": "OFF",
        "DGCPAIN_BUILD_TESTS": "ON",
        "DGCPAIN_USE_VULKAN": "ON"
      }
    },
    {
      "name": "android-arm64",
      "displayName": "Android arm64-v8a (目标产物)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/android-arm64",
      "toolchainFile": "$env{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake",
      "cacheVariables": {
        "ANDROID_ABI": "arm64-v8a",
        "ANDROID_PLATFORM": "android-30",
        "ANDROID_STL": "c++_static",
        "DGCPAIN_BUILD_JNI": "ON",
        "DGCPAIN_BUILD_TESTS": "OFF",
        "DGCPAIN_USE_VULKAN": "ON"
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

### 2.5 Vulkan 库查找（双 toolchain 关键片段）

```cmake
# vulkan/CMakeLists.txt
if(DGCPAIN_ANDROID)
    find_library(VULKAN_LIB vulkan)          # NDK 自带 libvulkan.so
else()
    find_package(Vulkan REQUIRED)            # LunarG SDK / 系统
    set(VULKAN_LIB Vulkan::Vulkan)
endif()
target_link_libraries(dgc_vulkan PUBLIC ${VULKAN_LIB})
```

### 2.6 Shader 编译（GLSL → SPIR-V，构建期）

```cmake
# 找 glslc（shaderc）；Android 用 NDK 自带，host 用 Vulkan SDK 自带
find_program(GLSLC glslc REQUIRED)

set(SHADER_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
set(SPV_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

add_custom_command(
    OUTPUT ${SPV_OUT_DIR}/brush_composite.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
    COMMAND ${GLSLC} -fshader-stage=compute ${SHADER_DIR}/brush_composite.comp
            -o ${SPV_OUT_DIR}/brush_composite.spv
    DEPENDS ${SHADER_DIR}/brush_composite.comp
    COMMENT "Compiling brush_composite.comp → SPIR-V")
# present.vert / present.frag / clear_canvas.comp 同理
# 产物作为 jni 资源的依赖，运行时从 APK assets 加载
```

### 2.7 libmypaint 依赖编译（关键坑点，来自 mypaint_ffi 已验证路径）

libmypaint 依赖 **json-c**、**glib**（可裁剪）、**mypaint-brushes**（笔刷预设）。Android 交叉编译的三个必踩坑点（已由 `mypaint_ffi` 验证）：

1. **`config.h` / `mypaint-config.h` 必须生成或 vendor**：直接删 `#include "config.h"` 会编译失败。做法是把生成的 `mypaint-config.h` 预置进 `third_party/libmypaint/include/`。
2. **glib 头文件必须保留**（即使 `MYPAINT_CONFIG_USE_GLIB=0`）：`mypaint-brush.c` 无条件 include glib 头，文件必须在源码树中。
3. **NDK 版本**：Krita 的 MyPaint 引擎 Android 移植用 NDK r18b 更稳；新 NDK（r27+）亦可，但需处理 glib 的 `_FILE_OFFSET_BITS` 等宏。

`tools/build_mypaint_android.sh` 流程：`git clone libmypaint + json-c` → 用 NDK toolchain 分别 configure/make 出静态库 → 产物拷到 `third_party/libmypaint/arm64-v8a/`。mypaint-brushes 笔刷预设目录作为 assets 打进 APK。

---

## 3. 测试环境搭建（物理平板优先）

### 3.1 主测试机（物理平板，用于手感 + 性能基准）

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
| libmypaint 单 stamp | < 3ms | CPU 打点 |

---

## 4. 技术路线详情

### 4.0 分层架构与插拔接口（可扩展核心）

高度可扩展的目标靠**三个插拔点**实现：换技术路线 = 换某个接口的底层实现，引擎核心与其余层不动。分层如下：

```
┌───────────────────────────────────────────────────────────────┐
│  UI 层（插拔·编译期选）                                        │
│   Android Pad UI（Compose/Kotlin）    PC UI（ImGui/C++）       │
└──────────────┬───────────────────────────────────────────────┘
               │ 绑定稳定引擎 API（IEngine）
┌──────────────▼───────────────────────────────────────────────┐
│  engine（平台无关核心，双平台共享）                             │
│   3 线程模型（Input → Brush → Render）+ ring buffer + 预测      │
└──────┬───────────────────────────────┬───────────────────────┘
       │ IPaintKernel 接口             │ IRenderBackend 接口
┌──────▼──────────────┐   ┌────────────▼───────────────────────┐
│ 绘画内核（插拔）      │   │ 渲染后端（插拔）                     │
│ libmypaint / 自研 GPU│   │ Vulkan compute / bgfx / Metal       │
└─────────────────────┘   └────────────────────────────────────┘
                ▲                          ▲
                └──── IPlatform 接口（surface/input/lifecycle）──┐
                                                          ┌─────▼─────┐
                                                          │ 平台层（插拔）│
                                                          │ Android/PC │
                                                          └───────────┘
```

三个接口定义在 `core/interfaces/`，共享类型在 `core/types.h`：

```cpp
// ── 共享类型 ────────────────────────────────
struct StrokePoint { float x, y, pressure, tilt_x, tilt_y; uint64_t t_us; bool is_predicted; };
struct BrushParams { float radius, hardness, opacity; /* 颜色/纹理按需扩 */ };
struct StampData   { float x, y, radius, hardness, opacity; /* alpha 形状位图 */ };

// ── IPaintKernel：绘画内核（笔迹输入 → StampData）──
// 插拔点①：libmypaint / 自研 GPU 内核 各自实现
class IPaintKernel {
public:
    virtual ~IPaintKernel() = default;
    virtual BrushHandle createBrush(const BrushParams&) = 0;
    virtual void beginStroke(BrushHandle, const StrokePoint&) = 0;
    virtual std::vector<StampData> strokeTo(BrushHandle, const StrokePoint&) = 0;
    virtual void endStroke(BrushHandle) = 0;
};

// ── IRenderBackend：渲染后端（StampData → 画布合成 → 上屏）──
// 插拔点②：Vulkan compute / bgfx / Metal 各自实现
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
// 插拔点③：Android(TextureView+ANativeWindow+MotionEvent) / PC(GLFW+鼠标+数位笔)
class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual PlatformSurface createSurface() = 0;
    virtual std::vector<StrokePoint> pollInput() = 0;
    virtual void runLoop(std::function<void()> frame) = 0;
};
```

**换路线的含义**：

| 换什么 | 动哪 | 不动 |
|---|---|---|
| 换绘画内核（libmypaint → 自研 GPU 内核） | 新增一个 `IPaintKernel` 实现 | engine / 渲染后端 / UI |
| 换渲染后端（Vulkan → bgfx/Metal） | 新增一个 `IRenderBackend` 实现 | engine / 内核 / UI |
| 换平台/UI（Android ↔ PC） | 新增一个 `IPlatform` 实现 + 一个 UI 壳 | engine / 内核 / 渲染后端 |

UI 本身也是编译期插拔：`ui/android/`（Compose）与 `ui/pc/`（ImGui）各自实现同一个「引擎控制 API」（设笔刷、设颜色、undo 等），互不影响。PC UI 用 ImGui 绘制控制面板，画布区域贴 Vulkan 渲染结果；Android UI 用 Compose 画 BrushPanel/ColorPicker，画布是 TextureView。

### 4.1 整体架构

```
┌───────────────────────────────────────────────────────────────┐
│                     Compose UI Layer（Main Thread）            │
│   BrushPanel · ColorPicker · TopBar（叠加层，非画布）           │
└──────────────────────────┬────────────────────────────────────┘
                           │ AndroidView 嵌入
┌──────────────────────────▼────────────────────────────────────┐
│              TextureView + SurfaceTexture → ANativeWindow       │
│              （Vulkan 渲染目标，全屏画布背景层）                 │
└──────────────────────────┬────────────────────────────────────┘
                           │ JNI
┌──────────────────────────▼────────────────────────────────────┐
│                   Native 层（3 线程模型）                       │
│  ┌────────────────┐  ┌────────────────┐  ┌─────────────────┐  │
│  │ Input Thread   │  │ Brush Thread   │  │ Render Thread   │  │
│  │ Jetpack Ink    │→│ libmypaint     │→│ Vulkan          │  │
│  │ 预测点流        │  │ stamp 生成     │  │ compute 合成     │  │
│  │ (ring buffer)  │  │ (ring buffer)  │  │ → present       │  │
│  └────────────────┘  └────────────────┘  └─────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

### 4.2 数据流（一条笔迹的生命周期）

```
触控笔按下 (MotionEvent)
  → InkHandler 接收 → Jetpack Ink 建模 InkStroke（含压力/tilt/时间戳）
  → 预测点流 push 到 ring_buffer（含预测点，标 isPredicted）
  → Brush Thread 取点 → libmypaint stroke_to(x, y, pressure, tilt)
  → MyPaintSurface 回调 → 生成 StampData {x,y,radius,opacity,color, hardness}
  → StampData push 到 GPU 上传队列（staging buffer 池）
  → Render Thread: vkCmdCopyBufferToImage（stamp → stamp texture）
  → vkCmdDispatch（brush_composite.comp 合成到 Canvas storage image）
  → vkCmdDraw（present.vert/frag 把 Canvas 画到 swapchain）
  → vkQueuePresent → 屏幕
```

### 4.3 Compose 集成 Vulkan 的关键点

- **TextureView**（非 SurfaceView）：与 Compose 通过 SurfaceFlinger 合成，集成简单，原型够用。TextureView 会多一次 GPU 合成拷贝，若延迟不达标，阶段 5 再评估 SurfaceView 独立 surface 方案。
- `onSurfaceTextureAvailable` 时创建 RenderThread，`onSurfaceTextureDestroyed` 时销毁。
- Vulkan surface 用 `VK_KHR_android_surface`（从 `ANativeWindow_fromSurface` 获取）。
- Canvas 尺寸变化（旋转/多窗口）时重建 swapchain，Canvas 纹理内容保留。

### 4.4 Vulkan 初始化与资源

```
Vulkan 1.1 baseline（minSdk 30 覆盖）
  instance → physicalDevice（选独立 GPU，Adreno）→ device
  queue families：graphics + compute（可同一 queue，优先独立 compute queue）
  扩展：VK_KHR_swapchain, VK_KHR_android_surface, VK_KHR_synchronization2（可选）

资源：
  Canvas storage image   : VK_IMAGE_USAGE_STORAGE_BIT | SAMPLED_BIT | TRANSFER_DST_BIT
                           布局常驻 VK_IMAGE_LAYOUT_GENERAL（避免频繁 transition）
  Stamp texture（池）    : TRANSFER_DST | SAMPLED，host staging buffer 上传
  Staging buffer 池      : HOST_VISIBLE | HOST_COHERENT，环形复用
  Descriptor sets        : compute 用（canvas + stamp + sampler）
  Command buffers        : 每帧重录 compute + graphics
  同步                   : semaphore（acquire→compute→draw→present）+ fence
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
    // 预乘 alpha (premultiplied) over 混合 —— 参考画世界PRO blit 模型（见附录 A）
    // dst = src + dst*(1-src.a)；canvas 与 stamp 均存预乘色
    vec4 outColor = vec4(stamp.rgb * a + canvas.rgb * (1.0 - a),
                         a + canvas.a * (1.0 - a));
    imageStore(u_Canvas, c, outColor);
}
```

**合成策略（关键设计，参考画世界PRO）**：
- **alpha 统一用 premultiplied（预乘）存储**，合成走 `over` 运算 `dst = src + dst*(1-src.a)`。这是画世界PRO `blitType` 三种模式（Direct/Remul/dePremul）背后的核心，也是低延迟 GPU 合成的关键。
- 一次笔迹移动可能产生多个 stamp，**累积到 scratch 再批量 dispatch**，避免每个 stamp 一次 dispatch 的 overhead。
- workgroup 8×8，覆盖 stamp 包围盒；只 dispatch 包围盒范围（`ceil(box/8)`），不全画布。
- 大 canvas（如 4K）时分 tile dispatch，控制单次 dispatch 尺寸。
- 预留三个合成扩展位（对应画世界PRO 图层能力）：`layerMask`（图层蒙版）、`lockAlpha`（阿尔法锁定，只在已画像素区域写）、`selMask`（选区裁剪）——原型可先只实现 lockAlpha，其余留接口。

### 4.6 Jetpack Ink 集成（已模块化，按需取用）

官方确认 Jetpack Ink **已模块化**，可只取所需模块。与本方案相关的模块：

| 模块 | 包名 | 是否使用 | 说明 |
|---|---|---|---|
| strokes | `androidx.ink.strokes` | ✅ 核心 | **纯输入数据层**：`StrokeInputBatch`（位置/时间戳/压力/倾斜/方向）、`InProgressStroke`、`Stroke` |
| geometry | `androidx.ink.geometry` | ✅ 依赖 | `PartitionedMesh` 等几何，`Stroke` 依赖 |
| brush | `androidx.ink.brush` | ✅ 仅类型 | `Brush`/`BrushFamily` 类型定义，`Stroke` 依赖 |
| authoring | `androidx.ink.authoring` | ⚠️ 可选 | `InProgressStrokesView`（捕获 MotionEvent + 低延迟渲染） |
| rendering | `androidx.ink.rendering` | ❌ **不用** | `CanvasStrokeRenderer`/`ViewStrokeRenderer` —— 绑定 Canvas 的渲染，由我们的 Vulkan 渲染替代 |

**核心结论**：`strokes` 模块是**纯数据层**，`StrokeInputBatch` 直接提供含时间戳/压力/倾斜/方向的输入点流，**完全脱离渲染**。因此「自定义 Vulkan 渲染 + libmypaint 笔刷」的集成是干净的，不存在"必须用 Ink 渲染管线"的绑定问题——原先标注的「预测层不可剥离」高风险已消除。

**依赖**（stable 1.0.0，按需取用）：
```kotlin
implementation("androidx.ink:ink-strokes:1.0.0")   // 输入点流（核心）
implementation("androidx.ink:ink-geometry:1.0.0")   // 几何（Stroke 依赖）
implementation("androidx.ink:ink-brush:1.0.0")      // 笔刷类型（Stroke 依赖）
// 不引入 ink-rendering（CanvasStrokeRenderer 用不到）
// ink-authoring 视输入捕获方式决定是否引入
```

**集成目标形态**：
```
MotionEvent → authoring 捕获（或直接构建 StrokeInputBatch）
           → StrokeInputBatch 点流（含时间戳/压力/倾斜/方向）
           → push ring_buffer
           → libmypaint 消费 → stamp → Vulkan compute
```

**阶段 0 唯一待验证项（低风险）**：低延迟"预测"机制位于 graphics-core 前缓冲渲染内部，文档未暴露独立预测 API。若需预测点喂 libmypaint，两条现成路径：(a) 自研速度外推预测（~30 行）；(b) 直接读 `MotionEvent` 历史事件。二者都不依赖 Ink 渲染管线。

### 4.7 线程模型与同步

| 线程 | 职责 | 同步 |
|---|---|---|
| Main Thread | Compose UI、Ink 事件接收、UI 状态 | — |
| Brush Thread | libmypaint stroke_to、stamp 生成 | SPSC ring buffer（lock-free） |
| Render Thread | Vulkan compute 合成 + present | 每帧 fence + semaphore |

- 点流用**无锁 SPSC ring buffer**（`core/ring_buffer.h`）从 Main/Ink 传给 Brush Thread。
- stamp 用另一个 ring buffer 从 Brush 传给 Render Thread。
- 预测点用后会被真实点**覆盖/修正**：预测 stamp 标 `isPredicted`，真实点到达时以真实 stamp 重合成该段（或直接忽略预测 stamp，预测只用于降低观感，不作为最终像素保留——这是 Procreate 同款策略）。

---

## 5. 分阶段实施计划

### 阶段 0 · 技术风险 spike（最高优先，先做）

**目标**：验证两大项——(a) Jetpack Ink 预测点获取方式；(b) libmypaint 交叉编译链。不依赖接口层，可与阶段 1 并行。

- 交付：结论文档（预测点来源：自研外推 vs MotionEvent 历史）+ libmypaint arm64 编译通过并跑通 `stroke_to`。
- **验收**：两篇结论各自成立，输出到 `docs/`。

### 阶段 1 · 接口层 + 多平台骨架（全平台地基）

**目标**：先立三根插拔桩，让内核 / 渲染 / 平台三线可以并行、互不阻塞。

- 定义三个插拔接口（`IPaintKernel` / `IRenderBackend` / `IPlatform`）+ 共享类型（`StrokePoint`/`BrushParams`/`StampData`）。
- CMake 多 toolchain 骨架 + 分层目录（`core/` `kernels/` `render/` `platform/` `ui/`）。
- **验收**：`host-windows` / `host-linux` / `android-arm64` 三 preset 配置通过；PC 可执行 + Android `.so` 都能编出空壳。

### 阶段 2 · 渲染后端（Vulkan，实现 `IRenderBackend`）

- `vk_backend` / `vk_canvas` / `vk_composite` + staging buffer 池（平台无关，host 可跑）。
- shader 编译 + `brush_composite.comp` + 批量 dispatch + 包围盒优化。
- **验收**：offscreen 用固定 stamp 合成出笔刷痕迹（不依赖内核与平台）。

### 阶段 3 · 绘画内核（libmypaint，实现 `IPaintKernel`）

- 交叉编译 libmypaint + json-c，打包 mypaint-brushes 预设。
- `mypaint_surface` → `StampData`，`mypaint_kernel` 实现 `IPaintKernel`，JNI 暴露 `createBrush/beginStroke/strokeTo/endStroke`。
- **验收**：JNI 调 libmypaint 生成 stamp，送入 `vk_composite` 可画（offscreen）。

### 阶段 4 · 平台层 + UI（双平台，UI 插拔）

- **Android**：TextureView + ANativeWindow + swapchain/present + JNI；Compose UI（CanvasView/BrushPanel/ColorPicker）。
- **PC**：GLFW 窗口 + Vulkan surface + swapchain/present；ImGui UI（笔刷面板/取色器，画布贴 Vulkan 结果）。
- **验收**：双平台都看到 Vulkan 画布，UI 能切换笔刷/颜色。

### 阶段 5 · 输入集成

- Android Jetpack Ink 点流（按阶段 0 结论）+ PC 鼠标/数位笔输入，统一进 `ring_buffer`。
- 预测点覆盖策略（`isPredicted` 标记 + 真实点重合成）。
- **验收**：双平台笔迹跟随良好，无明显可感知延迟。

### 阶段 6 · 全链路 + 性能测试

- 全链路联调压测（双平台：大 canvas / 连续快速笔触）。
- AGI + RenderDoc + 高速摄影测 §3.3 全部指标。
- **验收**：满足 §3.3 指标，产出性能报告 + 技术路线结论（是否达 Procreate 级别）。

### 里程碑

| 里程碑 | 触发 | 交付 |
|---|---|---|
| M1 | 阶段 4 末 | 双平台 Vulkan 画布上屏（PC + Android） |
| M2 | 阶段 3 末 | 笔刷内核经 compute 合成可画（offscreen 验证） |
| M3 | 阶段 6 末 | 全链路 + 性能报告 |

> M2（offscreen）可先于 M1（上屏）达成——内核与渲染和平台层解耦，offscreen 验证不需要 surface。

## 6. 关键技术风险与缓解汇总

| 风险 | 影响 | 缓解 |
|---|---|---|
| Jetpack Ink 预测点未暴露独立 API | 低 | Ink 已模块化，strokes 纯数据层直接取输入点；预测用自研速度外推或 MotionEvent 历史 |
| libmypaint 的 glib 依赖交叉编译难 | 高 | 走 mypaint_ffi 已验证路径；vendor config.h；裁剪 glib |
| TextureView 多一次合成拷贝导致延迟超标 | 中 | 阶段 5 评估 SurfaceView 独立 surface |
| Compute 在低端 Mali GPU 性能不足 | 中 | 包围盒 dispatch + tile 化；baseline 先锁 Adreno |
| 预测点与真实点合成冲突产生拖影 | 中 | 预测 stamp 标记 isPredicted，真实点重合成覆盖 |
| Compose 输入事件有额外延迟 | 中 | Ink 用 `PointerEventPass.Initial`；必要时绕过 Compose 直连 MotionEvent |

---

## 7. 留痕说明

本文档即留痕产物，输出于项目根目录 `DGCPaint_技术规划.md`，作为开发机器上接手 AI 的上下文文档。原始 RenderDoc 抓帧分析依据为 `RenderDoc结果/qqq.rdc`。

## 8. 附录 A：画世界PRO 渲染管线分析（RDC 抓帧）

> 依据 `RenderDoc结果/qqq.rdc`（RenderDoc v1.46，47.6MB）解析得出。方法：解析 RDC 二进制头 + 提取内嵌 GLSL shader 源码（该文件未含 SPIR-V，shader 以 GLSL 文本存储，说明是 OpenGL 而非 Vulkan capture）。

### 8.1 capture 基本信息

| 项 | 值 |
|---|---|
| 渲染上下文 | **OpenGL 4.6 Compatibility Profile**（Intel 驱动 25.20.50.04，Windows 环境） |
| UI 层 | **Qt Quick / QML**（shader 中大量 `qt_Matrix`/`qt_Texture`/`qt_VertexPosition`） |
| 笔刷合成方式 | **fragment shader 的 stamp/blit 模型**，**无 compute shader**（`layout(local_size)`、`gl_GlobalInvocationID` 均 0 次） |

**重要**：该抓帧来自 Windows PC 版（Qt + OpenGL 4.6），非 Android 版。参考价值在于**合成算法与数据组织**（可迁移到 Vulkan），而非 API 栈本身。

### 8.2 shader 清单（按职责）

| offset | #version | 职责 |
|---|---|---|
| 0x542f6 | 140 | 通用顶点 shader：`mvpMatrix * inPosition` |
| 0x546ef | 140 | 纯色填充 fragment shader |
| — | 140 | 网格/参考线 shader（`unitSize`/`lineW`） |
| 0x5d80f | **460** | **笔刷印章 vertex shader**：压力/圆度/半线宽/HSV 随机色/纸纹偏移/mipmap |
| （夹在中间） | 140 | **笔刷形状 + 合成 fragment shader**：shapeTex + 距离场 + hardness + 纸纹 + blit 混合 |
| 0xe9b5b / 0xe9ccc / 0xea46c | 150 | **Qt UI shaders**：纹理采样 / 描边扩展（vertexOffset） |

### 8.3 笔刷渲染管线（核心算法）

**① 笔刷印章 = quad + 顶点属性驱动的形状生成**

vertex shader（#version 460）每顶点输入：`pressu`（压力）、`inRoundness`（圆度）、`halfLineWidth`（半线宽）、`inColorHSVRand`（HSV 颜色+随机抖动）、`inOpacity`。顶点阶段做：
- `Tmb = pow(pressu.x, 1.5)` — 压力到笔刷宽度的映射
- `random()/random2()` — 抖动/随机化（色相随机、纹理偏移随机）
- `calcMipmapLevel(shapeSize, rendWidth)` — 按形状尺寸选 mipmap 级别

**② 笔刷形状 = 形状纹理 + 程序化距离场 + hardness 边缘**

fragment shader 用 `shapeTex`（笔刷形状 alpha 纹理）+ `dist`（圆形/方形距离场）+ `hardness`（`smoothstep(hardness,1.0,dist)` 控制边缘软硬），生成笔刷 stamp 的 alpha。`sumShapeTexAlp()` 用 fwidth 做边缘抗锯齿。

**③ 纸纹/纹理叠加**：`tex`（纸纹纹理）+ `contrastMatrix`/`brightnessMatrix` + `texBlendType`（12 种纹理混合模式：normal/multiply/screen/overlay…在 shader 内以 alpha 公式实现）。

**④ 合成 = premultiplied alpha 的 blit**：`blitType` 三态 —— `BlitType_Direct` / `BlitType_Rremul`（reverse premultiply）/ `BlitType_dePremul`（de-premultiply）。合成到图层时统一走预乘 `over` 运算。

### 8.4 多图层机制

每个图层 = 一张独立纹理（framebuffer），合成 = 源图层 `srcImage` blit 到目标，shader 同时传入：
- `layerMaskTex`（图层蒙版，黑白灰遮挡）
- `lockAlphaTex`（阿尔法锁定，只在已画像素区域写）
- `selMask`（选区裁剪）
- 完整色彩管理：`colSpaTex`（256×256 3D LUT）+ `connTransf`（颜色矩阵）+ EOTF/OETF（gamma）

### 8.5 对原方案的启示（已并入第 4.5 节）

1. **合成用 premultiplied alpha**（非 straight alpha）——本方案已改为 `dst = src + dst*(1-src.a)`。
2. **图层 = 独立纹理，合成 = blit**——原型单图层即可，但合成 shader 预留 `layerMask`/`lockAlpha`/`selMask` 三个 uniform 扩展位，为后续多图层铺路。
3. **笔刷形状 GPU 化**：画世界PRO 用 `shapeTex + 距离场 + hardness` 程序化生成；本原型仍由 **libmypaint 生成 stamp alpha**（用户指定），二者等价——libmypaint 的 dab 即 `shapeTex`，合成时同样走 premultiplied。若后续要极致性能，可评估把 libmypaint 的 dab 形状烘焙成 `shapeTex` 上传，顶点传压力，把形状生成也搬到 GPU（对齐画世界PRO）。
4. **高质量重采样**：stamp 缩放/旋转时用 bicubic（画世界PRO 有 `texbicubic`/`highQFilt`）；原型可先 linear，性能瓶颈再上 bicubic。

## 9. 参考资源

- [Jetpack Ink 官方文档（Android Developers）](https://developer.android.com/develop/ui/compose/touch-input/stylus-input/about-ink-api)
- [Jetpack Ink 版本发布页](https://developer.android.com/jetpack/androidx/releases/ink)
- [mypaint_ffi（libmypaint Android 移植参考实现）](https://pub.dev/packages/mypaint_ffi)
- [MyPaint 社区：How to make it for Android](https://community.mypaint.app/t/how-to-make-it-for-android/3965)
- [Android 手写渲染技术演进（前缓冲 / Ink API）](https://dorck.cn/android/2026/04/12/android-ink-api-compose/)
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/)
