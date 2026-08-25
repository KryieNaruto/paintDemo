# U2 · UI 双线真实绘制增量计划（paint-pc / paint-android）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在已完成 UI 接入（读回+贴图+FPS 浮层）的消费者双线上，前移 SDK submodule 到含 B3-1 真实笔刷内核的 `9e6eefb`，把「无笔迹的 Null 绘制」推进到「真实笔迹离屏可验证」；Android 侧同时翻 `DGCPAIN_RENDER_VULKAN=ON`（B5-1 已解决 NDK 无 shaderc 的 arm64 编译阻塞）。满足「CLI + 离屏渲染输出图像」硬约束。

**Spec:** `docs/superpowers/specs/2026-08-24-ui-canvas-integration-design.md`（上一期 UI 接入 spec）
**前置已确认：**
- SDK 16/16 任务全通过，`origin/main` = `9e6eefb`（B3-1 真实内核 + B5-1 arm64 Vulkan + B5-3 确定性 golden）。
- SDK C API 头 `508da64 → 9e6eefb` **零变化** → 消费者源码无需改动，仅前移 submodule。
- 两消费者仓库当前 `main`：paint-pc=`ef64bd1`、paint-android=`db5ccb2`，submodule 均钉 `508da64`。
- 本机环境：cmake 3.28 + NDK 28.2（含 glslc）+ JDK 21 + 无 DISPLAY（无头）。

## Global Constraints

- **CLI + 离屏硬约束**：paint-pc `--headless out.png` 离屏导出 PNG 为无头验收主证据；paint-android 以 `DemoExportActivity`（exported）触发 `dgcExportPNG` 到缓存目录为离屏自检通道。
- 只 `#include "dgc_paint_c_api.h"`，禁止 include `core/` 等 SDK 内部头；只 `add_subdirectory(sdk)` + 链接 `dgc_paint`。
- 只改消费者仓库文件；**不改 SDK**。
- SDK submodule 前移用 `git -C sdk fetch origin && git -C sdk checkout 9e6eefb` + 消费者仓库 `git add sdk` + commit（submodule 指针更新）。
- C API 签名以 `sdk/sdk_api/dgc_paint_c_api.h` 为准；C API 头零变化 → 不新增/修改消费者 C API 调用代码。
- 工作方式沿用上一期：消费者改动在**独立 worktree**（`paint-pc-ui-impl` / `paint-android-ui-impl`，分支 `ui-canvas-impl`）内完成并提交，评审/测试通过后合并回各 `main` 并 push。
- build-pipeline 门禁：审阅门 ≥80、测试门 100 分（0 失败 0 跳过）。

---

### Task 1: paint-pc —— 前移 submodule + smoke 断言升级「含真实笔迹像素」

**Files:**
- Modify: `paint-pc/sdk/`（submodule 指针 → `9e6eefb`）
- Modify: `paint-pc/tests/smoke.sh`（断言从「非空文件」升级为「含真实笔迹像素」）
- Test: `build/paint_pc --headless out.png` 产出 PNG 且含笔迹像素；`tests/smoke.sh` 全绿

**Interfaces:**
- Consumes: `dgc_paint_c_api.h`（零变化）；`build/paint_pc --headless`
- Produces: 含 B3-1 真实内核的 `dgc_paint`；smoke 门升级为「离屏 PNG 含笔迹像素」断言

- [ ] **Step 1: 前移 submodule 到 `9e6eefb`**

在 `paint-pc-ui-impl` worktree（分支 `ui-canvas-impl`）内：
```bash
cd /home/qiansenwei/workspace/paint-pc-ui-impl   # 若无则 git worktree add ../paint-pc-ui-impl -b ui-canvas-impl
git -C sdk fetch origin
git -C sdk checkout 9e6eefb
git add sdk && git commit -m "chore: 前移 SDK submodule 到 9e6eefb（含 B3-1 真实内核 + B5-1 arm64 Vulkan）"
```
> 前提：submodule origin（`https://github.com/KryieNaruto/paintDemo.git`）可达且含 `9e6eefb`（已验证在远程 main 上）。离线/网络失败：`git -C sdk fetch origin 9e6eefb` 或本地引用。

- [ ] **Step 2: 验证 submodule 指针 + C API 头**

```bash
git submodule status   # 期望 9e6eefb...
grep -n 'dgcFlush' sdk/sdk_api/dgc_paint_c_api.h   # 期望存在
```

- [ ] **Step 3: 升级 smoke.sh 断言（非空 → 含笔迹像素）**

替换 smoke.sh 中 `[ -s "$out" ]` 的朴素断言，改为：构建 + `--headless` 导出 PNG + **用 Python 校验 PNG 非纯白背景（存在与背景色不同的像素 = 真实笔迹）**。

```bash
#!/usr/bin/env bash
# tests/smoke.sh —— paint-pc 无头冒烟：构建 + headless 离屏导出 PNG + 含笔迹像素断言
set -euo pipefail
cd "$(dirname "$0")/.."
DGCPAIN_DEPS_ROOT="${DGCPAIN_DEPS_ROOT:-/tmp/dgc-deps/usr}"
PC_X11_DEPS_ROOT="${PC_X11_DEPS_ROOT:-/home/qiansenwei/.local/dgc-x11dev/usr}"
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
    -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF \
    -DDGCPAIN_DEPS_ROOT="$DGCPAIN_DEPS_ROOT" \
    -DCMAKE_PREFIX_PATH="$PC_X11_DEPS_ROOT" \
    -DCMAKE_C_FLAGS="-I$PC_X11_DEPS_ROOT/include" \
    -DCMAKE_CXX_FLAGS="-I$PC_X11_DEPS_ROOT/include"
cmake --build build -j
out=$(mktemp /tmp/paint_pc_headless.XXXXXX.png)
./build/paint_pc --headless "$out"
[ -s "$out" ] || { echo "FAIL: PNG empty/missing: $out"; exit 1; }

# 真实笔迹断言：B3-1 真实内核下，固定笔迹（seed=42，黑色）必须产生与纸白背景不同的像素。
# 纸白背景 = dgcClear(0.96,0.95,0.91) ≈ (245,242,232)。笔迹为黑 → 存在明显更暗的像素。
python3 - "$out" <<'PY'
import sys, zlib, struct
png = open(sys.argv[1], 'rb').read()
assert png[:8] == b'\x89PNG\r\n\x1a\n', "not a PNG"
# 解析首个 IDAT，解压出所有 scanline
pos = 8; idat = b''; w = h = None
while pos < len(png):
    ln = struct.unpack('>I', png[pos:pos+4])[0]; typ = png[pos+4:pos+8]; dat = png[pos+8:pos+8+ln]
    if typ == b'IHDR':
        w, h = struct.unpack('>II', dat[:8]); depth, ctype = dat[8], dat[9]
        assert depth == 8 and ctype == 6, "expected RGBA8"
    elif typ == b'IDAT':
        idat += dat
    elif typ == b'IEND':
        break
    pos += 12 + ln
raw = zlib.decompress(idat)
bpp = 4
stride = w * bpp
# 背景像素（取左上角 1 个像素作为背景参考）
def px(x, y):
    row = raw[y * (stride + 1) + 1: y * (stride + 1) + 1 + stride]
    o = x * bpp
    return row[o], row[o+1], row[o+2], row[o+3]
bg = px(0, 0)
dark = 0
for y in range(0, h, 3):      # 隔行抽样足够（笔迹对角线贯穿画面）
    for x in range(0, w, 3):
        r, g, b, a = px(x, y)
        if a > 0 and (r, g, b) != bg and (r + g + b) < (bg[0] + bg[1] + bg[2]) - 60:
            dark += 1
print(f"bg={bg} dark_pixels={dark}")
[ "$dark" -gt 50 ] || { echo "FAIL: no real stroke pixels (dark=$dark)"; exit 1; }
echo "PASS: headless PNG $(stat -c%s "$out") bytes, ${dark} stroke pixels"
PY
```

> 说明：`headless.cpp` 固定笔迹为 (100,100) 起、20 步 (20,10) 递增的黑色斜线（seed=42），B3-1 真实内核下必产生黑色像素；Null 内核下 PNG 纯白背景、dark=0 → 断言 FAIL。此断言同时是「B3-1 真实内核已生效」的机器可验证据。

- [ ] **Step 4: 构建 + headless 验证 + smoke**

```bash
cmake --build build -j
./build/paint_pc --headless /tmp/u2_pc.png && python3 -c "from PIL import Image; im=Image.open('/tmp/u2_pc.png'); print(im.size, len(set(im.getdata())))"
bash tests/smoke.sh
```
Expected: headless 返回 0，PNG 存在且含黑色笔迹像素（颜色数 > 1）；smoke 打印 `PASS: ... N stroke pixels`。

- [ ] **Step 5: 提交**

```bash
git add -A && git commit -m "test: smoke 断言升级 —— headless 离屏 PNG 含真实笔迹像素（B3-1 内核验证）"
```

---

### Task 2: paint-android —— 前移 submodule + RENDER_VULKAN=ON

**Files:**
- Modify: `paint-android/sdk/`（submodule 指针 → `9e6eefb`）
- Modify: `paint-android/app/build.gradle.kts`（`-DDGCPAIN_RENDER_VULKAN=OFF` → **ON** + 注释更新）
- Test: `./gradlew assembleDebug` 编译门（arm64-v8a + Vulkan ON）+ DemoExport 离屏自检（编译期可达性）

**Interfaces:**
- Consumes: `dgc_paint_c_api.h`（零变化）；NDK 28.2 自带 glslc（B5-1 构建期预编译 SPIR-V）
- Produces: 带真实 Vulkan 后端的 `libdgc_paint.so`（arm64-v8a）+ 全 C API JNI 桥

- [ ] **Step 1: 前移 submodule 到 `9e6eefb`**

在 `paint-android-ui-impl` worktree（分支 `ui-canvas-impl`）内：
```bash
cd /home/qiansenwei/workspace/paint-android-ui-impl   # 若无则 git worktree add ../paint-android-ui-impl -b ui-canvas-impl
git -C sdk fetch origin
git -C sdk checkout 9e6eefb
git add sdk && git commit -m "chore: 前移 SDK submodule 到 9e6eefb（含 B3-1 真实内核 + B5-1 arm64 Vulkan）"
```

- [ ] **Step 2: 翻 `DGCPAIN_RENDER_VULKAN=ON`**

`app/build.gradle.kts` 的 `externalNativeBuild.cmake.arguments`：
```diff
-                // 环境修复：SDK B2-1 Vulkan 离屏后端需 shaderc，NDK 无 arm64 预编译
-                // shaderc（仅源码，缺 glslang/spirv-tools），host amd64 lib 无法交叉链接进
-                // arm64-v8a。按 SDK 官方 Android 口径「Null 后端不硬依赖」关闭 Vulkan，
-                // 保证 assembleDebug 编译门全绿；真机离屏渲染依赖 arm64 shaderc 构建（后续项）。
-                arguments += "-DDGCPAIN_RENDER_VULKAN=OFF"
+                // B5-1 已解决：NDK 无 libshaderc → 改用 NDK 自带 glslc 构建期把
+                // brush_composite.comp 预编译为 SPIR-V 内嵌字节数组。arm64-v8a 现在
+                // 可编出带真实 Vulkan 后端（VkBackend）的 libdgc_paint.so。
+                arguments += "-DDGCPAIN_RENDER_VULKAN=ON"
```

- [ ] **Step 3: 编译门（assembleDebug，Vulkan-ON）**

```bash
cd /home/qiansenwei/workspace/paint-android-ui-impl
./gradlew assembleDebug --no-daemon
```
Expected: BUILD SUCCESSFUL；`app/build/outputs/apk/debug/app-debug.apk` 存在；`sdk` 侧在 NDK 下经 glslc 编出带 Vulkan 后端的 `libdgc_paint.so` 并链接进 `libpaint_android_jni.so`。
> 以实际构建为准（IDE clang 诊断是误报口）。无 arm64 设备 → 真机运行验证标人工后续项（B5-1 口径「只保证编出 .so」）。

- [ ] **Step 4: DemoExport 离屏自检可达性**

代码审阅确认 `DemoExportActivity` 仍 exported=true、触发 `dgcExportPNG` 到缓存目录；本机无 arm64 设备/模拟器 → 离屏导出运行验证标人工后续项，编译门 + 代码审阅为机器内验证。

- [ ] **Step 5: 提交**

```bash
git add -A && git commit -m "feat: android 翻 DGCPAIN_RENDER_VULKAN=ON —— B5-1 后 arm64 可编真实 Vulkan 后端"
```

---

### Task 3: 测试门（0 失败 0 跳过）

**Files:**
- Run: `paint-pc/tests/smoke.sh`（升级版，含笔迹像素断言）
- Run: `paint-android` `./gradlew assembleDebug`（Vulkan-ON）
- Verify: 双仓 `git submodule status` = `9e6eefb`；`git status` 干净（worktree 内）

**Interfaces:**
- Consumes: Task 1/2 的产物

- [ ] **Step 1: paint-pc 测试**
```bash
cd /home/qiansenwei/workspace/paint-pc-ui-impl && bash tests/smoke.sh
```
Expected: `PASS: ... N stroke pixels`（N>50），退出 0。失败 → 修复重测循环。

- [ ] **Step 2: paint-android 测试**
```bash
cd /home/qiansenwei/workspace/paint-android-ui-impl && ./gradlew assembleDebug --no-daemon
```
Expected: BUILD SUCCESSFUL。失败 → 修复重测循环。

- [ ] **Step 3: 收尾前核验**
```bash
git -C ../paint-pc-ui-impl submodule status && git -C ../paint-android-ui-impl submodule status
```
Expected: 两仓均 `9e6eefb...`；worktree 内 `git status` 无未提交改动。

---

## Self-Review 记录

- **CLI + 离屏硬约束**：paint-pc `--headless` 离屏 PNG（Task 1）；paint-android DemoExport 离屏导出（Task 2）。✓
- **C API 零变化**：SDK C API 头 `508da64→9e6eefb` 无 diff，故消费者**不新增/修改**任何 C API 调用代码，仅前移 submodule + 构建参数。✓
- **Android Vulkan 翻 ON 依据**：B5-1 计划明确「NDK 无 libshaderc → glslc 构建期预编译 SPIR-V 内嵌」；`render/vulkan/CMakeLists.txt` 已实现 `DGCPAIN_ANDROID` 分支。✓
- **无 DISPLAY / 无真机**：PC 上屏拖拽、Android 真机运行均标人工后续项；headless 离屏 PNG 像素断言 + Vulkan-ON 编译门为机器内最强验证。✓
- **占位符扫描**：无 TBD/TODO。✓
- **范围控制**：只改两消费者仓库的 submodule 指针 + smoke.sh + build.gradle.kts 一行参数，不扩范围。✓
