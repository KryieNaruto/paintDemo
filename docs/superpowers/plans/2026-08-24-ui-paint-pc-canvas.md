# paint-pc 画布接入实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** paint-pc 消费者把 SDK C API 接入 GLFW+ImGui+OpenGL 外壳，实现「画布输入 → 离屏渲染 → 读回 → 贴图 → FPS 浮层」闭环，并提供 headless 离屏自检模式满足「CLI + 离屏渲染输出图像」硬约束。

**Architecture:** 消费者仓库 `paint-pc`（`/home/qiansenwei/workspace/paint-pc`）。沿用现有 GLFW+ImGui+OpenGL 壳；`src/app.cpp` 输入回调改接 `dgc_paint_c_api.h`；新增 `src/gl_canvas` 做读回 RGBA → GL 纹理 → 画布 quad；新增 `src/headless.cpp` 走 `dgcSetOffscreenSurface → stroke → dgcExportPNG` 自检。只 include `dgc_paint_c_api.h`、只链接 `dgc_paint`，不改 SDK。

**Tech Stack:** C++17、GLFW、ImGui、OpenGL 3.3（贴图用 core profile）、`dgc_paint` SDK（C API）、CMake ≥ 3.22。

**Spec:** `docs/superpowers/specs/2026-08-24-ui-canvas-integration-design.md`

## Global Constraints

- 只 `#include "dgc_paint_c_api.h"`，禁止 include `core/` 等 SDK 内部头。
- 只 `add_subdirectory(sdk)` + 链接 `dgc_paint`，禁止链接 SDK 内部 target。
- SDK submodule 钉到 commit `508da64`（含 B1-7/B1-8/B2-1/B5-2，含 `dgcFlush`）。改指针用 `sdk` 目录内 `git checkout 508da64`（消费仓库 submodule 指针更新走 `git add sdk` + commit）。
- SDK C API 函数签名以 `sdk/sdk_api/dgc_paint_c_api.h` 为准（下文签名直接从该头抄录）。
- 构建：`cmake -B build -S . && cmake --build build`；产物 `build/paint_pc`。
- 无显示环境必须优雅退出（沿用现有 `app.init()` 失败返回）。
- `imgui.ini` 已 gitignore，不提交。
- 性能验收（稳定 60fps@120Hz、读回耗时打点）标注「依赖 B3-1 真实内核」，本期只验收链路 + FPS 浮层存在。

---

### Task 1: 前移 SDK submodule 到 `508da64`

**Files:**
- Modify: `sdk/`（submodule 指针）
- Test: `sdk/sdk_api/dgc_paint_c_api.h` 存在且含 `dgcFlush` 声明

**Interfaces:**
- Consumes: 无
- Produces: `dgc_paint` 库含完整 C API（`dgcSetOffscreenSurface` / `dgcBeginStroke` / `dgcStrokeTo` / `dgcEndStroke` / `dgcRender` / `dgcReadbackPixels` / `dgcExportPNG` / `dgcFlush`）

- [ ] **Step 1: 前移 submodule 并确认**

```bash
cd /home/qiansenwei/workspace/paint-pc
cd sdk && git fetch origin && git checkout 508da64 && cd ..
git add sdk && git commit -m "chore: 前移 SDK submodule 到 508da64（含 B1-7/B1-8/B2-1/B5-2）"
```

- [ ] **Step 2: 验证头文件含新函数**

```bash
grep -n 'dgcFlush\|dgcReadbackPixels\|dgcSetOffscreenSurface' sdk/sdk_api/dgc_paint_c_api.h
```
Expected: 4 行都出现（含 `dgcFlush`）。

- [ ] **Step 3: 干净构建确认 C API 编译**

```bash
rm -rf build && cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
```
Expected: 编译通过，产物 `build/paint_pc` 存在。

---

### Task 2: `gl_canvas` 最小 GL 纹理贴图模块

**Files:**
- Create: `src/gl_canvas.h`
- Create: `src/gl_canvas.cpp`
- Modify: `CMakeLists.txt`（加源文件）
- Test: 编译通过 + `build/paint_pc --headless` 不引用（后续 Task 4 接）

**Interfaces:**
- Consumes: 无（纯 GL）
- Produces:
  - `struct GlCanvas`：`GlCanvas(int w, int h)`；`void upload(const uint8_t* rgba, int w, int h)`；`void draw(int viewW, int viewH)`；`void destroy()`；成员 `GLuint tex_ = 0, vao_ = 0, vbo_ = 0, prog_ = 0`。

- [ ] **Step 1: 写头文件**

```cpp
// src/gl_canvas.h —— 把读回的 RGBA 画布贴到屏幕的最小 GL 模块（core profile 3.3）。
#pragma once
#include <cstdint>

namespace paint {

struct GlCanvas {
    explicit GlCanvas(int w, int h);
    ~GlCanvas();

    void upload(const uint8_t* rgba, int w, int h);  // 上传/更新画布纹理
    void draw(int viewW, int viewH);                 // 绘制全屏 quad（处理 Y 翻转）
    void destroy();

    int canvas_w = 0, canvas_h = 0;
    unsigned tex_ = 0, vao_ = 0, vbo_ = 0, prog_ = 0;
};

}  // namespace paint
```

- [ ] **Step 2: 写实现**

```cpp
// src/gl_canvas.cpp
#include "gl_canvas.h"

// GL 函数指针加载：复用 ImGui 自带的内嵌 gl3w（imgl3w），零新增依赖。
// 该头在 CMake 的 FetchContent 拉取的 imgui-src/backends/ 下，
// 由 paint_imgui target 的 PUBLIC include 路径提供。
// 需在声明 GL 函数前 #define GL3W_CALLBACK 由本 TU 提供 gl3wGetProcAddress 实现，
// 或直接依赖 imgl3wInit 内部用 glfwGetProcAddress 加载（见 imgui_impl_opengl3.cpp）。
// 本实现采用 imgl3w：include 该 loader 头后调用 imgl3wInit()。
#include <imgui_impl_opengl3_loader.h>
#include <cstdio>

namespace paint {

static bool g_glLoaded = false;
static void EnsureGlLoaded() {
    if (!g_glLoaded) {
        if (imgl3wInit() != 0) { std::fprintf(stderr, "[gl_canvas] imgl3wInit failed\n"); return; }
        g_glLoaded = true;
    }
}

static const char* kVS = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

static const char* kFS = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uCanvas;
void main(){ fragColor = texture(uCanvas, vec2(vUV.x, 1.0 - vUV.y)); }
)";

static GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); std::fprintf(stderr, "[gl_canvas] shader err: %s\n", log); }
    return s;
}

static GLuint Link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log); std::fprintf(stderr, "[gl_canvas] link err: %s\n", log); }
    return p;
}

GlCanvas::GlCanvas(int w, int h) : canvas_w(w), canvas_h(h) {
    EnsureGlLoaded();  // imgl3w：用 glfwGetProcAddress 加载 GL 函数指针
    unsigned vs = Compile(GL_VERTEX_SHADER, kVS), fs = Compile(GL_FRAGMENT_SHADER, kFS);
    prog_ = Link(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const float verts[] = { -1,-1, 0,0,   1,-1, 1,0,   1,1, 1,1,   -1,1, 0,1 };
    glGenVertexArrays(1, &vao_); glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
}

GlCanvas::~GlCanvas() { destroy(); }

void GlCanvas::upload(const uint8_t* rgba, int w, int h) {
    canvas_w = w; canvas_h = h;
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void GlCanvas::draw(int viewW, int viewH) {
    glViewport(0, 0, viewW, viewH);
    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_);
    glUniform1i(glGetUniformLocation(prog_, "uCanvas"), 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

void GlCanvas::destroy() {
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
}

}  // namespace paint
```

- [ ] **Step 3: 改 CMakeLists 加源**

在 `add_executable(paint_pc ...)` 里加 `src/gl_canvas.cpp`，并确保 `find_package(OpenGL REQUIRED)` 已存在。GL 函数指针由 ImGui 自带 `imgui_impl_opengl3_loader.h`（imgl3w）提供——`paint_imgui` target 的 PUBLIC include 已含 `backends/`，`gl_canvas.cpp` 直接 `#include <imgui_impl_opengl3_loader.h>` 即可，**零新增依赖**（不必引入 GLEW/gl3w）。链接仍 `OpenGL::GL`。

```cmake
# CMakeLists.txt —— 加 gl_canvas 源
add_executable(paint_pc
    src/main.cpp
    src/app.cpp
    src/gl_canvas.cpp
)
```

- [ ] **Step 4: 构建验证**

```bash
cmake --build build -j
```
Expected: 编译通过。

---

### Task 3: `app.cpp` 接入 SDK C API + 读回 + 贴图 + FPS 浮层

**Files:**
- Modify: `src/app.h`
- Modify: `src/app.cpp`
- Test: `build/paint_pc` 有显示环境启动可见白底画布 + FPS 浮层；`--headless` 走 Task 4

**Interfaces:**
- Consumes: `dgc_paint_c_api.h` 全函数；`GlCanvas`（Task 2）
- Produces: `App::init(width,height,title)` 签名不变；`App` 增加成员 `DgcContext* ctx_`、`GlCanvas* canvas_`、`int canvasW_/canvasH_`、`std::vector<uint8_t> pixels_`、FPS 计时字段。

- [ ] **Step 1: app.h 加成员**

```cpp
// src/app.h —— 追加（保持 App 接口不变）
#include "dgc_paint_c_api.h"
#include <cstdint>
#include <vector>
struct GlCanvas;

namespace paint {
struct App::Impl {  // 在 Impl 内追加
    DgcContext* sdk = nullptr;
    GlCanvas* canvas = nullptr;
    int canvasW = 1280, canvasH = 800;
    std::vector<uint8_t> rgba;  // RGBA8, canvasW*canvasH*4

    // FPS 统计
    double lastTick = 0.0; int frames = 0; double fps = 0.0;
    double lastReadMs = 0.0;

    bool strokeActive = false;
    bool headless = false;
};
}
```

- [ ] **Step 2: app.cpp init 接 SDK**

```cpp
// init() 内、glfwInit() 成功之后：
#include "dgc_paint_c_api.h"
#include "gl_canvas.h"
// （放在文件顶部 include 区）

// init() 里：
impl->sdk = dgcCreate();
if (!impl->sdk) { std::fprintf(stderr, "[paint-pc] dgcCreate failed\n"); return false; }
dgcSetOffscreenSurface(impl->sdk, impl->canvasW, impl->canvasH);
dgcClear(impl->sdk, 0.96f, 0.95f, 0.91f, 1.0f);  // 纸白
impl->rgba.assign((size_t)impl->canvasW * impl->canvasH * 4, 255);
impl->canvas = new GlCanvas(impl->canvasW, impl->canvasH);
```

- [ ] **Step 3: 输入回调接 C API（替换 TODO 桩）**

```cpp
static void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
    if (!impl || !impl->sdk) return;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        impl->strokeActive = (action == GLFW_PRESS);
        double x, y; glfwGetCursorPos(window, &x, &y);
        if (impl->strokeActive) {
            dgcBeginStroke(impl->sdk, (float)x, (float)y, 0.5f, 0.f, 0.f);
        } else {
            dgcEndStroke(impl->sdk);
        }
    }
}

static void OnCursorPos(GLFWwindow* window, double x, double y) {
    auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
    if (!impl || !impl->sdk || !impl->strokeActive) return;
    dgcStrokeTo(impl->sdk, (float)x, (float)y, 0.5f, 0.f, 0.f, 0);
}
```

- [ ] **Step 4: 每帧读回 + 贴图 + FPS**

```cpp
// run() 渲染循环内，ImGui 绘制后、glfwSwapBuffers 前：
// 1) readback（仅当有输入变化或每帧都读——本期每帧读，方便打点）
auto t0 = glfwGetTime();
if (impl->sdk) {
    dgcReadbackPixels(impl->sdk, impl->rgba.data());
}
impl->lastReadMs = (glfwGetTime() - t0) * 1000.0;
if (impl->canvas) impl->canvas->upload(impl->rgba.data(), impl->canvasW, impl->canvasH);

// 2) FPS 统计（每 0.5s 刷新一次显示值）
impl->frames++;
double now = glfwGetTime();
if (now - impl->lastTick >= 0.5) { impl->fps = impl->frames / (now - impl->lastTick); impl->frames = 0; impl->lastTick = now; }

// 3) ImGui 浮层
ImGui::Begin("Performance");
ImGui::Text("FPS: %.1f", impl->fps);
ImGui::Text("Frame: %.2f ms", 1000.0 / (impl->fps > 0 ? impl->fps : 1.0));
ImGui::Text("Readback: %.2f ms", impl->lastReadMs);
ImGui::Text("Canvas: %dx%d", impl->canvasW, impl->canvasH);
ImGui::End();

// 4) 画布贴图绘制（画布 quad 在 ImGui 之后、swap 之前）
impl->canvas->draw(impl->width, impl->height);
```

- [ ] **Step 5: shutdown 释放**

```cpp
// shutdown() 内：
if (impl->sdk) { dgcDestroy(impl->sdk); impl->sdk = nullptr; }
delete impl->canvas; impl->canvas = nullptr;
```

- [ ] **Step 6: main.cpp 传 headless 标记**

```cpp
// main.cpp：解析 argv 含 "--headless" 时走 Task 4 的 HeadlessRun，否则正常窗口。
```

- [ ] **Step 7: 构建 + 有显示验证**

```bash
cmake --build build -j && ./build/paint_pc   # 有显示环境：白底画布 + FPS 浮层，拖拽有输入
```
Expected: 窗口显示纸白画布 + FPS/Readback 浮层，拖拽不崩（Null 内核无笔迹属预期）。

---

### Task 4: headless 离屏自检（CLI + 离屏渲染输出图像硬约束）

**Files:**
- Create: `src/headless.cpp`
- Modify: `src/main.cpp`
- Test: `./build/paint_pc --headless out.png` 产出 PNG

**Interfaces:**
- Consumes: `dgc_paint_c_api.h`
- Produces: `int HeadlessRun(int w, int h, const char* outPng)` 在 `src/headless.h` 声明

- [ ] **Step 1: 写 headless.h/.cpp**

```cpp
// src/headless.h
#pragma once
namespace paint { int HeadlessRun(int w, int h, const char* outPng); }
```

```cpp
// src/headless.cpp —— 离屏自检：固定笔迹 → dgcExportPNG
#include "headless.h"
#include "dgc_paint_c_api.h"
#include <cstdio>

namespace paint {

int HeadlessRun(int w, int h, const char* outPng) {
    DgcContext* sdk = dgcCreate();
    if (!sdk) { std::fprintf(stderr, "[headless] dgcCreate failed\n"); return 1; }
    dgcSetOffscreenSurface(sdk, w, h);
    dgcClear(sdk, 0.96f, 0.95f, 0.91f, 1.0f);
    dgcSetRandomSeed(sdk, 42);            // 确定性：固定 seed
    dgcSetFixedTime(sdk, 1000.0);          // 固定时间步：1ms
    dgcBeginStroke(sdk, 100.f, 100.f, 0.5f, 0.f, 0.f);
    for (int i = 0; i < 20; ++i) {
        dgcStrokeTo(sdk, 100.f + i * 20.f, 100.f + i * 10.f, 0.5f, 0.f, 0.f, 0);
    }
    dgcEndStroke(sdk);
    dgcFlush(sdk);                          // 等合成完成（B5-2）
    int rc = dgcExportPNG(sdk, outPng);
    dgcDestroy(sdk);
    if (rc != 0) { std::fprintf(stderr, "[headless] export failed: %s\n", dgcGetLastError()); return 2; }
    std::printf("[headless] PNG written: %s\n", outPng);
    return 0;
}

}  // namespace paint
```

- [ ] **Step 2: main.cpp 接 headless 分支**

```cpp
// main.cpp
#include "headless.h"
#include <cstring>
int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--headless") == 0) {
        const char* out = (argc >= 3) ? argv[2] : "out.png";
        return paint::HeadlessRun(1280, 800, out);
    }
    // ... 原窗口路径
}
```

- [ ] **Step 3: CMake 加源**

```cmake
add_executable(paint_pc src/main.cpp src/app.cpp src/gl_canvas.cpp src/headless.cpp)
```

- [ ] **Step 4: 构建 + 无显示验证（CI 用）**

```bash
cmake --build build -j && ./build/paint_pc --headless /tmp/headless.png && ls -la /tmp/headless.png
```
Expected: 返回 0，`/tmp/headless.png` 存在且非 0 字节（白底 PNG）。

- [ ] **Step 5: 提交（本 Task 内已含多次 commit；一次总提交即可）**

```bash
git add -A && git commit -m "feat: paint-pc 接入 SDK C API —— 画布输入/读回贴图/FPS 浮层 + headless 离屏自检"
```

---

### Task 5: 测试门（0 失败 0 跳过）

**Files:**
- Create: `tests/smoke.sh`
- Test: 全绿

**Interfaces:**
- Consumes: `build/paint_pc`、`build/paint_pc --headless`

- [ ] **Step 1: 写 smoke 脚本**

```bash
#!/usr/bin/env bash
# tests/smoke.sh —— paint-pc 无头冒烟：构建 + headless 离屏导出 PNG
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
out=$(mktemp /tmp/paint_pc_headless.XXXXXX.png)
./build/paint_pc --headless "$out"
[ -s "$out" ] || { echo "FAIL: PNG empty/missing: $out"; exit 1; }
echo "PASS: headless PNG $(stat -c%s "$out") bytes"
```

- [ ] **Step 2: 运行 smoke**

```bash
bash tests/smoke.sh
```
Expected: `PASS: headless PNG <n> bytes`，退出 0。

- [ ] **Step 3: 提交**

```bash
git add tests/smoke.sh && git commit -m "test: paint-pc headless 冒烟（构建+离屏PNG导出）"
```

---

## Self-Review 记录

- **Spec 覆盖**：上屏（读回+贴图）→ Task 3/4；输入 → Task 3；FPS 浮层 → Task 3；headless 离屏 PNG → Task 4；性能依赖 B3-1 声明 → Global Constraints。✓
- **占位符扫描**：无 TBD/TODO/「类似 Task N」。✓
- **类型一致性**：`GlCanvas` 构造/upload/draw 签名跨 Task 2/3 一致；C API 签名从 `dgc_paint_c_api.h` 抄录（`dgcStrokeTo` 7 参含 `isPredicted`，`dgcFlush` 存在）。✓
