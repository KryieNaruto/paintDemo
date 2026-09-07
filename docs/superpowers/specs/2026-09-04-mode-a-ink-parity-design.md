# 设计 · Mode A 逼近 Ink 手感（渲染管线 + 预测线）

> 状态：已过 brainstorming 用户审阅（2026-09-04），待写实施计划。
> 触发：A8-2（预测瞬态 wet-tip 层）真机复测暴露——SDK 渲染路径（Mode A）快速挥摆下肉眼落后
> 手指约 2cm，同设备同应用内切到 Jetpack Ink 对照组肉眼 0 延迟。项目要求跨平台，不能用 Ink
> 替代交互态渲染，只能让 Mode A 自身逼近 Ink 手感。

## 1. 背景与已定量根因

真机（MDP1221，MediaTek MT6789/Mali-G57 MC2）实测、非猜测：

- **视觉落后随手速线性放大**：600mm/s 快速挥摆 ≈2cm（20mm）；100mm/s 慢速仅 ≈3.3mm（肉眼不明显）。
- **总落后 = 模型器平滑滞后（wobble+spring）+ 渲染管线延迟**，600mm/s 下两者量级相当：
  - 模型器滞后 ≈10.4mm（wobble_timeout=10ms/spring 默认参数下的稳态位置滞后）。
  - 渲染管线延迟 ≈9.3mm（真机测得「输入→上屏」代理 15.5ms × 600mm/s）。
- **Ink 与 Mode A 用的是同一套 modeler 算法**（B1-5 白盒移植结论），Ink 之所以肉眼 0 延迟，根因是
  **渲染路径**不同：Ink 走 HWUI 前缓冲矢量直接上屏（无 readback）；Mode A 走「Vulkan 离屏 raster →
  GPU 回读 → CPU Bitmap → Compose 双缓冲上屏」。
- **管线延迟的真实构成**（真机 `DGCPAIN_PERF` 插桩逐点验证，`__android_log_print`，非 fprintf——
  真机 `user` 固件不转发 native stderr 进 logcat，这是本次排查中新发现的平台限制）：
  - 渲染线程「攒批等待」（`kMaxBatchDurationMs=4ms` 定时器）**恒为 0**（`byTimeCap` 全样本为 0），
    **不是瓶颈**——此前假设有误，已用真实手指画线数据证伪。
  - 每次读回请求触发**两次连续 GPU 提交**：composite dab（真实批，~0.3-0.7ms）+ 该批**顺带**
    做的读回刷新合成（~2.2-4.8ms，多数样本落在 2.5-3ms）。慢的那次不是画 dab 慢，是
    `RefreshReadbackCacheLocked()` 自己**单独一次** `BeginCommands()/SubmitAndWait()`（GPU
    提交+等 fence 的固定开销），与画 dab 那次提交是分开的两次往返。
  - 结论：**两次独立 GPU 提交往返**（一次 composite、一次 readback-copy）是当前管线延迟的主要
    构成，而非 CPU memcpy 本身（`__android_log_print` 前测得纯 memcpy 只需约 0.9ms）。
- **预测尖现有覆盖率不足**：600mm/s 下 interval=20ms 的预测尖理论最远伸出 ≈9.25mm（约合计缺口
  的 47%），且已修复方向误差（`bugfix-prediction-curve-overshoot`，弦方向替代卡尔曼方向外推，
  已解决弧外毛边）。若 interval 超过约 25ms，紧曲率弧线（半径 60-120px≈6-12mm）会重新超出
  3px 毛边阈值，是线性外推模型对曲线路径的几何误差，非参数误调，**不能靠单纯调大 interval 解决**。
- **额外发现一个真实渲染 bug（未修）**：`core/engine.cpp` `flushAccum()` 先 composite 真实批、
  再 composite 预测批；而 `snapshotRefreshRequested_` 原子 `exchange` 谁先执行 `composite()`
  谁就消费掉刷新标志——真实批永远先于预测批，导致读回快照经常合成「上一批」的旧 tip 而非
  「这一批」刚画好的新 tip，预测尖视觉更新总慢半拍（~4-16ms）。

## 2. 目标与验收标准

**目标**：Mode A 交互态手感逼近 Ink（不替代 Ink，交互态仍是 SDK 自身渲染）。

**验收指标**：600mm/s 快速挥摆下，视觉总落后距离 ≤3mm（现状 ≈19.7mm）。

**非目标**：
- 不追求与 Ink 逐位一致的架构（不引入 HWUI/矢量 mesh）。
- 不在本设计内做曲率感知预测模型（先验证渲染管线修复后剩余缺口是否已被现有线性外推覆盖，
  不够再另开任务评估）。
- paint-pc 消费端本次不动。

## 3. 架构总览：两条线

| 线 | 改动 | 目标 |
|---|---|---|
| 渲染管线（4a） | 合并 composite + 读回刷新为一次 GPU 提交 | 砍掉一次固定 GPU 往返开销（~2-3ms） |
| 渲染管线（4b） | 新增 swapchain 直接上屏路径（Android 先行） | 消除 readback+Bitmap+Compose 整条 CPU 侧链路 |
| 预测线（4c） | 修复 stale-tip 顺序 bug | 预测尖视觉更新不再慢半拍 |
| 预测线（4d） | 4a/4b 落地后真机重新验证 tuning | 确认剩余缺口（预计主要是模型器自身滞后）是否已被现有 interval/wobble 覆盖 |

两条线的关系：4a/4b 把"渲染管线延迟"这部分缺口压掉；缺口压掉后，4d 用真机数据确认预测尖是否
已足够覆盖剩下的（主要是模型器自身滞后）部分——**只有 4d 验证不足时才需要考虑更大的预测模型
改动（曲率感知外推），本设计不预先假设需要**。

## 4. 组件设计

### 4a. 合并 GPU 提交（低风险，纯 SDK 内部）

- 文件：`render/vulkan/vk_backend.cpp`。
- 现状：`CompositeLocked()` 结尾 `snapshotRefreshRequested_.exchange(false,...)` 为真时调用
  `RefreshReadbackCacheLocked()`；后者自己 `BeginCommands()...SubmitAndWait()`，与
  `CompositeLocked()` 自身的 `BeginCommands()...SubmitAndWait()` 是两次独立提交。
- 改动：把"是否需要刷新读回缓存"的判断挪到 `CompositeLocked()` 的 `BeginCommands()` 之后、
  `SubmitAndWait()` 之前，把 `RefreshReadbackCacheLocked()` 内的 GPU 命令（merge dispatch
  条件性 + `vkCmdCopyImageToBuffer`）追加进**同一个** command buffer，只在末尾统一
  `SubmitAndWait()` 一次；`SubmitAndWait()` 之后再做 `vkMapMemory`/`invalidate`/`memcpy`/
  `vkUnmapMemory`（这部分不涉及 GPU 提交，可以留在 wait 之后）。
- 影响面核对：`ClearCanvasLocked`/`ClearTipLocked` 自己独立调用 `RefreshReadbackCacheLocked()`
  的路径（非 composite 触发）**不受影响**，继续保留各自独立提交（这两处频率低，不是本次优化
  目标）。
- 零回归判据：`test_readback_drain`/`test_midstroke_readback`/`test_perf_regression`/
  `test_snapshot_refresh_throttle`/`test_determinism` 全绿，导出 PNG 逐位不变。

### 4b. Swapchain 直接上屏（Android 先行，新路径与离屏路径并存）

- **新 SDK C API**：新增入口绑定原生 onscreen surface（如
  `dgcBindOnscreenSurface(void* nativeWindow, int w, int h)`），与现有 `dgcSetOffscreenSurface`
  并存、互斥（同一时刻只绑一种，或允许同时绑定——离屏用于 export，onscreen 用于交互显示，
  具体互斥/并存策略留给实施计划阶段细化，需先探查 `dgc_paint_c_api.h` 现有 surface 绑定生命周期
  约定）。
- **`VkBackend` 改动**：新增可选 `VkSwapchainKHR` 成员（仅 onscreen surface 绑定时创建）；
  依赖 `VK_KHR_surface` + `VK_KHR_android_surface`（Android 侧）+ `VK_KHR_swapchain` 扩展。
  每次 composite 完成、swapchain 已绑定时：`vkAcquireNextImageKHR` → `vkCmdBlitImage`
  （`canvasImage` 或 `displayImage`——按 `tipHasContent_` 同现有 readback 逻辑选择源 →
  当前 swapchain image）→ `vkQueuePresentKHR`。全程 GPU 内部操作，不读回、不 CPU 拷贝。
  **离屏 `canvasImage` 权威路径完全不动**——determinism/导出继续读它，逐位不变。
- **消费端（paint-android）改动**：`Canvas`+`Bitmap`+双缓冲+`ReadbackScheduler`+
  `nativeReadback`+`drawLagProbe` 这套交互态显示链路整体废弃，替换为 `AndroidView` 包一个
  `SurfaceView`；JNI 新增入口把 Kotlin `Surface` 转 `ANativeWindow*`（`ANativeWindow_fromSurface`）
  传给 SDK 的 `dgcBindOnscreenSurface`。导出 PNG 功能不变（仍走 `dgcExportPNG` 一次性离屏读回）。
- **已知限制（本轮不处理，记录留痕）**：swapchain 生命周期（resize/旋转/切后台/`Surface`
  丢失重建）需要专门的状态机；本轮只覆盖 Android、竖屏为主的正常前台绘制场景。

### 4a/4b 的独立交付顺序（用户已确认）

**先落地 4a、真机验证收益后，4b 再单独走一轮完整的申领→计划→审阅→测试流程**——4a 风险低、
有现成测试覆盖；4b 涉及新 C API + 平台耦合 + 消费端大改，独立评审风险收益更清晰，不绑在一次
交付里。

### 4c. 修复 stale-tip 顺序 bug（预测线，低风险）

- 文件：`core/engine.cpp` `flushBatch()` 内的 `flushAccum` lambda。
- 现状：`flushAccum()` 先 `composite(realStamps, false)` 后 `composite(predStamps, true)`；
  `snapshotRefreshRequested_.exchange()` 在**每次** `CompositeLocked()` 结尾检查，先执行的
  real composite 若命中刷新标志，会用「上一批」的旧 tip 内容合成快照。
- 改动方向（实施计划阶段定稿）：预测批先于真实批 composite（`predStamps` 先于 `realStamps`），
  或者把刷新判断从「每次 `CompositeLocked()` 结尾」挪到 `flushAccum()` 整体结束之后统一判断
  一次（更贴合本设计 4a 的"合并提交"思路，可能可以合并实现）。具体选择留给实施计划阶段，
  需核对是否与 4a 的合并提交改动冲突/可复用。

### 4d. 预测线重新验证（3b，验证性任务，非代码改动为主）

- 4a（+若已落地的 4b）完成后，真机重新测量 600mm/s 下「预测开/关」的实际视觉落后距离。
- 若落后距离已 ≤3mm 达标：预测模型（弦方向外推 + interval=20ms/wobble=10ms）无需再改。
- 若仍不达标：另开任务评估曲率感知外推（本设计不预先设计该方案）。

## 5. 测试与验证计划

| 改动 | 验证方式 |
|---|---|
| 4a 合并提交 | host ctest 全绿（`test_readback_drain`/`test_midstroke_readback`/`test_perf_regression`/`test_snapshot_refresh_throttle`/`test_determinism`），导出 PNG 逐位不变；真机重测「输入→读回」代理 lag（应从 ~6ms 降至 ~3-4ms 量级） |
| 4b swapchain | 真机手感对照（同一手势切换渲染路径前后）；「输入→上屏」延迟代理需重新设计测量口径（新路径没有 readback-complete/Compose-draw 这两个可挂钩的节点，具体探针方案留给实施计划阶段） |
| 4c stale-tip 修复 | `test_wet_tip` 等既有 A8-2 测试零回归；真机预测开启时肉眼确认 tip 不再慢半拍 |
| 4d 重新验证 | 真机 600mm/s 快速挥摆总落后距离 ≤3mm（本设计的最终验收指标） |

## 6. 风险与范围边界

- **4b 是本设计最大风险点**：新 C API 表面、平台原生 surface 生命周期管理、消费端渲染架构级
  改动（Bitmap→SurfaceView）。已决定 Android 先行、PC 另开，且与 4a 分开独立交付。
- **4a 风险低**：纯内部重构，不改对外接口，现有测试直接覆盖零回归判据。
- **4c 风险低**：改动范围局限在 `flushAccum` 一个函数内的 composite 顺序/刷新判断时机。
- **paint-pc 本次不动**：继续用离屏+readback 现状，不在本设计范围内。
- **曲率感知预测模型不在本设计范围**：4d 验证后如仍不达标，需要另一轮 brainstorm 设计（本设计
  不预判需要）。

## 7. 4d 验证结论（真机，MDP1221，2026-09-06）

**未达标。**

实测（sdk submodule `e7529e7`，paint-android `89f13b5`，600mm/s 快速挥摆）：

| | 输入→读回 lag | 输入→上屏 lag | 肉眼落后距离 |
|---|---|---|---|
| 预测关 | 6ms | 16ms | 明显落后，估计 >5mm |
| 预测开 | 6ms | 16ms | 与关几乎无区别，明显落后，估计 >5mm |
| Ink（对照） | n/a | 6ms | 肉眼 0 延迟 |

对照 4a 修复前的基线（§1：读回 6ms / 上屏 15.5ms）：**两个 lag 数字几乎没有变化**（16ms vs
15.5ms 在测量粒度内基本等同），4a（合并 composite 与读回刷新为一次 GPU 提交）预期的
「省一次 Mali 固定 2-3ms 提交往返」在真机整体延迟代理上**没有体现出可测的收益**。预测开/关
肉眼几乎无区别，与 4c 修复前观察到的现象一致——预测尖领先量级仍被总延迟淹没。

根因待查，可能方向（未验证，留给后续独立任务）：
- 4a 合并优化命中率不及预期——本次真机连续快速挥摆场景下，`composite()` 触发的刷新占比、
  以及是否真的进了合并路径（vs. 走 `ClearCanvasLocked`/`flushReadbackCache` 等仍然独立提交
  的调用点）需要用真机 `dgcTestSubmitAndWaitCount`/`dgcTestCompositeCount` 插桩实测验证，
  本轮未做这一步（host 测试验证的是构造场景，非真机连续绘制场景下的实际触发比例）。
- 每次独立 GPU 提交的固定开销本身可能被低估，或者真正的瓶颈在提交开销之外（Compose
  重组/draw 调度、bitmap 双缓冲切换、其他真机侧未插桩的环节）。
- 模型器自身滞后（wobble+spring）可能比 §1 估算的更大。

**决定**：本计划（4a 合并提交 + 4c stale-tip 顺序修复）到此为止，不在本轮继续深挖根因或
设计新方案——按 §6 范围边界，管线延迟的进一步定位与曲率感知预测模型均留作后续独立任务
（需要真机插桩+连续绘制场景下的合并命中率实测，作为下一轮 brainstorm 的输入）。4c 的
stale-tip 修复本身仍然是有效且独立成立的正确性修复，不因 4d 未达标而回退。

## 8. A8-3 Phase 0 验证结论（候选①host 核验证伪；候选②真机核验：瓶颈在 Compose/vsync 地板，
   §5.1 降低节流被真机数据证伪为无效改动，不采纳；验收标准口径核对见 §8.3）

`docs/plans/A8-3.md` 的 Phase 0 承接本节 §7 留下的三个候选根因，逐条核验：

### 8.1 候选①（4a 合并命中率在高频读回下是否退化）—— host 侧核验：证伪

新增 `tests/test_readback_merge_under_load.cpp`（`DGCPAIN_TEST_HOOKS` 门控，ctest 注册名
`test_readback_merge_under_load`）：在 `test_snapshot_refresh_throttle.cpp` 的 24 线连续
stroke 场景基础上，**中途每 8 个笔点插入一次 `dgcReadbackPixels`**（整段笔画共 753 次高频
交织读回，远高于消费端 `ReadbackScheduler` 现有 16ms 节流下单笔画的实际读回密度），断言
`dgcTestSubmitAndWaitCount(ctx) - dgcTestCompositeCount(ctx)` 的增量不随读回调用次数
线性增长。

**实测结果**（host，`-DDGCPAIN_TEST_HOOKS=ON`，Debug + `-DDGCPAIN_SANITIZE=ON` 均跑过，
ASan/LSan 零泄漏）：

| | compositeCount | submitAndWaitCount | readbackCalls | 差值 |
|---|---|---|---|---|
| 无中途读回（`test_snapshot_refresh_throttle`，基线） | 22 | 26 | 0 | +4 |
| 753 次高频中途读回（`test_readback_merge_under_load`，本任务新增） | 22-23 | 26-27 | 753 | +4 |

`submitAndWaitCount - compositeCount` 在两种场景下**完全相同（恒为 +4，即 dgcClear 自身
两次提交 + dgcFlush 收尾一次 + dgcExportPNG 一次的固定常数）**，与读回调用次数（0 →
753）无关——4a 合并在高频读回交织场景下**没有退化**，命中率健康。这是 §2/§3.1 分析的
「布尔事实、不依赖具体耗时数字」这一命题在 host 侧的确定性验证，与真机连续挥摆场景下
应观测到的行为按代码路径一致性推断应当相同（合并逻辑是纯代码路径判断，不依赖 GPU 型号/
真机时序）。

**判定**：候选①**证伪**（与 §7/本计划 §3.1 的预判一致）。决策树按 §3.4「候选①否 → 继续」
分支推进，不需要触碰 SDK 生产代码。

### 8.2 候选②（瓶颈是否在 GPU 提交往返之外）与 §5.1（消费端节流值真机逐档实测）—— 真机实测：候选②对「降低节流有收益」这个推论证伪

真机（MDP1221，`paint-android` `task/A8-3` 分支，`a8-2-preverify` 基线，SDK submodule 仍
钉 `e7529e7`，与 §7 基线同一 SDK 代码状态）实测，`ReadbackScheduler.DEFAULT_MIN_INTERVAL_NS`
改为 4ms（§5.1 梯度第一档，最激进候选），600mm/s 真实手指连续快速挥摆：

| | 输入→读回 lag（`lagProbe`） | 输入→上屏 lag（`drawLagProbe`） |
|---|---|---|
| §7 基线（16ms 节流，4a/4c 已落地） | 6ms | 16ms |
| 本次 4ms 节流候选、预测关 | 12ms | 16ms |
| 本次 4ms 节流候选、预测开 | 12ms | 16ms（与预测关几乎一样） |
| Ink（对照，本次同一真机同一时段测） | n/a | 4ms |

人工肉眼原话：「预测开关仍然手感无变化」。

**候选②当初的预判**（§3.2）是：`readMs`（纯 SDK 侧 memcpy）显著小于 `lagProbe`，差值主要
来自消费端 `ReadbackScheduler` 的 `minIntervalNs` 节流等待——若属实，把该节流值降低应该能
按比例压低 `lagProbe`、进而压低 `drawLagProbe`。真机数据**部分证实、部分证伪**这个预判：

- **证实的部分**：瓶颈确实不在 SDK 的 GPU 提交往返（§8.1 已证伪合并退化；本节 §7 基线
  `readMs` 量级本就远小于 `lagProbe`/`drawLagProbe`）——候选②"瓶颈在 GPU 提交往返之外"这句
  本身成立。
- **被证伪的部分（关键，不能回避）**：候选②隐含的推论——"因此降低消费端读回节流值能改善
  端到端上屏延迟"——**不成立**。节流从 16ms 降到 4ms（放宽 4 倍）后：
  - **「输入→上屏」lag（`drawLagProbe`，这是任务行「body 拖后」直接对应的指标）完全没变**
    （16ms → 16ms，与 §7 基线一模一样），说明端到端上屏延迟被消费端读回节流值**以外**的
    因素钉死，不受该节流值影响。
  - **「输入→读回」lag（`lagProbe`）反而从 6ms 恶化到 12ms**——读回更频繁（4x）没有换来
    更短的等待，反而更差。机制未细究（推测与后台读回单线程 dispatcher 更高频调度/竞争、
    或更频繁的 `requestFlush` 原子操作与主线程输入处理的相互干扰有关），但方向明确：
    这是一个**真实的轻微副作用**，不是测量噪声可以完全解释的量级（6→12ms，翻倍）。
  - 两者合起来看：`drawLagProbe - lagProbe`（Compose/draw 那一段的表观占比）从 §7 基线的
    `16-6=10ms` 缩到本次的 `16-12=4ms`，而总量恒为 16ms——这个「此消彼长、总量不变」的模式
    是「下游有一个不随上游提速而提前的硬性 vsync 边界」的典型signature，与 §5.3（本计划）
    对 Compose 单一 Choreographer 帧管线 1-vsync 固有下限的分析完全吻合：无论读回在这一帧
    窗口内多早完成，合成后的帧仍然只能在下一个 vsync 边界上屏，读回完成得更早只是把「等待」
    从「读回排队」这一段移到了「等 vsync」那一段，总时长不变。

**判定（按 §3.4 决策树的精神，而非机械套用其字面分支）**：候选②"瓶颈在 GPU 提交往返之外"
成立，但真正的下游瓶颈是 Compose/vsync 帧管线本身（§5.3 已分析的架构下限），**不是**
`ReadbackScheduler` 的节流值本身。§5.1"降低节流值"这个具体修复手段被真机数据**证伪为无效
改动**：零端到端收益 + 读回侧轻微副作用。**不采纳 4ms（或更低）候选，`DEFAULT_MIN_INTERVAL_NS`
维持原值 16ms**——`paint-android` `ReadbackScheduler.kt` 已改回 16ms 并在注释里记录本次真机
实测结论与数据（避免未来有人凭直觉重新尝试同样的改动、重复踩坑）。这不是"因为真机数据不好看
就回避改动"，而是如实执行计划 §9 R2 早已声明的取舍原则：「若所有候选值都无法在不掉帧前提下
提供可感知的 lag 改善，如实报告『本任务未能进一步降低』」——本次实测甚至更进一步：不仅没有
改善，还有轻微副作用，结论应该更明确地是"不改"，而非"暂缓"。

### 8.3 A8-3 验收标准「body 拖后 ≤~1 帧」的口径核对——很可能在改动前已经满足

任务行原文：「body 拖后 ≤~1 帧（肉眼差距较 Ink 可接受）」。核对本仓库对同一措辞的既有用法：

- `docs/plans/P7-3.md`：任务行「…感知延迟 ≤1 帧…」，验收字段逐字写明**「真机快速笔画，笔尖
  到墨迹视觉滞后 ≤1 帧」**，并把「真机连续绘制 Frame ≤16.7ms（60Hz）」直接等同于「感知延迟
  ≤1 帧」——这是本仓库对「≤1 帧」这个说法唯一一次给出可操作定义的地方，且定义是**渲染管线
  自身的输入→上屏延迟**（帧时间倒数），不是「与 Ink 对照的视觉距离差」。
- A8-3 所在的 spec（本文档）§2 给出的是另一个、更严格的独立指标：「视觉总落后距离 ≤3mm」
  （600mm/s 下），这是 A8-1/A8-2/4d 一直在用、且 4d 已确认「未达标」的指标（≈16-20mm）。
  任务线 A8-3 这一行是在 4d「未达标」结论**之后**才写入的（`docs/plans/ephemeral-predicted-tip-eval.md`
  §… 已预告"A8-3（已画 body 延迟收敛 ~1 帧）为后续任务"），量纲上「~1 帧@60Hz≈16.7ms」与
  spec §2 的「≤3mm≈5ms@600mm/s」明显不是同一把尺子——如果 A8-3 的「~1 帧」就是 spec §2 的
  「≤3mm」的换算写法，两者数字对不上（16.7ms ≠ 5ms），说明 A8-3 任务行是在 4d 未达标后**刻意
  放宽、重新定级**的验收线，采用的正是 P7-3 先例那种"渲染管线自身延迟 ≤1 帧"的口径，而不是
  重复 spec §2 那个已经证明够不到的更严指标。

**结论**：「body 拖后 ≤~1 帧」按本仓库既有先例（P7-3），最合理的读法是「笔尖到墨迹的渲染
管线延迟本身 ≈1 帧（60Hz 下 ≈16.7ms）」，括号里的「肉眼差距较 Ink 可接受」是对这个数字的
效果说明（P7-3 同款「渲染管线达到 60fps 视为感知延迟可接受」的论证方式），不是另一个独立的、
更严格的「与 Ink 差距 mm」量化门槛（那是 spec §2 的门槛，已知达不到，也不是 A8-3 任务行的
文字）。

`drawLagProbe`（输入→上屏 lag）**在 A8-3 任务开始之前（§7 基线，4a/4c 已落地，节流仍是
16ms）就已经是 16ms**，本次真机候选测试（§8.2）确认它不受节流值改变影响、恒为 16ms——
16ms 与「1 帧 @60Hz ≈16.7ms」几乎重合。也就是说，**按上述口径，「body 拖后 ≤~1 帧」这条
验收标准在 A8-3 开始改动之前大概率就已经满足**，前提是 fps 能稳定保持 60（P7-3 已经把这个
条件确立为常态基线，本任务真机测试全程 FPS 面板显示 60.1，未观测到掉帧）。

这不意味着 A8-3"什么都不用做"：Phase 0 排查（§8.1/§8.2）本身就是任务行「必要时评估」的
落地，且**排除了两个曾经被怀疑的根因**（4a 合并退化、消费端节流可优化空间）——这本身是有
价值的结论产出，只是最终没有落地成"改一个数字"的代码改动，而是"确认不该改、记录为什么"。
如实结论：**本任务范围内的验收指标（body 拖后 ≤~1 帧）在改动前后均已满足（16ms≈1帧、
fps 稳定 60），A8-3 未采纳任何会改变这一数字的代码修改**；至于「肉眼差距较 Ink 可接受」这
句话字面上仍有主观判定空间——本次真机人工判定「预测开关手感无变化」，未获得"落后手指
明显多"的负面反馈，但也未专门就"较 Ink 差距是否可接受"给出单独的肯定评语，如实记录、不
替人工下结论。剩余的、任务线未要求本任务处理的差距（若有），根因大概率落在候选③（模型器
wobble/spring 滞后，spec §1 定量约 10.4mm@600mm/s）与 spec §4b（swapchain 直接上屏，
消灭 16ms 的 Compose/vsync 地板），两者均在 A8-3 范围外，留给后续任务。

### 8.4 §5.3/§5.4 评估结论（复用计划文档，本次真机数据进一步印证）

「尽早换帧上屏」「必要时评估低延迟 present」两项的评估结论已在 `docs/plans/A8-3.md` §5.3/
§5.4 给出并经 build-pipeline 审阅通过：Compose 单一 Choreographer 帧管线下 `bitmap = back`
之后必须等下一个 vsync 才 recompose+draw，是 Bitmap+Compose 架构固有的 1-vsync 量级下限；
真正消灭这段 CPU 侧链路的手段是 spec §4b（swapchain 直接上屏），已按 spec 既定交付顺序
留给独立任务，不在 A8-3 范围内实现。**本节 §8.2 的真机数据（4x 放宽节流后端到端 lag 分毫
不变）为这个"下游 vsync 地板"分析提供了直接的实证支持**（此前只是代码路径分析，未经真机
验证），不再是纯推测。
