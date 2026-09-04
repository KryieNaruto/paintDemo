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
