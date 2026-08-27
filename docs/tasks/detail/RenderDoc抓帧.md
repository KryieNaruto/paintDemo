# 任务书 · SDK RenderDoc 程序化抓帧（线5-闭环）

> 本任务书对应 `docs/tasks/任务线.md` 任务 **B5-4**。背景事实可回溯到：
> - `render/vulkan/vk_backend.h:11` —— 离屏 Canvas storage image（常驻 GENERAL），**不创建 swapchain，`present()` no-op**。
> - `render/vulkan/vk_backend.h:17` —— 窗口/swapchain 路径**本期不做**（init 收到非空 surface 时记录未实现）。
> - `DGCPaint_技术规划.md` §3.2 ——「GPU 抓帧 | RenderDoc（Android Vulkan）| 逐 dispatch 分析 compute shader」列为辅助环境工具（性能测量手段）。

## 背景

SDK 渲染为**纯离屏**：Vulkan 后端自建 VkInstance/VkDevice，画布为常驻 GENERAL storage image，`brush_composite.comp` compute 合成 + readback，**从不创建 swapchain、从不 `vkQueuePresentKHR`，`present()` 空操作**。

RenderDoc 常规「present 帧」抓帧按 swapchain / `vkQueuePresentKHR` 界定帧边界：SDK 从不上屏，且其 compute dispatch 发生在**独立 VkDevice** 上（与宿主 present 的 device/context 不同一），因此渲染内核管线在 RenderDoc 里不可见 —— 技术规划 §3.2 的「逐 dispatch 分析 compute shader」能力无法落地。

本任务给 SDK 加 **RenderDoc 程序化注入**：运行时加载 renderdoc 库、用 `RENDERDOC_API_1_1_1` 在 composite dispatch 前后包 `StartFrameCapture(vkDevice)` / `EndFrameCapture(vkDevice)`，使离屏内核管线**无 present 也可被抓**；宿主 present 帧仍走 RenderDoc 常规抓帧。

## B5-4 · SDK RenderDoc 程序化抓帧

**目标**：本机 Windows（MSVC）编译 SDK 并配合消费者宿主（paint-pc）启动后，RenderDoc 能同时抓到**宿主 present 帧**与**SDK 内核渲染管线**（`brush_composite.comp` compute dispatch、shader 源码/资源绑定可查）。

**产出**

1. RenderDoc 运行时注入（renderdoc 库不存在时优雅降级，零回归）：
   - Windows `LoadLibrary("renderdoc.dll")` / Linux `dlopen("librenderdoc.so")` 动态加载，经 `RENDERDOC_GetAPI` 取 `RENDERDOC_API_1_1_1`；库句柄 **RAII 包装**（沿用 SDK 所有权约束：所有权一律 RAII，无裸 `new`/`delete` 所有权）。
   - 未启用 / 加载失败时**不加载、不报错**，现有离屏路径行为不变。
2. 抓帧钩子（Vulkan 后端）：
   - 在 `brush_composite.comp` dispatch 前后（含对应 barrier）调 `StartFrameCapture(vkDevice)` / `EndFrameCapture(vkDevice)`，按 **SDK 自身 device** 抓帧。
   - 可配 `SetCaptureFilePathTemplate` / `SetCaptureOption`（捕获目录、文件数等）。
   - 在单一渲染线程/队列内包住完整 dispatch，遵守现有帧同步，不破坏确定性/时序。
3. 开关（**默认关**）：
   - 环境变量（如 `DGC_RENDERDOC=1`）自动开启；
   - （可选）C API / CLI 开关，计划阶段定（保持 C ABI 不暴露内部类型，Pimpl 隐藏实现）。
4. `renderdoc_app.h` 采购：入 fetch-deps 清单或 vendor 单头（计划阶段定，遵循仓库三方库供给约定）。
5. 验收宿主对接说明：paint-pc（独立仓库）启动时如何启用抓帧（文档或最小改动）。

**验收**（人工验收：本机 Windows MSVC + paint-pc 宿主）

- Windows 编译 SDK 与 paint-pc 宿主并启动，RenderDoc 下：
  1. 常规抓帧能抓到宿主 **present 帧**（画面正确上屏）。
  2. 同一 RenderDoc 会话内，程序化抓帧能抓到 **SDK 内核管线**：`brush_composite.comp` compute dispatch 可见，shader 源码 / 资源绑定可查看。
- 无 renderdoc 库 / 未启用环境下**零回归**：现有 host `ctest` 全绿（含 B5-3 确定性、B4-1 CPU/GPU 对照）；`android-arm64` preset 仍可编出 `.so`。
- 人工验收记录：RenderDoc 会话截图 / 抓帧文件路径 + 结果登记。

**依赖理由**：B2-1（Vulkan 离屏后端 + `brush_composite.comp` dispatch 已落地）。B4-1（dab GPU 化）已在 main，抓的是当前完整 GPU 管线。验收宿主 paint-pc 在**独立仓库**（本任务线不含 UI 消费者），验收清单注明宿主即 paint-pc。

---

## 评审打「通过」的必要条件

| 任务 | 指标 |
|---|---|
| B5-4 | renderdoc_app.h 注入 + Start/EndCapture 包 composite；默认关、无 renderdoc 库零回归；Windows 人工验收 present 与 SDK 管线都可见（会话截图/抓帧文件为证） |

---

> **后续不在本任务书范围**：swapchain/present 路径（`vk_backend.h` 标注「本期不做」）、宿主（paint-pc）自身渲染、Android 端 RenderDoc 抓帧（本期验收仅 Windows）。
