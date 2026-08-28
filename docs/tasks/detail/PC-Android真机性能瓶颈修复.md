# 任务书 · PC/Android 真机性能瓶颈修复（线7-性能修复）

> 本任务书对应 `docs/tasks/任务线.md` 任务 **P7-2**，是 P7-1（`bc68617`/`5a0c6ff`，已合并）真机验收未达标后的直接后续修复。P7-1 合并后用户真机实测：Windows(paint-pc) **70fps**（目标 ≥120fps），Android(paint-android) **7fps**（目标 ≥60fps，比此前任一次回归都差）。host ctest（`test_perf_regression`/`test_continuous_input_regression`）全程显示 avgFPS 1800-3000+，完全没有复现问题——说明现有自动化测试的负载模式与真实消费者用法存在系统性差异。

## 背景（只读调查已确认的根因）

- **PC 根因**：`paint-pc/src/app.cpp:226-236` 主循环每帧无条件调用 `dgcReadbackPixels`（关闭 vsync 后为无上限忙循环，调用频率可达数千次/秒）。P7-1 让每次 `dgcReadbackPixels` 调用都对 `Engine::flush_requested_` 做一次非阻塞 `store(true)`（`sdk_api/dgc_paint_c_api.cpp`），而 `Engine::renderLoop()`（`core/engine.cpp`）在每弹出一个 stamp 后就检查一次该标志——readback 高频调用几乎持续把标志置真，逼渲染线程被迫逐 stamp / 极小批量 flush，[[sdk-perf-bottlenecks]] 记录的"批量 composite 168ms→9.5ms"优化收益被打回原形。
- **Android 根因**：`paint-android` 的 `PaintScreen.kt:65-92` 在 `withFrameNanos`（vsync 回调）里，dirty 时先显式调用 `nativeFlush()`——JNI 里对应的是**阻塞版** `dgcFlush()`（`engine->flush()`），再调用 `nativeReadback`。这条路径完全没有用上 P7-1 新增的非阻塞 `requestFlush()`，等于每个 vsync 帧都同步阻塞等渲染线程 drain 完，叠加 Mali 单次 GPU 提交开销更高（[[sdk-perf-bottlenecks]] 记录的 174ms→2.4ms 优化同样依赖攒批），比 PC 更差。
- **共同教训**：P7-1 的回归测试（`test_continuous_input_regression`）只模拟了"固定节奏后台输入 + 间歇性 readback 采样"，从未覆盖"readback 调用频率≈无上限忙循环"或"调用方自己在新非阻塞路径之外又叠加一次阻塞调用"这两种真实消费者用法，因此完全没有拦住这次回归。本任务新增的测试必须补上这个覆盖缺口。

## P7-2 · 目标与产出

**目标**：在不牺牲既有"批量 composite"吞吐收益的前提下，让 PC ≥120fps（无上限）、Android 真机 ≥60fps 真正达标；同时不能重新引入 `d64c2fa` 之前"readback 阻塞渲染线程"或 `f96456e` 之前"孔洞"的老问题。

**这里存在一个必须显式面对的方案冲突风险**：任何"让 flush 更快跟上 readback 节奏"的改法都可能重新打散批量收益（重演 P7-1 本次的问题）；任何"让 flush 更克制/更懒"的改法都可能重新引入孔洞或滞后（重演 `f96456e` 的问题）。**计划阶段必须把这个 tradeoff 明确摆出来并给出具体数值依据；如果找不到两全的方案，必须停下报告人工定夺，不能自行取舍后直接通过评审。**

**产出**

1. SDK 侧（`demo` 仓库）：给"非阻塞置位 `flush_requested_`"加一个节流/去抖机制——例如维护"上次实际 flush 完成的时间戳"，`renderLoop()` 只有在距上次 flush 已过至少 `kMinFlushIntervalMs`（具体数值计划阶段定，需给出帧预算依据，比如不短于 PC 120fps 目标帧时间的合理比例）时才真正响应 `flush_requested_`；未达间隔时不能静默丢弃这次请求——必须保证间隔一到就会被下一次循环检查捕获，且不能引入新的跨线程阻塞等待。同时复核现有 `kMaxBatchStamps`/`kMaxBatchDurationMs` 攒批上限兜底是否与新的节流机制冲突（例如节流窗口是否应该和攒批上限用同一套阈值，还是刻意错开）。
2. 消费端 `paint-android` 仓库：`nativeFlush()` JNI 封装 / `PaintScreen.kt` 绘制路径去掉每帧显式调用阻塞 `dgcFlush()` 再 readback 的逻辑，改为直接调用 `nativeReadback`（内部走 SDK 新的非阻塞 `requestFlush` 路径），使 Android 真正用上 P7-1 的非阻塞机制而不是绕开它。
3. 新增/加强回归测试（`demo` 仓库）：
   - 新增一个模拟"readback 调用频率远高于渲染批次自然形成速度"的回归测试（在紧凑循环里以能达到的最高频率反复调用 `dgcReadbackPixels`，而不是像现有测试那样固定间隔采样），验证批量 composite 的平均批大小/吞吐没有被打回到接近逐条处理的水平（给出具体量化断言，阈值在计划阶段定），且孔洞判据仍与现有 `test_continuous_input_regression` 一致地通过。
   - `dgcReadbackPixels`/`dgcExportPNG` 的头文件注释补充说明："内部已做非阻塞 catch-up，调用方不应在调用本函数前再自行调用阻塞版 `dgcFlush()`，否则会抵消本函数的非阻塞设计目的"，避免消费端重犯 Android 这次的误用。
4. `paint-pc` 侧核查：确认其主循环无节流地调用 `dgcReadbackPixels` 是否在 SDK 侧新增节流机制后已经安全；若判断仍需消费端配合节流，一并列入产出并说明理由；若判断 SDK 侧修复已足够、`paint-pc` 不用改，需在计划里给出理由和验证依据。

**验收**（对应 P7-1 未达标的两项真机指标）

| 序号 | 目标 | 验收方式 |
|---|---|---|
| 1 | PC 真机 ≥120fps（无上限） | Windows 真机人工实测记录（复现本次 70fps 的相同场景/机型）；**此项为硬门槛，不得以"沙箱局限"豁免**——除非新增的高频 readback 回归测试被证明真实复现了同等负载并给出量化通过证据 |
| 2 | Android 真机 ≥60fps | 基准机型（XP-Pen MDP1221, Mali-G57 MC2, 1440×2160）人工实测记录；**同上，硬门槛不豁免** |
| 3 | 不回归孔洞、不回归 PC/Android 此前任一次 fps 崩溃 | 现有 `test_readback_drain`/`test_midstroke_readback`/`test_perf_regression`/`test_continuous_input_regression` 零回归 + 新增高频 readback 回归测试通过 |

**验收共同前提**：host `ctest` 全绿零回归（含新增测试）；`-DDGCPAIN_SANITIZE=ON` ASan/LSan 零泄漏；`android-arm64` preset 仍可编出 `.so`；SDK 所有权/RAII 规范不变；`paint-android` 侧改动需保证其自身编译/单测（若有）不回归。

**依赖理由**：P7-1（本任务是其真机验收未达标后的直接后续修复，复用同一套 flush/readback 机制，问题域完全重叠）。

---

## 评审打「通过」的必要条件

| 任务 | 指标 |
|---|---|
| P7-2 | Windows 真机实测记录 ≥120fps **且** Android 真机实测记录 ≥60fps（人工数据为硬门槛，不接受"沙箱局限"豁免，除非新增回归测试被证明等效复现同等负载）；新增高频 readback 回归测试通过；既有 4 个回归测试零回归；host ctest 全绿；ASan/LSan 零泄漏；`android-arm64` 仍可编出 `.so`；若计划中发现"防孔洞"与"保批量吞吐"两个目标存在两难，必须先停下报告人工定夺，不能自行取舍后直接过评审 |

---

> **设计说明**：本任务横跨 `demo`（SDK）与 `paint-android`（消费者仓库）——参照 D6-1/2/3 先例（调试按钮本体在消费端，作为 SDK 验收宿主纳入任务线），P7-2 同样以 SDK 侧核心修复为主体，消费端改动作为验收闭环的必要组成部分纳入本任务书，而非拆成独立任务线。
> **后续不在本任务书范围**：`paint-pc` 如果最终判定也需要节流改造且与 SDK 侧修复方案强耦合，本任务书产出 4 已覆盖；若判定为独立的、优先级更低的性能改造，另开任务书处理。更激进的架构调整（渲染线程改为固定帧率驱动而非事件驱动）不在本任务书范围。
