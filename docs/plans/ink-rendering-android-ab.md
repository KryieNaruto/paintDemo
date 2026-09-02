# Jetpack Ink 渲染 A/B 对照 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 paint-android 增加一条 Jetpack Ink（androidx.ink 1.0.0）低延迟渲染路径，与现有 SDK（Vulkan 离屏 → readback → 贴图）路径可在应用内一键切换，量化两路径的延迟差距与手感差异。

**Architecture:** 纯消费端（paint-android）改动。SDK stroke modeler 与 SDK 渲染路径零改动，作为 **Mode A（SDK）** 基线原样保留；新增 **Mode B（ink）** 用 androidx.ink 的 `InProgressStrokes` composable 低延迟上屏（矢量 mesh，无像素 readback）。`PaintScreen` 加一个 `renderMode` boolean 状态在两条路径间切换。SDK 预测代码（`sdk/core/stroke_predictor.*`）一字不动。

**Tech Stack:** Android / Jetpack Compose（compose-bom 2025.01.00）/ androidx.ink 1.0.0（stable，Google Maven）/ 现有 SDK（Vulkan 离屏，submodule `sdk/`，NDK 28.2 arm64-v8a）。

**Spec:** 本计划自包含——brainstorming 结论已并入下方「需求与验收」与「设计」节，无独立 spec 文档（build-pipeline 的 brainstorm→plan 流程）。

## Global Constraints

- **androidx.ink 版本钉 `1.0.0` stable**（2025-12-17 GA），禁止 rc/alpha。
- **只改 paint-android**；`demo` 仓库与 `sdk/` submodule 零 diff（仅 Task 4 复用现有 `dgc_cli` 做 host 基线，不改其代码）。
- **SDK stroke modeler 零改动零删除**：`sdk/core/stroke_predictor.{h,cpp}` 保持原样；Mode A 继续用它，Mode B 用 ink 内置 modeler（同算法，公平）。
- **主交付 = 应用内开关切换**；独立 APK（buildConfig 锁死 ink）仅作退路，非主路。
- **评估维度 = 延迟 + 手感**，不评估画笔效果、不评估预测保真。
- **build-pipeline 硬约束**：必须有 CLI（headless 脚本化）+ 离屏渲染输出图像（PNG）。SDK 侧由现有 `dgc_cli` + `dgcExportPNG` 承担；ink 侧由 Task 4 的 on-device 离屏 bitmap → PNG 承担（ink 渲染为 Android-only，无 host JVM 像素渲染，见 Task 4 说明）。
- paint-android 当前 `minSdk = 26`；Task 1 必须核实 androidx.ink 1.0.0 的实际 minSdk，若不兼容则在本任务内定夺（bump minSdk 或按 API 门控 ink 模式）。

---

## 需求与验收标准

| # | 目标 | 验收方式 |
|---|---|---|
| R1 | ink 模式能画 | 真机切到 Mode B，手指/笔画出可见笔画（`InProgressStrokes` 实时渲染） |
| R2 | 两模式可快速切换 | `PaintScreen` 顶部开关一次点击在 SDK / ink 间切换，状态即切即生效 |
| R3 | SDK 模式零回归 | Mode A 下既有链路（输入→JNI→SDK→readback→贴图 + 调试面板）行为不变；`git diff` 确认 `sdk/` 零改动 |
| R4 | 延迟量化 | 两模式各自记录帧时 p50/p99 + 输入到帧延迟代理 + SDK readMs，产出可比数字 |
| R5 | 离屏输出图像（硬约束） | host：`dgc_cli` 离屏 → PNG（SDK 基线）；ink：on-device 离屏 bitmap → PNG |
| R6 | 手感对照 | 同一手势两模式各画（快速笔画/圈/折线），人工记录「领先/滞后/抖动」感受 |

## 设计

### 两模式数据流

```
Mode A（SDK，基线，不动）
  pointer → detectDragGestures → PaintNative.nativeStrokeTo
    → SDK stroke modeler + Vulkan 离屏 → readback（readMs）→ Bitmap → Canvas.drawImage

Mode B（ink，新增）
  pointer → InProgressStrokes composable（自处理触控输入）
    → ink modeler → ink mesh → 低延迟上屏（无 readback）
```

### 切换状态

```kotlin
enum class RenderMode { SDK, INK }
var renderMode by remember { mutableStateOf(RenderMode.SDK) }
```

顶部一个 `Text` 开关（复用现有右上角「⚙ 参数」按钮同款样式），点击在 `SDK` / `INK` 间翻转。

### 测量（R4/R6）

- **帧时**：`FrameTimeAccumulator`（纯 Kotlin，TDD）聚合每帧耗时，出 p50/p99。
- **输入到帧延迟代理**：`LatencyProbe`（纯 Kotlin，TDD）记 `onInput(t)` / `onFramePresented(t)`，出滑动平均 lag。SDK 模式 `onFramePresented` 在 readback+blit 后触发；ink 模式在 ink 帧回调后触发。
- **SDK readMs**：已有（`PaintScreen` 的 `readMs`），ink 模式无此成本，作为对照证据。
- **手感**：真机人工（R6）。

---

## 文件结构

| 文件 | 责任 |
|---|---|
| `app/build.gradle.kts`（改） | 加 androidx.ink 1.0.0 依赖（钉稳定版） |
| `app/src/main/java/com/dgcamp/paint/ui/InkStrokeCanvas.kt`（新） | ink 渲染路径：`InProgressStrokes` + `StockBrushes`，暴露 `onStrokesFinished` |
| `app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`（改） | 加 `RenderMode` 状态 + 切换开关 + 分支；接延迟测量 |
| `app/src/main/java/com/dgcamp/paint/ui/LatencyMetrics.kt`（新） | `FrameTimeAccumulator` + `LatencyProbe`（纯 Kotlin，可单测） |
| `app/src/test/java/com/dgcamp/paint/ui/LatencyMetricsTest.kt`（新） | 测量逻辑单测 |
| `app/src/main/java/com/dgcamp/paint/ui/InkPngExporter.kt`（新） | ink 离屏 bitmap → PNG（Task 4 硬约束） |

> 所有路径均相对 `/home/qiansenwei/workspace/paint-android`（消费仓库根）。

---

## Task 1: 引入 androidx.ink 1.0.0 依赖 + minSdk 核实

**Files:**
- Modify: `app/build.gradle.kts`

**Interfaces:**
- Consumes: 无（前置）
- Produces: 编译期可解析 `androidx.ink.*` 符号；确认 minSdk 兼容性结论

- [ ] **Step 1: 在 `app/build.gradle.kts` 的 `dependencies` 块追加 ink 依赖（钉稳定版）**

在现有 `dependencies { ... }` 块末尾（`testImplementation("junit:junit:4.13.2")` 之后）追加：

```kotlin
    // Jetpack Ink：Mode B（低延迟渲染）依赖，钉稳定版 1.0.0（不用 rc/alpha，见计划 Global Constraints）
    val inkVersion = "1.0.0"
    implementation("androidx.ink:ink-nativeloader:$inkVersion")
    implementation("androidx.ink:ink-rendering:$inkVersion")
    implementation("androidx.ink:ink-strokes:$inkVersion")
    implementation("androidx.ink:ink-authoring-compose:$inkVersion")
    implementation("androidx.ink:ink-brush-compose:$inkVersion")
    implementation("androidx.ink:ink-geometry-compose:$inkVersion")
    implementation("androidx.ink:ink-storage:$inkVersion")
```

- [ ] **Step 2: 同步 + 构建，确认依赖解析**

Run: `cd /home/qiansenwei/workspace/paint-android && ./gradlew :app:assembleDebug`
Expected: BUILD SUCCESSFUL；若报「找不到 androidx.ink:ink-*」则先 `./gradlew --refresh-dependencies` 重试。

- [ ] **Step 3: 核实 ink 1.0.0 实际 minSdk，与本项目 `minSdk=26` 对齐**

Run: 解包任一 ink AAR 的 `AndroidManifest.xml` 看 `minSdkVersion`，例如
`unzip -p ~/.gradle/caches/modules-2/files-2.1/androidx.ink/ink-authoring-compose/1.0.0/*/ink-authoring-compose-1.0.0.aar AndroidManifest.xml`
Expected: 记录 ink 的 minSdk 值。
- 若 `<= 26`：无需改动，本任务通过。
- 若 `> 26`（如 API 34）：在 `app/build.gradle.kts` 的 `defaultConfig` 中 `minSdk = max(26, inkMinSdk)`，并在 `PaintScreen` 的 ink 分支加 `Build.VERSION.SDK_INT >= inkMinSdk` 门控（低于该版本显示「本机不支持 ink 模式」占位）；记录到对比报告（Task 5）作为「ink 上屏需 API X+」的已知约束。

- [ ] **Step 4: 提交**

```bash
cd /home/qiansenwei/workspace/paint-android
git add app/build.gradle.kts
git commit -m "build(android): 引入 androidx.ink 1.0.0（Mode B 低延迟渲染依赖）"
```

---

## Task 2: 模式开关 + ink 渲染路径（能画）

**Files:**
- Create: `app/src/main/java/com/dgcamp/paint/ui/InkStrokeCanvas.kt`
- Modify: `app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`

**Interfaces:**
- Consumes: Task 1 的 ink 依赖；`androidx.ink.authoring.compose.InProgressStrokes`、`androidx.ink.brush.StockBrushes`、`androidx.ink.strokes.Stroke`
- Produces: `InkStrokeCanvas(onStrokesFinished: (List<Stroke>) -> Unit)` composable；`RenderMode` 枚举 + `PaintScreen` 分支

- [ ] **Step 1: 核实 `InProgressStrokes` / `StockBrushes` 的确切签名（唯一外部 API 面，需照 1.0.0 参考文档确认）**

读 `https://developer.android.com/reference/kotlin/androidx/ink/authoring/compose/package-summary#InProgressStrokes(...)` 与 `androidx.ink.brush` 的 `StockBrushes`。确认三件事后据此微调 Step 2 代码：
1. `InProgressStrokes` 的必需参数（至少 `brush: Brush`）与 `onStrokesFinished` 回调形参类型（`(List<Stroke>) -> Unit` 还是别的）；
2. `StockBrushes` 里钢笔类静态刷的构造名（如 `pen()` / `ballpoint()`），选一个默认；
3. 有无 `modifier` 参数（无则用 `Box` 包裹）。

- [ ] **Step 2: 新建 `InkStrokeCanvas.kt`**

```kotlin
package com.dgcamp.paint.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.ink.authoring.compose.InProgressStrokes
import androidx.ink.brush.StockBrushes
import androidx.ink.strokes.Stroke

/**
 * Mode B（ink）渲染路径：androidx.ink 的 InProgressStrokes 自处理触控输入并低延迟上屏
 * （矢量 mesh，无像素 readback）。完成后经 onStrokesFinished 回调交出已定型笔画。
 */
@Composable
fun InkStrokeCanvas(
    modifier: Modifier = Modifier,
    onStrokesFinished: (List<Stroke>) -> Unit = {},
) {
    // 默认刷：StockBrushes 钢笔类（Step 1 确认具体名，此处以 pen() 示意）。
    val brush = remember { StockBrushes.pen() }
    Box(modifier = modifier.fillMaxSize()) {
        InProgressStrokes(
            brush = brush,
            onStrokesFinished = { strokes -> onStrokesFinished(strokes) },
        )
    }
}
```

- [ ] **Step 3: `PaintScreen` 加 `RenderMode` 状态 + 顶部切换开关**

在 `PaintScreen.kt` 文件**顶层**（`PaintScreen` 函数之外，紧邻 `BRUSH_SETTINGS` 声明之后）加枚举：

```kotlin
/** Mode A（SDK）vs Mode B（ink）切换：A/B 对照延迟/手感的入口（本计划核心）。 */
internal enum class RenderMode { SDK, INK }
```

在 `PaintScreen()` 函数体顶部（`var panelExpanded` 附近）加状态：

```kotlin
    var renderMode by remember { mutableStateOf(RenderMode.SDK) }
```

在 `Box` 内容里、右上角「⚙ 参数」按钮**上方**加一个同款开关（放在 `Text` 叠加层，`align(Alignment.TopEnd)` 让位）：

```kotlin
            Text(
                text = if (renderMode == RenderMode.SDK) "Mode: SDK ▸" else "Mode: ink ▸",
                color = Color.White,
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .safeDrawingPadding()
                    .padding(12.dp, top = 40.dp)   // 让出「⚙ 参数」按钮
                    .background(Color(0xAA007ACC), RoundedCornerShape(8.dp))
                    .clickable { renderMode = if (renderMode == RenderMode.SDK) RenderMode.INK else RenderMode.SDK }
                    .padding(horizontal = 10.dp, vertical = 4.dp),
            )
```

- [ ] **Step 4: 画布层按 `renderMode` 分支——INK 走 ink，SDK 走原路径**

把现有 `Box` 内 `if (bmp != null) { Canvas(...) }` 块包裹成条件；在 `renderMode == RenderMode.INK` 时改渲染 `InkStrokeCanvas`，否则维持原 `Canvas.drawImage`。实现：在现有 `if (bmp != null) { Canvas(...) }` 之前插入：

```kotlin
            if (renderMode == RenderMode.INK) {
                InkStrokeCanvas(
                    modifier = Modifier.fillMaxSize(),
                    onStrokesFinished = { /* Task 3 接 LatencyProbe；暂空 */ },
                )
            }
```

并把原 `if (bmp != null) { Canvas(...) }` 改为 `if (renderMode == RenderMode.SDK && bmp != null) { Canvas(...) }`（指针输入 `pointerInput` 保持挂在整个 `Box` 上不动——ink 模式由 `InProgressStrokes` 自建输入层覆盖处理，SDK 模式继续用 `detectDragGestures`；两者输入源互不干扰，切模式即切换输入归属）。

- [ ] **Step 5: 构建 + 安装 + ink 模式能画（R1/R2）**

Run: `cd /home/qiansenwei/workspace/paint-android && ./gradlew :app:assembleDebug`
真机/模拟器安装后：点「Mode: SDK ▸」切到 ink，手指画一笔 → 可见笔画实时出现；切回 SDK → 原链路正常。
Expected: R1（ink 能画）+ R2（即切即生效）通过。

- [ ] **Step 6: 提交**

```bash
cd /home/qiansenwei/workspace/paint-android
git add app/src/main/java/com/dgcamp/paint/ui/InkStrokeCanvas.kt app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt
git commit -m "feat(android): 加 RenderMode 开关 + ink InProgressStrokes 渲染路径"
```

---

## Task 3: 延迟/手感测量埋点（TDD）

**Files:**
- Create: `app/src/main/java/com/dgcamp/paint/ui/LatencyMetrics.kt`
- Test: `app/src/test/java/com/dgcamp/paint/ui/LatencyMetricsTest.kt`
- Modify: `app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`

**Interfaces:**
- Consumes: Task 2 的 `RenderMode` 分支
- Produces: `FrameTimeAccumulator`（`record(ms)` → `p50()`/`p99()`）；`LatencyProbe`（`onInput(ns)`/`onFramePresented(ns)` → `avgLagMs()`）

- [ ] **Step 1: 写失败测试 `LatencyMetricsTest.kt`**

```kotlin
package com.dgcamp.paint.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LatencyMetricsTest {
    @Test
    fun frameTimeAccumulator_p50_p99() {
        val acc = FrameTimeAccumulator()
        // 喂 100 个 1..100 的帧时
        for (i in 1..100) acc.record(i.toFloat())
        assertEquals(50.0f, acc.p50(), 0.5f)   // 中位数 50~51
        assertEquals(99.0f, acc.p99(), 0.5f)   // p99 ≈ 99
    }

    @Test
    fun frameTimeAccumulator_empty_returnsZero() {
        val acc = FrameTimeAccumulator()
        assertEquals(0f, acc.p50(), 0f)
        assertEquals(0f, acc.p99(), 0f)
    }

    @Test
    fun latencyProbe_avgLagMs() {
        val probe = LatencyProbe()
        probe.onInput(0L)
        probe.onFramePresented(16_000_000L)     // 16ms 后上屏
        probe.onInput(20_000_000L)
        probe.onFramePresented(36_000_000L)
        assertEquals(16.0f, probe.avgLagMs(), 0.5f)
    }

    @Test
    fun latencyProbe_ignoresStrayPresent() {
        val probe = LatencyProbe()
        probe.onFramePresented(10_000_000L)     // 无输入先上屏 → 忽略
        assertEquals(0f, probe.avgLagMs(), 0f)
    }
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /home/qiansenwei/workspace/paint-android && ./gradlew :app:testDebugUnitTest --tests "com.dgcamp.paint.ui.LatencyMetricsTest"`
Expected: 编译失败（`FrameTimeAccumulator`/`LatencyProbe` 未定义）。

- [ ] **Step 3: 实现 `LatencyMetrics.kt`**

```kotlin
package com.dgcamp.paint.ui

/** 帧时累加器：聚合每帧耗时，出 p50/p99（毫秒）。 */
class FrameTimeAccumulator(private val capacity: Int = 240) {
    private val samples = ArrayList<Float>(capacity)
    fun record(frameMs: Float) {
        if (samples.size == capacity) samples.removeAt(0)
        samples.add(frameMs)
    }
    fun p50(): Float = percentile(0.5f)
    fun p99(): Float = percentile(0.99f)
    private fun percentile(q: Float): Float {
        if (samples.isEmpty()) return 0f
        val sorted = samples.sorted()
        val idx = ((sorted.size - 1) * q).toInt().coerceIn(0, sorted.size - 1)
        return sorted[idx]
    }
    fun clear() = samples.clear()
}

/** 输入到帧延迟代理：记「输入时间」与「该输入上屏时间」的滑动平均差（毫秒）。 */
class LatencyProbe {
    private var pendingInputNs: Long? = null
    private var lagSumNs = 0L
    private var lagCount = 0
    fun onInput(nowNs: Long) { pendingInputNs = nowNs }
    fun onFramePresented(nowNs: Long) {
        val t = pendingInputNs ?: return      // 无待测输入的上屏忽略
        lagSumNs += nowNs - t
        lagCount++
        pendingInputNs = null
    }
    fun avgLagMs(): Float =
        if (lagCount == 0) 0f else (lagSumNs / lagCount) / 1_000_000f
    fun reset() { pendingInputNs = null; lagSumNs = 0; lagCount = 0 }
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /home/qiansenwei/workspace/paint-android && ./gradlew :app:testDebugUnitTest --tests "com.dgcamp.paint.ui.LatencyMetricsTest"`
Expected: 4 用例全绿。

- [ ] **Step 5: 接入 `PaintScreen` 两模式**

在 `PaintScreen()` 加：

```kotlin
    val frameAcc = remember { FrameTimeAccumulator() }
    val lagProbe = remember { LatencyProbe() }
```

SDK 模式：在 `detectDragGestures` 的 `onDragStart`/`onDrag` 里加 `lagProbe.onInput(System.nanoTime())`；在 readback worker 成功路径（`dirty=false` 之后）加 `lagProbe.onFramePresented(System.nanoTime())`；在 vsync 循环里把每帧耗时 `(now - lastFrameNow)` 记入 `frameAcc.record(...)`。
ink 模式：在 `InkStrokeCanvas` 外包一层 `pointerInput` 无法直接拿（`InProgressStrokes` 内部吞输入），故 ink 模式只记**帧时**（vsync 循环已覆盖），延迟代理在 ink 模式标注为「不适用（无 readback）」，HUD 里 ink 模式显示 `readMs = 0`。

- [ ] **Step 6: HUD 展示两模式测量**

把现有 HUD `Text`（`FPS/Frame/Readback`）改为按模式输出：SDK 模式显示 `FPS / Frame(p50/p99) / Readback`；ink 模式显示 `FPS / Frame(p50/p99) / Readback: n/a(无readback)`。

- [ ] **Step 7: 提交**

```bash
cd /home/qiansenwei/workspace/paint-android
git add app/src/main/java/com/dgcamp/paint/ui/LatencyMetrics.kt app/src/test/java/com/dgcamp/paint/ui/LatencyMetricsTest.kt app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt
git commit -m "feat(android): 加帧时 p50/p99 + 输入到帧延迟代理测量（TDD）"
```

---

## Task 4: 离屏输出图像（build-pipeline 硬约束）

**Files:**
- Create: `app/src/main/java/com/dgcamp/paint/ui/InkPngExporter.kt`
- Modify: `app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`（ink 模式加导出按钮）

**Interfaces:**
- Consumes: Task 2 的 ink 渲染 + `androidx.ink.rendering.android.canvas.CanvasStrokeRenderer`、`androidx.ink.strokes.Stroke`
- Produces: `InkPngExporter.export(strokes, w, h, path): Boolean`；SDK 基线复用 `dgc_cli`

> **硬约束两段落地**：(1) SDK 基线——host 上 `dgc_cli`（`demo/build/host-linux/cli/dgc_cli`，已存在）跑同一脚本离屏渲染 → PNG，作为 Mode A 的执行图像；(2) ink 侧——androidx.ink 渲染为 **Android-only**（`CanvasStrokeRenderer` 依赖 `android.graphics.Canvas`，无 host JVM 像素渲染），故离屏输出用 **on-device bitmap → PNG**（`CanvasStrokeRenderer.draw` 到 `Bitmap` 背的 `Canvas` → `compress(PNG)`）。两者均产出可落盘的执行图像，满足「离屏渲染输出图像」；CLI/脚本化由 `./gradlew` 与 `dgc_cli` 承担。

- [ ] **Step 1: 核实 `CanvasStrokeRenderer.create/draw` 签名（同 Task 2 Step 1 参考）**

读 `https://developer.android.com/reference/kotlin/androidx/ink/rendering/android/canvas/CanvasStrokeRenderer`：确认 `create(TextureBitmapStore)` 与 `draw(Canvas, InProgressStroke|Stroke, AffineTransform)` 的形参，据以微调 Step 2。

- [ ] **Step 2: 新建 `InkPngExporter.kt`**

```kotlin
package com.dgcamp.paint.ui

import android.graphics.Bitmap
import android.graphics.Canvas
import androidx.ink.geometry.AffineTransform
import androidx.ink.rendering.android.canvas.CanvasStrokeRenderer
import androidx.ink.strokes.Stroke
import java.io.File
import java.io.FileOutputStream

/** Mode B（ink）离屏输出：把已完成笔画经 CanvasStrokeRenderer 画进 Bitmap → PNG。 */
object InkPngExporter {
    fun export(strokes: List<Stroke>, w: Int, h: Int, path: String): Boolean {
        return try {
            val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bmp)
            canvas.drawColor(0xFFF5F2E8.toInt())
            val renderer = CanvasStrokeRenderer.create(/* textureBitmapStore */ null)
            strokes.forEach { s ->
                renderer.draw(canvas, s, AffineTransform.identity())
            }
            FileOutputStream(File(path)).use { out ->
                bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
            }
            true
        } catch (t: Throwable) {
            false
        }
    }
}
```

> 注：`create(...)` 是否接受 `null` 的 `TextureBitmapStore`、`AffineTransform.identity()` 的确切构造，属 Step 1 需照 1.0.0 参考核实的点；若 `create` 必填 texture store，则先 `TextureBitmapStore()` 构造再传入。

- [ ] **Step 3: ink 模式加「导出 PNG」按钮 + 收集已完成笔画**

在 `InkStrokeCanvas` 的 `onStrokesFinished` 回调里把笔画累积进 `var finishedStrokes by remember { mutableStateOf(listOf<Stroke>()) }`；ink 模式 HUD 加一个「导出 PNG」按钮，点击调 `InkPngExporter.export(finishedStrokes, cw, ch, context.filesDir.path + "/ink_snapshot.png")`，成功后 HUD 显示路径。

- [ ] **Step 4: host 侧跑 SDK 基线离屏 → PNG（复用，不改代码）**

Run: `cd /home/qiansenwei/workspace/demo && ./build/host-linux/cli/dgc_cli <demo 脚本> --export-png /tmp/sdk_baseline.png`
Expected: 产出 `/tmp/sdk_baseline.png`（若 `dgc_cli` 不在该路径，用 `cmake --build build/host-linux` 先建出）。此步骤不改 SDK，仅复用作 Mode A 执行图像。

- [ ] **Step 5: 提交**

```bash
cd /home/qiansenwei/workspace/paint-android
git add app/src/main/java/com/dgcamp/paint/ui/InkPngExporter.kt app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt
git commit -m "feat(android): ink 离屏导出 PNG（build-pipeline 硬约束）"
```

---

## Task 5: 真机 A/B 验收 + 对比报告

**Files:**
- Create: `docs/ink-ab-comparison.md`（对比报告，落 paint-android 仓库）

**Interfaces:**
- Consumes: Task 2/3/4 全部产物
- Produces: 延迟对比表 + 手感结论 + 已知约束

- [ ] **Step 1: 真机两种模式各采一组数据**

同一设备（记录型号/刷新率）：
1. Mode A（SDK）：快速连续画 20 秒，记录 HUD 的 Frame p50/p99 + Readback + avgLag。
2. 切 Mode B（ink）：同样手势画 20 秒，记录 Frame p50/p99 + avgLag（Readback=n/a）。
3. 各重复 3 轮，取中位。

- [ ] **Step 2: 手感对照（R6）**

同一手势（快速直线 / 画圈 / 折线 / 快速点画），两模式各画，记录：笔迹是否领先/滞后、有无抖动/抢跑、跟手度。对照 SDK 模式的 readback 可见滞后与 ink 模式的无 readback。

- [ ] **Step 3: 写 `docs/ink-ab-comparison.md`**

包含：设备信息、两模式延迟数字对比表（Frame p50/p99、readMs、avgLag）、手感结论、`minSdk` 约束（Task 1 Step 3 结论）、「是否有延迟差距 + 是否值得换」的结论与建议。

- [ ] **Step 4: 提交**

```bash
cd /home/qiansenwei/workspace/paint-android
git add docs/ink-ab-comparison.md
git commit -m "docs(android): ink vs SDK 延迟/手感 A/B 对比报告"
```

---

## 风险与已知约束

1. **androidx.ink 是刚 GA 的库**（1.0.0，2025-12-17）：`InProgressStrokes`/`CanvasStrokeRenderer`/`StockBrushes` 的确切签名以 1.0.0 参考文档为准——Task 2 Step 1、Task 4 Step 1 已设「照参考核实再落码」步骤，属显式验证而非占位。
2. **minSdk 兼容**：ink 若要求 API 34+（`android.graphics.Mesh` 是 API 34 引入），paint-android 的 `minSdk=26` 需 bump 或按 API 门控。Task 1 Step 3 核实并记录。
3. **延迟代理精度**：`LatencyProbe` 是「输入→帧」代理，非严格 input-to-photon（后者需 Perfetto/高速相机）。报告如实标注其为代理值 + 手感人工为准。
4. **ink 模式输入归属**：`InProgressStrokes` 自建输入层，与外层 `detectDragGestures` 并存时需确认不会双重消费（Task 2 Step 4 已让 ink 模式不走 `Canvas.drawImage` 分支；若真机出现双击/双笔，需在 ink 分支外给 `pointerInput` 加 `if (renderMode == SDK)` 短路）。
5. **独立 APK 退路**：若真机验证希望两模式并列安装对比，可用 `buildConfigField("String","DEFAULT_RENDER_MODE", ...)` 把初始 `renderMode` 锁死为 ink 出第二个 APK——但**主路仍是应用内开关**，此退路仅在需要时启用。

## 非目标（明确不做）

- 不改 `demo` / `sdk/` submodule 任何代码（含 `core/stroke_predictor.*`、`render/*`、`kernels/*`）。
- 不评估画笔效果、不评估预测保真（SDK stroke modeler 与 ink modeler 同算法，已确认等价）。
- 不把 ink 打进 SDK 作新 `IRenderBackend`（延迟收益在消费端 mesh 上屏，不在 SDK 离屏架构内，见 brainstorming 结论）。
- 不做预测自校准/自适应、不做 SurfaceView/GL 上屏重构。
