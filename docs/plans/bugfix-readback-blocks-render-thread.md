# 修复计划 · drain-before-readback 阻塞渲染线程导致快速甩笔掉帧（PC 20fps 回退）

> 对应上一轮修复 `d64c2fa`（`docs/plans/bugfix-readback-drain.md`）引入的新回归：修复空洞后，
> paint-pc 快速甩笔时帧率跌回 20fps。用户反馈：「为什么你修复完反而将之前的优化回退了？？现在只有20fps！」

## ① 根因（已复现，含证据）

**现象**：`d64c2fa` 让 `dgcReadbackPixels` 在拷贝画布前先 `engine->flush()`（drain 屏障）。
三线程异步架构（输入 → 笔刷 → 渲染）的设计意义就是渲染线程的 composite 耗时不阻塞调用方；
drain 把这层解耦堵死了——GUI 主循环每帧调 `dgcReadbackPixels` 时，现在必须**同步等渲染线程
把当前积压的 dab 全部 composite 完**才能拿到画布。快速甩笔（每帧鼠标位移大）时一批要
composite 几十个 dab，composite 本身就要几到十几毫秒，直接体现成掉帧。

**证据**（`tests/test_perf_regression.cpp`，host lavapipe 无头复现，模拟每帧 40~80px 位移的
快速甩笔 + 每帧读回，300 帧）：
```
avg=9.06ms(110fps)  p95=34.9ms  max=103.7ms(9.6fps)
under30fps=15/300   under20fps=8/300
```
即便在这台机器的**纯软件渲染**（llvmpipe，无真实 GPU）上，仍有 5%+ 的帧跌破 20fps；真实
GPU 驱动下 vkQueueSubmit/fence 的调度开销、与 OpenGL 显示层的驱动争用只会更明显——与用户
反馈的「现在只有 20fps」吻合。

**根因定位**（对照 `core/engine.cpp` `Engine::flush()` / `renderLoop()`）：`flush()` 是纯轮询
屏障，会等 `composited_ == submitted_`；渲染线程的「批量 composite」优化（把多个输入点的
stamp 攒批一次提交，避免每点一次 SubmitAndWait）原意是**只在 flush 请求或队列空闲时才提交
一次**，让 GUI 线程不必等这个提交——但 `d64c2fa` 让每次外部 `dgcReadbackPixels`（GUI 主循环
每帧必调）都变成一次 flush 请求，等于把「批量 composite 只需异步跑」的解耦点堵成了「GUI 线程
每帧同步等 composite」，两个优化（批量 composite 减少提交次数、readback 与渲染线程解耦）互相
抵消，回退到了原 20fps 问题的同一种阻塞模式（渲染线程慢 → 调用方等）。

**影响面**：`dgcReadbackPixels`（消费端高频路径，paint-pc 每帧调）。`dgcExportPNG` 不受影响
（本就是低频操作，阻塞可接受，且需要「最终态」而非快照，本次不改）。

## ② 修复方案（不回退、主路径——"快照缓存"，正确性与性能都要）

**核心思路**：让 readback 永远不等渲染线程，同时永远不暴露"半个 dab"的中间态——渲染线程
自己在每次 composite 完成后，把画布**异步发布**成一份完整快照（CPU 侧 cache），GUI 线程的
`dgcReadbackPixels` 只从这份 cache 做一次内存拷贝，不碰 GPU、不等锁竞争、不等 composite。

**改 `render/vulkan/vk_backend.cpp`（`VkBackend` / `VkBackend::Impl`）**：

1. 新增 `std::vector<std::uint8_t> cache_` + 独立的 `std::mutex cache_mutex_`（与现有守护
   GPU 命令提交的 `mutex_` **分开**，避免 GUI 线程的 cache 读取被渲染线程的 GPU 提交阻塞）。
2. `Impl` 新增 `RefreshReadbackCacheLocked()`：复用 `ReadbackLocked` 里现成的
   `vkCmdCopyImageToBuffer + SubmitAndWait + invalidate` 逻辑，把结果写进一块内部
   host-visible 缓冲，再在 `cache_mutex_` 保护下 `memcpy` 进 `cache_`（`cache_mutex_` 的
   临界区只有一次 memcpy，微秒级，不做任何 GPU 等待）。
3. `CompositeLocked` 尾部、`ClearCanvasLocked` 尾部、`initOffscreen` 创建画布后，各调一次
   `RefreshReadbackCacheLocked()`——三者均已在 `mutex_` 保护下串行执行（现状不变，未引入新
   竞争），只是多做一次"顺手"的画布快照发布，成本由渲染线程自己承担，不传导给 GUI 线程。
4. `VkBackend::readback(void* rgbaOut)` **不再**加 `mutex_`、**不再**调 `ReadbackLocked`：
   直接 `std::lock_guard<std::mutex> lock(cache_mutex_); std::memcpy(rgbaOut, cache_.data(),
   cache_.size());`。彻底与渲染线程的 GPU 提交解耦。
5. `exportPNG` 不变：仍走 `mutex_` + `ReadbackLocked`（低频、需要真正的最终态，阻塞可接受）。

**改 `sdk_api/dgc_paint_c_api.cpp`**：

1. `dgcReadbackPixels`：**去掉** `d64c2fa` 加的 `engine->flush()`（不再需要——`backend->
   readback()` 本身已经是"永远完整、永远不等"的快照读取）。
2. `dgcExportPNG`：**保留** `d64c2fa` 加的 drain（不变——导出仍需要真正最终态）。

**正确性论证（为什么快照方案不会再引入空洞）**：cache 只在 `CompositeLocked`/
`ClearCanvasLocked` 整批提交并 `SubmitAndWait` 完成**之后**才刷新——`dgcReadbackPixels`
读到的永远是"某次完整 composite 批之后"的画布，不可能读到"提交到一半"的中间态。代价是
可能比最新输入落后一批（渲染线程排队中的下一批还没提交），但**落后 ≠ 缺失**：下一次
`dgcReadbackPixels` 调用时，只要渲染线程已经把那批 composite 完并刷新过 cache，就能读到；
不会像 `d64c2fa` 之前那样永久跳过某些 dab。

**性能论证**：`dgcReadbackPixels` 退化为一次 mutex + memcpy（1920×1080 RGBA ≈ 8.3MB，
本机实测 memcpy 本身 ~0.7ms），彻底不含 GPU 命令提交/等待，不再随渲染线程 composite 批
大小波动。

## ③ 回归用例（先红后绿）

**沿用现有正确性回归**（验证快照方案仍然「无空洞」）：
- `tests/test_readback_drain.cpp`：笔画结束后不 flush 直接读回，缺 < 5%。
- `tests/test_midstroke_readback.cpp`：笔画进行中逐帧读回（无 flush），单调不减、末帧缺 < 5%。

两者当前基于 `d64c2fa` 的 drain 机制通过；重构后必须**仍然通过**（验证快照方案没有引入新的
空洞回归）——这是这次修复"不能顾此失彼"的硬约束。

**新增性能回归 `tests/test_perf_regression.cpp`**（已作为诊断探针写好，转正为 ctest 断言）：

- 模拟快速甩笔（每帧鼠标位移 40~80px）+ 每帧读回 300 帧，断言**无单帧超过 20ms**（对应
  50fps 下限，留出比 20fps 门槛更宽的安全边际；此为纯 memcpy 的 cache 读取路径，不该受
  composite 批大小影响）。
- **红（当前 `d64c2fa` 版本）**：已实测 max=41.8~103.7ms，`under20fps=8/300`，断言必然失败。
- **绿（快照方案后）**：`dgcReadbackPixels` 不再触发/等待 composite，预期单帧稳定在
  memcpy 量级（<5ms），断言通过。

```cpp
// 核心断言（转正后）
CHECK(maxFrameMs < 20.0, "no single frame exceeds 20ms under fast-stroke load");
```

## ④ 影响面核对

- `readback()` 语义再次变更：从「drain 后完整快照」→「渲染线程主动发布的最近一次完整快照」。
  仍然「永远完整（不含半个 dab）」，只是不再保证是"提交调用瞬间"的最新态，而是"最近一次
  composite 批完成"时的状态——对连续读回的 GUI 主循环无感（下一帧自然追上）。
- `dgcExportPNG` 行为不变（仍 flush 后走 GPU 权威读回）。
- 确定性（B5-3 golden）：`test_determinism` 用的是 `dgcExportPNG`（flush 路径不变），不受
  影响，必须全绿。
- 所有权/泄漏（B1-8）：新增 `cache_`/`cache_mutex_` 是 `VkBackend`/`Impl` 内部值成员，随
  对象生命周期 RAII 析构，无裸指针、无新增所有权转移。
- CLI/headless 现有调用方（`dgc_cli`、`test_offscreen`、`test_brush_offscreen` 等）：均在
  `dgcFlush`/`dgcEndStroke` 之后才读回或导出，语义上早已期望"最终态"，快照方案下这些调用
  时渲染线程早已把最后一批 composite 完并刷新过 cache，行为不变。

## ⑤ 验证方式（无头）

- `cmake --build build/host-linux` 后跑 `test_readback_drain`、`test_midstroke_readback`
  （正确性不回归）+ `test_perf_regression`（新增性能断言，先红后绿记录）。
- 全量 `ctest`（host-linux）0 失败 0 跳过，含 `test_determinism` golden。
- `android-arm64` preset 仍可编出 `.so`（改动限于 `render/vulkan` + `sdk_api`，无平台特定
  API，两端共用同一份 C++ 源）。
- paint-pc 消费端：更新 SDK submodule 到修复提交后重编 `paint_pc`，人工用真实鼠标快速甩笔
  验证掉帧是否消失（本仓验证范围之外，记录为交付后续项）。
