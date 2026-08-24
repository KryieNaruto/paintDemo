# 参考文档（净室重写参考）

> 本目录沉淀 DGCamp Paint **净室重写**所需的逆向与架构参考。所有事实均可回溯到原始素材（RenderDoc 抓帧 / Krita 源码），不含臆造。供任务线实现与评审引用。

## 篇目

| 文档 | 内容 | 数据来源 | 用途 |
|---|---|---|---|
| [画世界渲染管线解析.md](画世界渲染管线解析.md) | 画世界 App 的实际 GPU 渲染管线逆向：shader 集合、笔刷 dab 算法（SDF 覆盖/fwidth 抗锯齿）、画布自混合（16F/32F 三态 + fragment interlock）、图层合成蒙版链、纸纹 | `RenderDoc结果/qqq.rdc`（RenderDoc v1.46 GLES 抓帧） | 净室重写 **GPU 笔刷内核 / 合成后端**（对应 IRenderBackend + IPaintKernel 的 GPU 化） |
| [Krita模块解析.md](Krita模块解析.md) | Krita 绘画内核的模块切割与架构大纲：分层、物理边界、逐模块核心类、接口边界、与 DGCamp 三接口映射、非目标清单 | `/home/qiansenwei/workspace/Krita_Linux/krita/`（Krita 6.x 源码）+ 既有边界分析 | 净室重写 **模块划分 / 代码组织 / 移植决策** |

## 两篇的分工

- **画世界篇**：算法层——**GPU 管线怎么画**（笔刷 dab 数学、合成策略、精度模式）。
- **Krita 篇**：结构层——**模块怎么组织**（类骨架、依赖方向、切哪些、跳哪些）。

两者在「与 DGCamp §4.0 三接口映射」处交汇：画世界 → IRenderBackend（GPU 合成），Krita → IPaintKernel（内核逻辑）。

## 关联

- 技术规划：`DGCPaint_技术规划.md` §4.0/§4.5（分层接口与 compute 合成目标）
- 既有调研：`docs/调研/`（路线评审）
- Krita 深层分析（外部工作区）：`paint_workspace/docs/krita/`（设计意图）、`paint_workspace/docs/boundary-analysis/`（切割分析）
