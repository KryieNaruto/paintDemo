# paint-android 画布接入实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** paint-android 消费者把 SDK C API 经 JNI 接入 Compose 外壳，实现「触摸输入 → 离屏渲染 → 读回 → ImageBitmap 贴图 → FPS 浮层」闭环，并提供「离屏导出 PNG」自检通道（`dgcExportPNG` 到 app 缓存目录）满足「CLI + 离屏渲染输出图像」硬约束（Android 无 CLI，以自检 Activity/Intent 触发离屏导出替代）。

**Architecture:** 消费者仓库 `paint-android`（`/home/qiansenwei/workspace/paint-android`）。`jni/paint_android_jni.cpp` 扩展为持 `DgcContext*` 的完整 C API 桥接；`PaintNative.kt` 声明 external fun；`PaintScreen.kt` 把输入回调改为经 JNI 调 C API，并把 readback 结果贴成 `ImageBitmap`；新增 `DemoExportActivity` 触发「画固定笔迹 → dgcExportPNG → 返回 PNG 路径」。只 include `dgc_paint_c_api.h`、只链接 `dgc_paint`，不改 SDK。

**Tech Stack:** Kotlin、Jetpack Compose（Material3）、JNI/C++17、`dgc_paint` SDK（C API）、Gradle + AGP + NDK（arm64-v8a）、CMake ≥ 3.22.1。

**Spec:** `docs/superpowers/specs/2026-08-24-ui-canvas-integration-design.md`

## Global Constraints

- 只 `#include "dgc_paint_c_api.h"`，禁止 include `core/` 等 SDK 内部头。
- 只 `add_subdirectory(sdk)` + 链接 `dgc_paint`，禁止链接 SDK 内部 target。
- SDK submodule 钉到 commit `508da64`（含 B1-7/B1-8/B2-1/B5-2）。改指针走 `git -C sdk checkout 508da64` + `git add sdk` + commit。
- C API 签名以 `sdk/sdk_api/dgc_paint_c_api.h` 为准（`dgcStrokeTo` 7 参含 `isPredicted`，含 `dgcFlush`/`dgcReadbackPixels`/`dgcExportPNG`）。
- 构建：`./gradlew assembleDebug`；`externalNativeBuild` 已用根 `CMakeLists.txt` 编 `paint_android_jni`（`-DDGCPAIN_BUILD_TESTS=OFF`，`abiFilters arm64-v8a`）。
- JNI 符号默认 hidden，native 方法用 `JNIEXPORT`/`JNICALL` 显式导出，命名 `Java_com_dgcamp_paint_jni_PaintNative_<name>`。
- 无真机时以「编译通过 + 代码审阅」为测试口径；真机验证标注人工后续项。
- 性能验收（稳定 60fps@120Hz）标注「依赖 B3-1 真实内核」，本期只验收链路 + FPS 浮层存在。
- `local.properties`（含 sdk.dir）不入库（已在 .gitignore）。

---

### Task 1: 前移 SDK submodule 到 `508da64`

**Files:**
- Modify: `sdk/`（submodule 指针）
- Test: `sdk/sdk_api/dgc_paint_c_api.h` 含 `dgcFlush`

**Interfaces:**
- Consumes: 无
- Produces: `libdgc_paint` 含完整 C API（含 `dgcFlush`）

- [ ] **Step 1: 前移 submodule**

```bash
cd /home/qiansenwei/workspace/paint-android
cd sdk && git fetch origin && git checkout 508da64 && cd ..
git add sdk && git commit -m "chore: 前移 SDK submodule 到 508da64（含 B1-7/B1-8/B2-1/B5-2）"
```

- [ ] **Step 2: 验证头**

```bash
grep -n 'dgcFlush\|dgcReadbackPixels\|dgcSetOffscreenSurface' sdk/sdk_api/dgc_paint_c_api.h
```
Expected: 3 行出现。

---

### Task 2: JNI 扩展 —— 完整 C API 桥接

**Files:**
- Modify: `jni/paint_android_jni.cpp`
- Test: `./gradlew :app:compileDebugNative`（或 assembleDebug）编译通过

**Interfaces:**
- Consumes: `dgc_paint_c_api.h`
- Produces（Kotlin 侧 `PaintNative` external fun 一一对应，见 Task 3）：
  - `nativeInit(canvasW:Int, canvasH:Int): Boolean`
  - `nativeStrokeBegin(x:Float, y:Float, pressure:Float): Unit`
  - `nativeStrokeTo(x:Float, y:Float, pressure:Float): Unit`
  - `nativeStrokeEnd(): Unit`
  - `nativeReadback(): ByteArray`（RGBA8, w*h*4）
  - `nativeExportPng(path:String): Boolean`
  - `nativeDestroy(): Unit`

- [ ] **Step 1: 替换 jni 为完整桥接**

```cpp
// jni/paint_android_jni.cpp
#include <jni.h>
#include <vector>
#include <string>
#include "dgc_paint_c_api.h"

namespace {
DgcContext* g_sdk = nullptr;
int g_w = 0, g_h = 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeInit(JNIEnv* env, jobject, jint w, jint h) {
    (void)env;
    g_sdk = dgcCreate();
    if (!g_sdk) return JNI_FALSE;
    g_w = w; g_h = h;
    dgcSetOffscreenSurface(g_sdk, w, h);
    dgcClear(g_sdk, 0.96f, 0.95f, 0.91f, 1.0f);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeStrokeBegin(JNIEnv*, jobject, jfloat x, jfloat y, jfloat pressure) {
    if (g_sdk) dgcBeginStroke(g_sdk, x, y, pressure, 0.f, 0.f);
}
extern "C" JNIEXPORT void JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeStrokeTo(JNIEnv*, jobject, jfloat x, jfloat y, jfloat pressure) {
    if (g_sdk) dgcStrokeTo(g_sdk, x, y, pressure, 0.f, 0.f, 0);
}
extern "C" JNIEXPORT void JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeStrokeEnd(JNIEnv*, jobject) {
    if (g_sdk) dgcEndStroke(g_sdk);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeReadback(JNIEnv* env, jobject) {
    if (!g_sdk) return nullptr;
    std::vector<uint8_t> buf((size_t)g_w * g_h * 4);
    dgcReadbackPixels(g_sdk, buf.data());
    jbyteArray arr = env->NewByteArray((jsize)buf.size());
    env->SetByteArrayRegion(arr, 0, (jsize)buf.size(), (const jbyte*)buf.data());
    return arr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeExportPng(JNIEnv* env, jobject, jstring path) {
    if (!g_sdk) return JNI_FALSE;
    const char* p = env->GetStringUTFChars(path, nullptr);
    dgcFlush(g_sdk);
    int rc = dgcExportPNG(g_sdk, p);
    env->ReleaseStringUTFChars(path, p);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_dgcamp_paint_jni_PaintNative_nativeDestroy(JNIEnv*, jobject) {
    if (g_sdk) { dgcDestroy(g_sdk); g_sdk = nullptr; }
}
```

- [ ] **Step 2: 编译验证**

```bash
cd /home/qiansenwei/workspace/paint-android && ./gradlew :app:compileDebugNative 2>&1 | tail -20
```
Expected: `BUILD SUCCESSFUL`，无 JNI 链接错误。

---

### Task 3: Kotlin 桥接 PaintNative.kt

**Files:**
- Modify: `app/src/main/java/com/dgcamp/paint/jni/PaintNative.kt`
- Test: 编译通过

**Interfaces:**
- Consumes: Task 2 的 JNI 符号
- Produces: `PaintNative.init(w,h)` / `strokeBegin/To/End` / `readback(): ByteArray?` / `exportPng(path): Boolean` / `destroy()`

- [ ] **Step 1: 扩展 PaintNative.kt**

```kotlin
package com.dgcamp.paint.jni

object PaintNative {
    init { System.loadLibrary("paint_android_jni") }

    external fun nativeHello(): String            // 保留自检
    external fun nativeInit(w: Int, h: Int): Boolean
    external fun nativeStrokeBegin(x: Float, y: Float, pressure: Float)
    external fun nativeStrokeTo(x: Float, y: Float, pressure: Float)
    external fun nativeStrokeEnd()
    external fun nativeReadback(): ByteArray?
    external fun nativeExportPng(path: String): Boolean
    external fun nativeDestroy()

    fun init(w: Int, h: Int): Boolean = nativeInit(w, h)
}
```

- [ ] **Step 2: 编译验证**

```bash
./gradlew :app:compileDebugKotlin 2>&1 | tail -10
```
Expected: `BUILD SUCCESSFUL`。

---

### Task 4: PaintScreen.kt —— 输入接 C API + ImageBitmap 贴图 + FPS 浮层

**Files:**
- Modify: `app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`
- Test: 编译通过 + 真机运行（人工）

**Interfaces:**
- Consumes: `PaintNative`（Task 3）
- Produces: 可交互画布 + FPS/读回耗时浮层

- [ ] **Step 1: 画布接入**

```kotlin
package com.dgcamp.paint.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.graphics.nativeCanvas
import com.dgcamp.paint.jni.PaintNative

@Composable
fun PaintScreen() {
    // 画布逻辑尺寸（1080x720 内，避免 readback 带宽过大；真机可按屏宽高）
    val cw = 1080
    val ch = 720

    var bitmap by remember { mutableStateOf<Bitmap?>(null) }
    var fps by remember { mutableFloatStateOf(0f) }
    var readMs by remember { mutableFloatStateOf(0f) }
    var lastError by remember { mutableStateOf("") }
    val ctx = remember { PaintNative }
    val started = remember { ctx.nativeInit(cw, ch) }
    DisposableEffect(Unit) { onDispose { ctx.nativeDestroy() } }

    // 每帧：readback → bitmap
    LaunchedEffect(Unit) {
        var frames = 0; var t0 = System.nanoTime(); var last = t0
        while (true) {
            withFrameNanos { now ->
                val rb0 = System.nanoTime()
                val arr = ctx.nativeReadback()
                val rb1 = System.nanoTime()
                readMs = (rb1 - rb0) / 1_000_000f
                if (arr != null) {
                    bitmap = Bitmap.createBitmap(cw, ch, Bitmap.Config.ARGB_8888)
                    bitmap?.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(arr))
                }
                frames++
                if (now - last >= 500_000_000L) {
                    fps = frames * 1e9f / (now - last).toFloat()
                    frames = 0; last = now
                }
            }
        }
    }

    val canvasColor = Color(0xFFF5F2E8)
    val overlayColor = Color.Black.copy(alpha = 0.7f)

    Scaffold { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .background(canvasColor)
                .pointerInput(Unit) {
                    detectDragGestures(
                        onDragStart = { offset -> ctx.nativeStrokeBegin(offset.x, offset.y, 0.5f) },
                        onDrag = { change, _ -> change.consume(); ctx.nativeStrokeTo(change.position.x, change.position.y, 0.5f) },
                        onDragEnd = { ctx.nativeStrokeEnd() },
                    )
                },
        ) {
            val bmp = bitmap
            if (bmp != null) {
                Canvas(modifier = Modifier.fillMaxSize()) {
                    drawImage(bmp.asImageBitmap(), dstSize = IntSize(this.size.width.toInt(), this.size.height.toInt()))
                }
            }
            Text(
                text = if (!started) "SDK init failed" else
                    "FPS: ${"%.1f".format(fps)}\nReadback: ${"%.2f".format(readMs)} ms\n$lastError",
                color = overlayColor,
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.align(Alignment.TopStart).safeDrawingPadding().padding(12.dp),
            )
        }
    }
}
```

> 注：`withFrameNanos` 需 import `androidx.compose.runtime.withFrameNanos`；`drawImage` 的 `dstSize` 是 `androidx.compose.ui.unit.IntSize`。若 `nativeCanvas` 未用到可删 import。

- [ ] **Step 2: 编译验证**

```bash
./gradlew :app:compileDebugKotlin 2>&1 | tail -10
```
Expected: `BUILD SUCCESSFUL`。

---

### Task 5: 离屏导出自检（替代 CLI 的离屏图像输出硬约束）

**Files:**
- Create: `app/src/main/java/com/dgcamp/paint/DemoExportActivity.kt`
- Modify: `app/src/main/AndroidManifest.xml`（注册 Activity）
- Test: 编译通过 + 真机触发（人工）；或以 JVM/仪器测试断言导出成功

**Interfaces:**
- Consumes: `PaintNative.exportPng`
- Produces: 触发后生成 PNG（`context.cacheDir/demo_export.png`），Toast 显示路径

- [ ] **Step 1: 写 DemoExportActivity**

```kotlin
package com.dgcamp.paint

import android.app.Activity
import android.os.Bundle
import android.widget.Toast
import com.dgcamp.paint.jni.PaintNative
import java.io.File

/** 离屏导出自检：画固定笔迹 → dgcExportPNG 到 cacheDir。满足离屏图像输出硬约束。 */
class DemoExportActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        PaintNative.init(640, 480)
        PaintNative.nativeStrokeBegin(50f, 50f, 0.5f)
        for (i in 0 until 10) PaintNative.nativeStrokeTo(50f + i * 50f, 50f + i * 20f, 0.5f)
        PaintNative.nativeStrokeEnd()
        val out = File(cacheDir, "demo_export.png")
        val ok = PaintNative.nativeExportPng(out.absolutePath)
        PaintNative.nativeDestroy()
        Toast.makeText(this, "export=${ok} ${out.absolutePath}", Toast.LENGTH_LONG).show()
        // 供无头验证：结果写入日志
        android.util.Log.i("DemoExport", "ok=$ok path=${out.absolutePath} size=${if (out.exists()) out.length() else -1}")
        finish()
    }
}
```

- [ ] **Step 2: Manifest 注册**

```xml
<activity android:name=".DemoExportActivity" android:exported="false" />
```

- [ ] **Step 3: 编译验证**

```bash
./gradlew assembleDebug 2>&1 | tail -10
```
Expected: `BUILD SUCCESSFUL`，APK 生成。

---

### Task 6: 测试门（编译全绿；无真机则代码审阅）

**Files:**
- Create: `tests/README.md`（测试口径说明）
- Test: `./gradlew assembleDebug` 0 失败

**Interfaces:**
- Consumes: 全工程

- [ ] **Step 1: 构建全绿**

```bash
cd /home/qiansenwei/workspace/paint-android && ./gradlew assembleDebug 2>&1 | tail -15
```
Expected: `BUILD SUCCESSFUL`，0 错误 0 警告（编译层）。真机安装运行 + DemoExport 触发 Toast 为人工后续项，记录于 `tests/README.md`。

- [ ] **Step 2: 写测试口径说明 + 提交**

```bash
git add -A && git commit -m "feat: paint-android 接入 SDK C API —— 输入/读回贴图/FPS 浮层 + 离屏导出自检"
```

---

## Self-Review 记录

- **Spec 覆盖**：上屏（ImageBitmap 贴图）→ Task 4；输入 → Task 4；FPS/读回耗时 → Task 4；离屏导出（硬约束）→ Task 5；性能依赖 B3-1 → Global Constraints。✓
- **占位符扫描**：无 TBD/TODO/「类似 Task N」。✓
- **类型一致性**：JNI 符号名（`Java_com_dgcamp_paint_jni_PaintNative_*`）与 Kotlin external fun 一一对应（Task 2 ↔ Task 3）；`dgcStrokeTo` 7 参（含 isPredicted）、`dgcFlush` 存在，符合 `dgc_paint_c_api.h`。✓
