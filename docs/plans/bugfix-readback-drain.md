# 修复计划 · readback 不先 drain 导致画布缺 dab（PC 空洞 + 20fps）

> 对应 bugfix-pipeline 报告：PC 端 20fps（安卓 60fps）+ 线条 dab 之间有空洞。

## ① 根因（已复现，含证据）

**现象**：消费端 paint-pc 每帧调用 `dgcReadbackPixels` 全画布读回，**读回前不调 `dgcFlush`**。
引擎是「输入线程 → 笔刷线程 → 渲染线程」三线程异步模型，渲染线程合成 dab 有滞后。
`dgcReadbackPixels` 直接 `backend->readback()` 拷贝画布，**不等待尚未合成的 dab**，
因此返回的是**缺最近 dab 的旧画布** → 线条出现空洞；同时渲染线程积压的巨批 composite
在 flush 时一次性提交，持 backend mutex 阻塞消费端 readback → 帧时间爆炸（20fps）。

**证据**（`tests/test_readback_drain.cpp`，host lavapipe 无头复现，3 次稳定）：
```
ink no-flush=654  flush=4586  缺=3932 (85.7%)
```
即不 flush 直接读回，画布缺 86% 的最近 dab。flush 后读回 = 完整画布。

**对照消费端**：`paint-android`（`PaintScreen.kt`）用 dirty 标志 + **读回前先 `dgcFlush`**
+ 画布封顶 1080×720 → 画布完整、60fps。`paint-pc`（`src/app.cpp`）每帧无条件全画布
readback + 全画布 `glTexImage2D` 上传，且不 flush → 空洞 + 慢。

**影响面**：所有经 `dgcReadbackPixels` / `dgcExportPNG` 读回的调用方（paint-pc、paint-android、
CLI、tests）。CLI 与 Android 已自行先 flush（侥幸正确），paint-pc 未 flush（触发 bug）。
SDK 层 readback 语义缺陷是根因。

## ② 修复方案（不回退、主路径）

**改 `sdk_api/dgc_paint_c_api.cpp`**：

1. `dgcReadbackPixels`：在 `backend->readback()` 前，若 `engine->running()` 则先
   `engine->flush()`（drain 屏障，与 `dgcFlush` 同逻辑）。保证拷贝到的画布已合成全部
   已提交输入。
2. `dgcExportPNG`：同样在 `backend->exportPNG()` 前先 drain（内部即 readback，自足化）。

无死锁：`engine->flush()` 不持 backend mutex（渲染线程 composite 时短暂持有，flush 纯轮询
等待 composited_ 追平），flush 返回后渲染线程空闲，readback 再取 mutex 拷贝。
flush 幂等：无未决输入时直接返回，空闲读回开销不变（实测 60× 空闲读回 0.43ms/帧）。

**20fps 说明**：修复后每次 readback 触发 drain，渲染线程积压变为「每帧有界批」，不再累积
巨批阻塞；readback 返回的也是完整画布（无空洞）。paint-pc 侧「全画布每帧 readback +
全画布上传」在高分辨率下仍有带宽成本（属消费端优化，另开 D6 线跟踪；本修复保证读回正确）。

## ③ 回归用例（先红后绿）

**新测试 `tests/test_readback_drain.cpp`**（已建，红）：模拟 paint-pc 行为——喂一条长直线
（121 点）后**不 flush 直接 `dgcReadbackPixels`**，与 `dgcFlush` 后读回对比墨迹像素数。

- **红（当前）**：no-flush=654 vs flush=4586，缺 85.7% → 断言 `missing < 5%` 失败。
- **绿（修复后）**：no-flush 读回前自动 drain，两次数值接近（缺 < 5%）→ 通过。

```cpp
// 核心断言
const int missing = inkFlushed - inkNoFlush;
CHECK(missing < inkFlushed / 20, "readback without flush is complete (missing<5%)");
```

## ④ 影响面核对

- `dgcReadbackPixels` 语义变更：从「当前已合成画布快照」→「drain 后完整画布快照」。
  更强契约，现有调用方（已自行 flush 的 CLI/Android）幂等无害。
- `dgcExportPNG`：CLI 已在 export 前 flush，修复后自足，行为不变。
- 确定性（B5-3 golden）：readback 前多一次 drain，像素输出不变（drain 只等已提交工作，
  不改变合成结果）。`test_determinism` 必须全绿。
- 所有权/泄漏（B1-8）：不引入新资源，无影响。

## ⑤ 验证方式（无头）

- `cmake --build build/host-linux` 后跑新增回归 `test_readback_drain`（先红后绿记录）。
- 全量 `ctest`（host-linux，含 test_determinism / test_gpu_dab_raster / test_engine /
  test_brush_offscreen 等）0 失败 0 跳过。
- `android-arm64` preset 仍可编出 `.so`（C API 层改动无平台差异）。
- 离屏图像：CLI 跑笔画脚本导出 PNG，确认连续无空洞（现有 gap_repro 脚本，人工目检）。
