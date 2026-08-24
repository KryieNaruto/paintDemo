# Krita 模块解析（净室重写参考 · 架构大纲）

> **用途**：DGCamp Paint 净室重写绘画内核的**模块切割与算法参考**。本文回答四个问题——Krita 的哪些子系统是我们要的（§1）、切哪些/切多少（§2）、每块怎么工作（§3）、边界与映射到 DGCamp 三接口（§4/§5）。与「画世界渲染管线解析.md」互补：画世界讲 GPU 管线怎么画（算法），本文讲 Krita 模块怎么组织（结构）。
>
> **数据来源**：`/home/qiansenwei/workspace/Krita_Linux/krita/`（Krita 6.x 官方源码）实测 + 既有边界分析（`paint_workspace/docs/boundary-analysis/` 07 等篇）。所有类/路径均已核实存在。
>
> **定位**：本版为**架构大纲**——先建立模块地图与切分结论，细节算法（每模块逐个深挖）后续按大纲补。

---

## 1. 总体架构与分层

Krita 围绕 **KisImage（图像模型）** 为中心组织，其他子系统向外辐射。对净室重写而言，只关心**绘画内核**相关层，UI/应用/文件格式层可整段排除。

### 1.1 与净室重写相关的分层

```
层 4 · 交互（输入 → stroke 投递）         libs/ui：canvas controller → input manager → tool
              │ 只保留「startStroke/addJob/endStroke」协议
层 3 · 笔触调度（异步 job 池）            libs/image：KisStroke + KisStrokesQueue + KisUpdateScheduler
              │ 调用 paintAt / doStroke
层 2 · 笔刷引擎（paintop 插件）           libs/image/brushengine + plugins/paintops/{libpaintop,defaultpaintops}
              │ 产出 dab 缓冲（KisFixedPaintDevice）
层 1 · 像素与绘画                        libs/image：KisPaintDevice + KisPainter + KisNode 树
              │ bltFixed 落层
层 0 · 地基                              libs/image/tiles3（分块存储） + libs/pigment（颜色/合成）+ 撤销
```

### 1.2 一条笔迹的数据流（净室重写的核心链路）

```
KisCanvasController → KisInputManager → KisToolFreehand
  → image->startStroke(strategy) / addJob(data) / endStroke(id)     [UI 线程，异步投递]
    → KisUpdateScheduler → KisUpdaterContext（工作线程池）
      → KisStrokeJob → KisStrokeJobStrategy → KisStrokeStrategy::doStroke
        → KisPaintOp::paintAt(info) → 产出 dab（KisFixedPaintDevice）
          → KisPainter::bltFixed → KisPaintDevice（tiles3 分块写入）
            → KoCompositeOp 合成 + KoColorSpace::convertPixelsTo 转色
              → KisTransactionData 记录 → 入 undo 栈
                → KisImage 投影刷新
```

> **净室重写洞察**：UI 线程与内核的唯一耦合就是 `(startStroke, addJob, endStroke)` 三元组。重写时 UI 层可整段砍掉，只保留这条投递协议。

---

## 2. 模块地图与物理边界（要切哪些）

| 模块 | 源码路径 | 构建归属 | 重写层级 |
|---|---|---|---|
| 分块像素存储 | `libs/image/tiles3/`（22 头文件） | 独立（`kritaimage` 内） | 层 0 · **地基，最先写** |
| 颜色科学 | `libs/pigment/`（KoColor / KoColorSpace / KoCompositeOp / Registry） | `kritapigment` 独立 target | 层 0 · **第二** |
| 撤销 | `libs/image/`（KisTransaction / KisUndoAdapter）+ tiles3 memento | `kritaimage` | 层 0 · 可先简化版 |
| 像素/绘画 | `libs/image/`（KisPaintDevice / KisPainter / KisNode 树） | `kritaimage` | 层 1 |
| 笔刷引擎 | `libs/image/brushengine/`（KisPaintOp / Factory / Registry）+ mask generator | `kritaimage` | 层 2 |
| 通用笔刷库 | `plugins/paintops/libpaintop/`（KisBrushBasedPaintOp / DabCache / Option 家族 / Sensor） | `kritalibpaintop` 独立 target | 层 2 |
| 内置笔刷 | `plugins/paintops/defaultpaintops/`（brush + duplicate 引擎） | MODULE 插件 | 层 2 |
| 笔刷资源 | `libs/brush/`（KisBrush + 各种格式）+ `libs/image/` mask generator | `kritalibbrush` 独立 target | 层 2（资源） |
| 笔触调度 | `libs/image/`（KisStroke / KisStrokesQueue / KisUpdateScheduler / KisUpdaterContext） | `kritaimage` | 层 3 |
| 输入链 | `libs/ui/`（canvas/input/tool） | `krita ui` | **排除**（只保留协议） |

### 2.1 两个容易切错的边界

- **mask generator 在 `libs/image/`，不在 `libs/brush/`**——笔尖形状生成（KisCircleMaskGenerator 等）物理上属于 image 层。切模块时别按直觉切到 brush。
- **依赖方向 `brush → image`，反向没有**：`libs/brush/` 的 KisBrush 依赖 `libs/image/` 的 KisMaskGenerator，反之不成立。必须保持单向，否则引入环。

---

## 3. 按模块拆解（核心章节）

每节固定骨架：**核心类 → 关键方法 → 净室重写可移植点 → 映射到 DGCamp**。

### 3.1 分块像素存储（tiles3）→ 画布纹理/tile

- **职责**：把无限画布切成 tile（默认 64×64），COW + 内存池 + memento 做极致级撤销。
- **核心类**：`KisTile`（共享 tile 数据）、`KisMemento`（撤销快照句柄）、`KisMementoManager`（历史栈 mem/swap 双层级）、`KisTiledDataManager`（KisPaintDevice 背后的管理类）、`KisHLineIterator2`/`KisRandomAccessor2`（迭代器）。
- **净室重写可移植点**：COW tile + memento + 哈希表自给自足不依赖上层，**应最先写、最完整复刻**——撤销与性能都在这。
- **映射**：对应 IRenderBackend 的画布存储概念 + IPaintKernel 的像素写路径。

### 3.2 颜色科学（pigment）→ 像素格式/合成算子

- **职责**：像素表示、颜色空间抽象、混合模式、颜色转换图。所有像素运算的数学底层。
- **核心类**：`KoColor`（带颜色空间的像素值）、`KoColorSpace`（`pixelSize`/`convertPixelsTo`/`createColorConverter`/`compositeOp`）、`KoCompositeOp`（合成 op，带 category：算术/亮度/HSV/HSL）、`KoColorSpaceRegistry`（单例注册表）。
- **关键方法**：`convertPixelsTo(src,dst,..)` 是 dab 与层间颜色转换的唯一入口；`compositeOp(id, srcSpace)` 支持跨色空间合成。
- **净室重写可移植点**：`KoColor`（值类型）→ `KoColorSpace`（布局+转换）→ `KoCompositeOp`（合成）三层叠加；registry 用 `KoGenericRegistry` 基类（Brush/PaintOp/Sensor registry 共用）。
- **映射**：对应 IRenderBackend 的合成算子 + IPaintKernel 的像素格式。

### 3.3 像素/绘画层 → 画布设备与合成

- **核心类**：`KisPaintDevice`（逻辑像素画布，`readBytes`/`writeBytes`/`exactBounds`）、`KisFixedPaintDevice`（**固定大小 dab 缓冲**，构造即定颜色空间）、`KisPainter`（`bitBlt`/`bltFixed`/`bitBltWithFixedSelection`——`bltFixed` 是 dab→device 主路径）、`KisNode` 树（`KisLayer`/`KisGroupLayer`/`KisPaintLayer`）。
- **净室重写可移植点**：painter 是 dab→layer 的唯一出口；node 树先只做 PaintLayer（group 后补）。
- **映射**：对应 IRenderBackend::composite + IPaintKernel 的 dab 落层。

### 3.4 笔刷引擎（brushengine + libpaintop）→ IPaintKernel::strokeTo（核心）

- **职责**：PaintOp 抽象、工厂、注册表、设置、笔触随机源——不依赖具体笔刷类型。
- **核心类**：
  - `KisPaintOp::paintAt(const KisPaintInformation&)` —— 核心入口，纯虚 `paintAt` 返回 spacing。
  - `KisPaintOpFactory`（`createOp`/`createSettings`/`createConfigWidget`）+ `KisPaintOpRegistry`（单例）。
  - `KisPaintInformation`（`pos`/`pressure`/`tilt`/`drawingSpeed`/`randomSource`）。
  - `KisBrushBasedPaintOp`（持 `KisDabCache`）、`KisDabCache`（按 paint info + 镜像 + 纹理 key 缓存 dab）。
  - **`KisSimplePaintOpFactory` 三件套模板**（引擎/设置/设置面板）——注册机制精髓。
- **内置引擎**（`defaultpaintops`）：`paintbrush`（KisBrushOp）+ `duplicate`（KisDuplicateOp）两个；**eraser 是 brush op 的擦除合成模式，非独立引擎**。
- **并行 dab 渲染管线**：`KisDabRenderingExecutor/Queue/Job` + `KisDabRenderingQueueCache`——提前渲染 dab、排队、按序落层（性能关键，DGCamp 可借鉴）。
- **净室重写可移植点**：`KisSimplePaintOpFactory` 模板工厂是注册机制精髓；默认引擎只需 brush + duplicate + eraser（模式）三个。
- **映射**：对应 **IPaintKernel::strokeTo 的核心实现**。

### 3.5 笔触系统（KisStroke）→ 异步笔触生命周期

- **职责**：把"一次连续绘画动作"拆成有序 job 队列，工作线程异步执行。
- **核心类**：`KisStroke`（`addJob`/`cancelStroke`/`suspendStroke`）、`KisStrokeStrategy`（生命周期数据工厂 `createInitData`/`createFinishData`/`createCancelData` + `addMutatedJob` 运行中注入）、`KisStrokeJobData` + `KisStrokeJobStrategy`（job 数据 + 执行策略）、`KisStrokesQueue`。
- **净室重写可移植点**：用 barrier 语义；UI 线程只投递 `(startStroke, addJob, endStroke)`。
- **映射**：对应 IPaintKernel::beginStroke/strokeTo/endStroke 的异步调度壳。

### 3.6 笔刷资源（libs/brush）→ BrushParams / 预设

- **核心类**：`KisBrush`（基类）、`KisAutoBrush`（**由 KisMaskGenerator 程序化生成，非图像**）、`KisBrushesPipe`（图管笔刷）、各种格式（`KisGbrBrush`/`KisPngBrush`/`KisAbrBrush`/`KisImagePipeBrush`）、`KisQImagePyramid`（缩放 mip 金字塔）。
- **mask generator**：`KisMaskGenerator` + `KisCircleMaskGenerator`/`KisGaussCircleMaskGenerator`/`KisRectangleMaskGenerator` + `struct FastRowProcessor`（SIMD 逐行 mask 填充，性能关键）。
- **映射**：对应 IPaintKernel 的 `BrushParams`/`createBrush` + DGCamp 的 `dgcLoadBrushFromMyb` 预设加载。

---

## 4. 接口边界（入口/出口）

净室重写只需保留以下协议：

| 模块 | 入口（谁调） | 出口（调谁） |
|---|---|---|
| Stroke | UI 调 `startStroke/addJob/endStroke` | `KisStrokeJobStrategy::doStroke` |
| PaintOp | scheduler 调 `paintAt(info)` | 产出 `KisFixedPaintDevice` dab |
| Painter | PaintOp 调 `bltFixed` | 写 `KisPaintDevice` + KoCompositeOp |
| Tile | PaintDevice 调 `writeBytes` | tiles3 写 + memento 抓差 |
| Color | Painter 调 `convertPixelsTo` | KoColorSpace 转换图 |

---

## 5. 与 DGCamp §4.0 三插拔接口的映射

| DGCamp 接口 | 对应 Krita 概念 | 移植难度 |
|---|---|---|
| `IPaintKernel::createBrush` | `KisBrush` + mask generator + `KisPaintOpFactory` | 中（重写） |
| `IPaintKernel::beginStroke/strokeTo/endStroke` | `KisStroke` + `KisStrokeStrategy` + `KisPaintOp::paintAt` | 中（重写） |
| `IPaintKernel::countDabsTo/prepareAndDrawDab` | `KisPaintInformation` spacing/timing + `KisDabRenderingQueue` | 中（借鉴） |
| `IRenderBackend::composite` | `KisPainter::bltFixed` + `KoCompositeOp` 语义 | 中（GPU 化改写） |
| `IRenderBackend::clearCanvas/present` | `KisImage::projection` 投影刷新 | 低（自建） |
| `IPlatform::pollInput/runLoop` | `KisInputManager` 输入链 | **低（自建，UI 层排除）** |

### 5.1 无对应、需自建的接口

- **确定性渲染**（DGCamp §4.0.3）：Krita 无此概念。需注入 seed 替换 `KisPerStrokeRandomSource` 的 RNG。
- **离屏渲染/readback**（§4.0.5）：Krita 无 headless 概念。自建 Vulkan storage image + readback。
- **CLI 宿主**（§4.0.6）：Krita 无。自建。

### 5.2 非目标（Krita 有但 DGCamp 明确不做）

DGCamp §4.0 非目标（文件读写、图层/混合模式/蒙版、项目管理、导出/导入、撤销重做栈可留接口、社交社区）→ 对应 Krita 的 `libs/impex`（文件格式）、`libs/ui` 面板/工具、`kritacommand` 撤销栈（可留接口）、`plugins/filters`、动画、矢量、扩展。**重写范围应主动排除这些。**

---

## 6. 净室重写参考要点（结论）

1. **地基顺序**：tiles3 分块存储 → pigment 颜色/合成 → 撤销（简化版）→ stroke 调度 → device/painter/node → paintop。tiles3 最先写、最完整复刻。
2. **依赖纪律**：`brush → image` 单向；mask generator 在 `libs/image` 不在 `libs/brush`。违反则引入环。
3. **UI 层整段排除**：只保留 `(startStroke, addJob, endStroke)` 投递协议，用最小事件源模拟。
4. **注册机制**：`KisSimplePaintOpFactory` 三件套模板是引擎扩展的骨架，值得原样搬。
5. **dab 缓存管线**：`KisDabCache` + `KisDabRenderingQueue` 的提前渲染/排队/按序落层是笔刷性能关键，DGCamp 应借鉴。
6. **同步原语**：6.x 已无 `registerSynchronizedEventBarrier`，现役是 `barrierLock()`/`KisUpdateScheduler::barrier`。重写直接用 barrier 语义。

---

## 7. 非目标与可跳过清单

| 跳过子系统 | 原因 | 是否留接口 |
|---|---|---|
| `libs/impex` 文件格式（.kra/PSD） | DGCamp 非目标 | 否 |
| `libs/ui` 面板/工具框架 | 重写 UI 在 Compose/ImGui 侧 | 否（留输入协议） |
| `kritacommand` 撤销栈 | DGCamp 可留接口 | **是**（`dgcUndo` 留） |
| `plugins/filters` | 非目标 | 否 |
| 动画 / 矢量 / 扩展 / 脚本 | 非目标 | 否 |

> 若未来要图层/蒙版/选区/滤镜，回看：蒙版→ `libs/image` 的 mask/selection、选区→ `KisSelection`、滤镜→ `plugins/filters`。

---

## 附：子系统索引速查（重写三态）

| Krita 概念 | 关键路径 | 核心类 | 状态 |
|---|---|---|---|
| 分块存储 | `libs/image/tiles3/` | KisTile / KisTiledDataManager / KisMemento | **重写（地基）** |
| 颜色科学 | `libs/pigment/` | KoColorSpace / KoCompositeOp | **重写** |
| 像素/绘画 | `libs/image/` | KisPaintDevice / KisPainter | **重写** |
| 笔刷引擎 | `libs/image/brushengine/` | KisPaintOp / KisPaintOpFactory | **重写（核心）** |
| 笔刷资源 | `libs/brush/` + image mask | KisBrush / KisMaskGenerator | **重写** |
| 笔触调度 | `libs/image/` | KisStroke / KisStrokesQueue | **重写** |
| dab 缓存 | `plugins/paintops/libpaintop/` | KisDabCache / KisDabRenderingQueue | **参考** |
| 撤销 | `libs/image/` + tiles3 | KisTransaction / KisUndoAdapter | **简化/留接口** |
| 输入链 | `libs/ui/` | KisInputManager / KisToolFreehand | **排除**（留协议） |
| 文件格式 | `libs/impex/` | 21+ 个过滤器 | **排除** |
