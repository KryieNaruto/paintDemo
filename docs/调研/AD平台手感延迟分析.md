# AD 平台手感延迟 · 可行性分析

> 2026-08-30 · 原型机初步搭建完成后的性能问题分析。
> 触发背景：AD（Android）平台真机绘制感觉手感延迟，初步怀疑两条：① async 强制锁定帧率、笔刷路径计算被限 60fps；② modeler 预测似乎作用不大。
> 结论速览：两条怀疑都指向真问题，但**机制都描错了位置**——瓶颈不在 SDK 计算（三线程事件驱动、自由运转），在**消费端读回/上屏链路被 vsync 锁住**。

---

## 0. 关键实测数据（用户提供）

| 指标 | 读数 | 含义 |
|---|---|---|
| Frame | **20ms** | 帧时间 20ms ≈ **50fps**，低于 60Hz 目标，主线程/上屏链路未跟上刷新率 |
| ReadMs | **0.6ms** | `dgcReadbackPixels` 单次读回（非阻塞 catch-up + 纯内存 memcpy）极快 |

ReadMs 0.6ms 直接排除「读回本身慢」；Frame 20ms 说明**上屏链路才是瓶颈**——这印证假设①的现象面，且坐实「SDK 计算远达不到计算上限」不成立（读回都 0.6ms 了，计算更不是瓶颈）。

---

## 1. 数据流全链路（谁在哪被卡）

```
MotionEvent（InputDispatcher 按 vsync 批送）
  → onDrag（UI 主线程）→ nativeStrokeTo
    → dgcStrokeTo（签名无时间参数！t_us=合成固定步长）
      → submitInput → inputLoop ──(predictor 激活时)── 1 输入 → Update 1 真实点 + Predict 3 预测点 = 4 事件
        → input_to_brush_ → brushLoop → strokeTo → brush_to_render_
          → renderLoop → composite(GPU) → 快照缓存（异步刷新，≤4ms 攒批上限）
─────────────────────────── 分界线：以下全在 withFrameNanos（Compose vsync）───────────────────────────
  next vsync tick → dirty? → dgcReadbackPixels（非阻塞 requestFlush + 读快照缓存，0.6ms）
    → copyPixelsFromBuffer(3.1MB, 主线程) → bitmap = bmp → Compose 重组 → drawImage → present(vsync)
```

SDK 三段（输入→笔刷→渲染）是**事件驱动、自由运转**，无任何帧率依赖。真正的帧率锁在分界线以下的**读回 + present**。

---

## 2. 假设①：async 锁定帧率 → 计算被限 60fps？

### 机制判定：不成立 —— SDK 计算从未被锁帧

`core/engine.cpp` 三线程全部无帧率概念：

- **inputLoop**：condvar 等 `pending_input_`，有事件即醒（`engine.cpp:141-194`），无 sleep/节拍。
- **brushLoop**：SPSC 有数据即算（`engine.cpp:197-233`）。
- **renderLoop**：攒批后按三条条件合批 composite——`flush_requested_` / 攒批超 4ms 或 512 stamp / 队列空（`engine.cpp:255-318`）。4ms 上限意味着连续输入下 composite 约 250Hz，远高于 60fps。

「计算达不到上限」不成立：计算上限本来就远超 60fps，且 ReadMs 0.6ms 证明读回也不慢。

### 现象判定：方向对 —— 感知确实被锁在 ~50–60Hz，但锁在消费端显示链路

四个证据：

1. **读回/上屏以 vsync 为节拍**：`PaintScreen.kt:142-173` 帧循环用 `withFrameNanos`（Compose 的 Choreographer 回调），`dirty` 后在下个 vsync tick 才读回。
2. **读回本身还可能滞后一批**：`dgcReadbackPixels`（`sdk_api/dgc_paint_c_api.cpp:385-415`）只做**非阻塞** `requestFlush()`（置一个原子标志）然后读**快照缓存**——缓存只在渲染线程完成一批 composite 后才刷新，快照最多落后一个攒批周期（≤4ms）。
3. **Android 上屏无 vsync 开关**：`MainActivity.kt:12-31` 自认「Compose 上屏由窗口 vsync 驱动，上限=屏幕刷新率」，`requestMaxRefreshRate()` 只能尽力，**突破不了面板刷新率**，低功耗机型默认锁 60Hz。
4. **PC 对照坐实**：`paint-pc/src/app.cpp:334-337` 用 `glfwSwapInterval(0)` 关 vsync 即达 ≥120fps——**同一个 SDK，PC 喂得动，Android 只是显示端被锁**。

### 修正后的根因表述

> 感知延迟 ≈ 1–2 帧（真机 Frame 20ms → 20–40ms），由「vsync 读回等待 + 快照滞后 ≤4ms + present」构成，与 SDK 计算快慢无关。

**Frame 20ms（50fps）是新增的关键证据**：读回只有 0.6ms，却整帧 20ms，说明**主线程每帧还背着 3.1MB `copyPixelsFromBuffer` + `drawImage` 缩放 + 重组**，帧时间撑爆 16.7ms 预算 → 掉帧到 50fps，进一步放大延迟。

---

## 3. 假设②：modeler 预测作用不大？

**成立，且是结构性失效——三个叠加原因：**

### 原因 A（最关键）：时间基准是合成的，不是真实的

`dgcStrokeTo` 的签名**没有时间参数**（`sdk_api/dgc_paint_c_api.h:82`：`x,y,pressure,tiltX,tiltY,isPredicted`），真机 MotionEvent 时间戳被直接丢弃。t_us 由 `FixedTimeStepper` 每点递增固定步长 `default_step = 1e6/min_output_rate_hz`（默认 180Hz → **5555µs/点**，`dgc_paint_c_api.cpp:216-225`）。

后果——SDK 假定输入点间隔恒为 5555µs：

- **真机触摸速率 ≠ 180Hz 时，卡尔曼速度直接失真**。60Hz 输入（真实间隔 16667µs）被当成 5555µs → 速度被高估 ~3x → 预测点**过度外推**，下一真实点 Update 又立即覆盖（`stroke_predictor.cpp:350-353` 覆盖语义）→ 表现为「抢跑 + 回扯」抖动，而不是稳定的「领先一笔」。高刷输入则反过来低估。
- **预测长度与真实延迟脱钩**：Predict 沿合成时间外推 16000µs（`stroke_predictor.cpp:399-422`），它需要覆盖的是 20–40ms 的**真实**滞后。合成时间↔真实时间不成 1:1，预测量既不对标延迟、也不对标真实笔速。

### 原因 B：预测墨迹被 vsync 读回节拍吞掉

就算预测点已合成进快照，也要等下个 `withFrameNanos` tick 才上屏；而且预测被覆盖得很快（每来一个真实点就重推一遍）。预测的「领先」在帧节拍之外根本呈现不出来。

### 原因 C：默认根本没启用

predictor 默认 **passthrough**，只有用户动过 modeler 滑杆（id 4–12）才会经 `dgcSetBrushSetting` 惰性创建注入（`dgc_paint_c_api.cpp:468-478`）。没动过滑杆 → 字面意义的「没有预测」。

---

## 4. ⚠️ 新发现：重绘信号疑点（可能是延迟的最大单一来源）

`PaintScreen.kt:138` `val bmp = remember { Bitmap.createBitmap(...) }` 是**复用单一实例**；帧循环里 `PaintScreen.kt:159` `bitmap = bmp` 写的是**同一个对象引用**。

Compose 的 `mutableStateOf` 默认 `structuralEqualityPolicy`（`==` 比较）：Bitmap 未重写 `equals` → 引用相等 → **同引用写入不触发重组**。即 `bitmap = bmp` 从第二帧起是 no-op，Canvas 的 `drawImage` 不会因像素更新而重绘。

`ReadbackScheduler.kt:61-63` 的 `version()` 注释「同引用 bitmap 强制重绘信号」正是为此设计——但目前**没接进 PaintScreen**。

**必须真机验证**：如果当前笔迹只在 fps 文本（500ms 周期）或缩放/清屏时顺带更新，那么用户感知的「手感延迟」实为「**重绘节拍缺失 → 笔迹滞后刷新**」，这比 vsync 读回等待更严重。P7-3 用**双缓冲（交替新引用）**或 `version()` 强制重绘，一并解决。

---

## 5. 根因排序（AD 平台手感延迟）

| 优先级 | 根因 | 归属 | 对应任务 |
|---|---|---|---|
| 1 | 读回/上屏被 Compose vsync 锁在 ~50–60Hz 感知节拍（Frame 20ms） | 消费端 | P7-3 |
| 1' | **重绘信号疑点**：`bitmap=bmp` 同引用，Compose 可能不重绘 | 消费端 | P7-3 |
| 2 | 主线程每帧背 3.1MB copyPixels + drawImage，帧时间撑爆 → 掉帧 50fps | 消费端 | P7-3 |
| 3 | 快照缓存 + 非阻塞 requestFlush → 读回最多滞后一个攒批周期（≤4ms） | SDK | P7-2 已覆盖（节流） |
| 4 | modeler 预测结构性失效：合成时间基准 + vsync 吞预测 + 默认未启用 | SDK + 消费端 | P7-4 |

---

## 6. 结论

1. **假设①现象成立、机制错误**：不是「async 锁帧把计算限到 60fps」，而是「消费端读回/present 被 vsync 锁住 + 主线程重活掉帧」。修法不是提速 SDK 计算，而是**把读回/上屏从 Compose vsync 解耦 + 减轻主线程每帧像素搬运**（P7-3）。
2. **假设②成立且更严重**：预测在「合成时间 + vsync 读回」架构下**既算不准（时间基准假）、又投不出去（帧节拍锁）**。要让预测真正补偿延迟，需 C API 传真实时间戳 + 显示链路解耦（P7-4）。
3. **新疑点**：`bitmap=bmp` 同引用重绘问题，可能是延迟的最大单一来源，P7-3 首要修复。

---

## 7. 关联文档

- [P7-3 实施计划](../plans/P7-3.md)：AD 读回/上屏 vsync 解耦 + 双缓冲强制重绘
- [P7-4 实施计划](../plans/P7-4.md)：modeler 预测生效（真实时间戳）
- 技术背景 → [`DGCPaint_技术规划.md`](../../DGCPaint_技术规划.md)；性能根因 → `docs/perf/`
