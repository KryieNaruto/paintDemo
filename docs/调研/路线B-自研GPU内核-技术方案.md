# 路线 B · 自研 GPU 笔刷内核 + Vulkan（全 GPU）技术方案

> **阶段**：原型技术路线细化（评审落地的详细设计）
> **定位**：dab 生成 + 合成全在 GPU compute shader，从零自研，对齐画世界PRO 的 shapeTex/距离场/hardness 渲染模型
> **评审结论**：加权得分 4.03（第二），是唯一能拿满「性能 5 分」的路线，也是路线 E 的 GPU 化演进终局
> **关键约束**：开发全程由 AI 实现；手感调参 AI 不可替代，需美术人工反馈兜底
> **日期**：2026-08-20

---

## 1. 技术路线概览

**一句话定位**：把画世界PRO 的「vertex 属性 → fragment 形状 → blit 合成」三段式 OpenGL 管线，平移到 Vulkan Compute Shader，dab 的形状光栅化与画布合成全部在 GPU 完成，CPU 只保留「输入点流 + 轻量参数层」，从根源上消除 CPU stamp 生成瓶颈。

```
┌──────────────────────────────────────────────────────────────────────┐
│  输入层 · Ink Stroke Modeler（平滑预测 / 预测，CPU）                   │
│  MotionEvent → Update/Predict → 预测 → ring_buffer(StrokePoint)       │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ StrokePoint {x,y,pressure,tilt_x,tilt_y,t}
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  参数层（CPU·轻量，可热调）—— 或 GPU dab_generate.comp                │
│  平滑 → 间距 → 压力曲线 pow(p,1.5) → 圆度 → 抖动 → HSV 随机            │
│  产出 DabVertex 流 {halfWidth, roundness, rotation, hardness, opacity, │
│                     colorHSV, jitterSeed}                             │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ DabVertex SSBO（仅一次 CPU→GPU 上传）
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  GPU Compute（Vulkan）                                                │
│  ① dab_raster.comp     shapeTex + 距离场 + hardness → stamp alpha     │
│  ② brush_composite.comp  premultiplied over → Canvas storage image    │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ 布局常驻 GENERAL，无过渡
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  present.vert / present.frag → swapchain → 屏幕（60/120Hz）            │
└──────────────────────────────────────────────────────────────────────┘
```

**与画世界PRO 抓帧模型的对应关系**（技术规划 §8 附录 A）：

| 画世界PRO（OpenGL 460） | 路线 B（Vulkan Compute） | 说明 |
|---|---|---|
| vertex shader 每顶点属性 pressu/inRoundness/halfLineWidth/inColorHSVRand/inOpacity | `DabVertex` SSBO 结构体 | 同样以「dab 顶点属性」为单位 |
| vertex 阶段 `Tmb = pow(pressu, 1.5)`、random 抖动、calcMipmapLevel | dab_generate / 参数层 | 压力→宽度映射同公式 |
| fragment shader shapeTex + dist + hardness + fwidth 抗锯齿 | dab_raster.comp | 距离场 + smoothstep 同公式 |
| premultiplied alpha blit（Direct/Remul/dePremul） | brush_composite.comp | over 运算同公式 |

---

## 2. 总体架构

### 2.1 全 GPU 管线与 compute shader 划分

路线 B 的 GPU 侧由 **3 个 compute shader + 1 个 graphics shader** 构成，按「一次笔触一批」批量执行：

| Shader | 阶段 | 输入 | 输出 | 关键点 |
|---|---|---|---|---|
| `dab_generate.comp` | 参数层 GPU 化（可选） | 点流 SSBO + 笔刷 UBO | DabVertex SSBO（append） | 压力曲线/间距/抖动/HSV 随机，确定性 |
| `dab_raster.comp` | dab 形状光栅化 | DabVertex SSBO + shapeTex + shapeTexSampler | stamp 纹理（premultiplied） | shapeTex × SDF × hardness × fwidth AA |
| `brush_composite.comp` | 画布合成 | stamp 纹理 + Canvas storage image | Canvas（就地 over） | premultiplied over + 3 扩展位 |
| `present.vert/frag` | 上屏 | Canvas（sampled） | swapchain | 纯贴图，1 draw |

> **融合优化**：单图层原型可把 `dab_raster` 的形状数学**内联进** `brush_composite`，省去一次 stamp 纹理往返（GPU 本地读写，代价极小但仍值得省）。分层保留 stamp 纹理是通用形态，为「纸纹调制、双重笔刷、stamp 复用（撤销重放）」留位。原型阶段建议**先写融合版**（一个 pass 搞定），验证性能后再拆分层版。

### 2.2 与三插拔接口的映射

路线 B 不改变三个插拔接口的**边界语义**，只改变 `IPaintKernel` 的实现方式（详见 §5）：

| 接口 | 职责变化 | 实现位置 |
|---|---|---|
| `IPaintKernel` | 从「CPU 生成 StampData」→「生成 GPU dab 属性流 + 提交 compute 命令」 | `kernels/gpu/` |
| `IRenderBackend` | 合成 + 上屏不变，新增「执行内核产出的 compute 命令」能力 | `render/vulkan/` |
| `IPlatform` | 完全不变（surface/input/lifecycle） | `platform/` |

**关键架构张力**：dab 生成现在是 GPU compute，与 `IRenderBackend` 的 Vulkan device 高度耦合。解法是引入一个**共享 GPU 上下文**（`IGpuContext`，封装 device/queue/descriptor 池），`IPaintKernel` 与 `IRenderBackend` 都持有它；内核产出「抽象的 compute 命令描述」，由后端统一录制执行。这样内核仍是「平台无关的算法」，后端仍是「API 的具体执行者」。

### 2.3 3 线程模型如何退化 / 调整

原规划 3 线程：`Input → Brush → Render`。路线 B 下 Brush 线程的 CPU 重活（libmypaint 光栅化）消失，退化为两种形态：

| 形态 | 线程 | 说明 |
|---|---|---|
| **形态一（推荐起步）** | Input → **Prep（参数层，CPU 轻量）** → Render | 手感算法（压力曲线/间距/抖动）留在 C++，便于 AI/美术快速迭代；Prep 只填 DabVertex SSBO，不做任何像素光栅化 |
| **形态二（终局全 GPU）** | Input → Render（2 线程） | 参数层也搬进 `dab_generate.comp`，Prep 合并进 Render，纯 GPU |

> **为什么推荐先走形态一**：dab 的「参数数学」开销极小（每点几个 float 运算），CPU 跑 <0.1ms，不是瓶颈；瓶颈在 stamp 光栅化（每 dab 上千像素），那才是必须 GPU 化的部分。把「手感参数」留在 CPU，收益是**改手感曲线无需重编译 shader**，直接热加载 LUT，这对「AI 不擅长手感、需美术反复试」的约束至关重要。等手感冻结后，再把参数层平移进 GPU（形态二），是「改自己代码」而非换库。

---

## 3. 核心模块设计

### 3.a dab 生成 compute shader（形状光栅化）

这是路线 B 的**核心 shader**，把「dab 顶点属性 + shapeTex」变成「stamp 像素 alpha」，等价于画世界PRO 的 vertex+fragment 融合进一个 compute 调用。一个 invocation 负责「某个 dab 包围盒内的一个像素」。

```glsl
// dab_raster.comp —— dab 顶点属性 + shapeTex + 距离场 + hardness → stamp alpha
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// ── dab 顶点属性（对齐画世界PRO：pressu/inRoundness/halfLineWidth/inColorHSVRand/inOpacity）──
struct Dab {
    vec2  pos;          // 画布像素坐标（抖动后中心）
    float halfWidth;    // 半线宽 = 半径（已含压力映射 pow(p,1.5)）
    float roundness;    // 圆度 0~1（1=正圆）
    float rotation;     // 旋转角（rad）
    float hardness;     // 边缘软硬 0~1
    float opacity;      // 本 dab 不透明度（已含压力→不透明度）
    vec3  colorHSV;     // HSV 颜色（已含抖动）
    float jitterSeed;   // 随机种子（纹理偏移/抖动）
};
layout(std430, set = 0, binding = 0) readonly buffer Dabs { Dab dabs[]; };

layout(set = 0, binding = 1) uniform sampler2D u_ShapeTex;   // 笔刷形状纹理（§3.b 烘焙）

// 输出 stamp 像素（premultiplied）；单图层融合版可去掉，直接 imageLoad/Store canvas
layout(std430, set = 0, binding = 2) writeonly buffer StampPixels {
    vec4 stamps[];   // 布局：dabIdx * boxSize^2 + py*boxSize + px
};

layout(push_constant) uniform PC {
    uint boxSize;    // dab 包围盒边长（= ceil(2*halfWidth+2)）
    uint shapeType;  // 0=圆形 SDF, 1=方形 SDF, 2=纯纹理
} pc;

vec3 hsv2rgb(vec3 c) {
    // 标准 HSV→RGB，省略实现
}

void main() {
    ivec3 gid = ivec3(gl_GlobalInvocationID);
    uint dabIdx = gid.z;
    if (dabIdx >= dabs.length()) return;
    Dab d = dabs[dabIdx];

    int bs = int(pc.boxSize);
    ivec2 px = gid.xy;
    if (px.x >= bs || px.y >= bs) return;

    // 1. 局部坐标 → 归一化 [-1,1]，中心为 dab 中心
    vec2 local = (vec2(px) + 0.5 - 0.5 * float(bs)) / d.halfWidth;

    // 2. 圆度 + 旋转 → 椭圆（roundness 沿旋转轴压扁）
    float c = cos(d.rotation), s = sin(d.rotation);
    vec2 q = mat2(c, s, -s, c) * local;
    q.y /= max(d.roundness, 0.01);

    // 3. 距离场：圆形 SDF（方形用 box SDF = max(|q.x|,|q.y|)）
    float dist = (pc.shapeType == 1u) ? max(abs(q.x), abs(q.y)) : length(q);

    // 4. hardness 控制边缘软硬：smoothstep(hardness,1,dist)
    //    （对齐画世界PRO；azul-core brush_dab_coverage = 1 - smoothstep(hardness,1,t)）
    float shape = 1.0 - smoothstep(d.hardness, 1.0, dist);

    // 5. shapeTex 调制：纹理 alpha 与距离场相乘（纹理笔刷/灰度 shape）
    //    calcMipmapLevel：按 dab 尺寸选 mipmap，避免缩放走样
    vec2 texUV = (vec2(px) + 0.5) / float(bs);
    float lod = max(0.0, log2(d.halfWidth * 2.0 / 64.0)); // 64=基础纹理尺寸
    float texA = textureLod(u_ShapeTex, texUV, lod).r;

    // 6. fwidth 抗锯齿：修正 smoothstep 斜率（对齐 sumShapeTexAlp 的 fwidth AA）
    float w = fwidth(dist);
    float aa = 1.0 - smoothstep(d.hardness - w, 1.0 + w, dist);
    float alpha = max(shape * texA, aa * texA) * d.opacity;  // 取两者，保证边缘 AA

    if (alpha <= 0.001) return;

    // 7. 输出 premultiplied stamp 像素
    vec3 rgb = hsv2rgb(d.colorHSV);
    uint slot = dabIdx * uint(bs) * uint(bs) + uint(px.y) * uint(bs) + uint(px.x);
    stamps[slot] = vec4(rgb * alpha, alpha);
}
```

**dispatch 方式**：`vkCmdDispatch(ceil(boxSize/8), ceil(boxSize/8), dabCount)`，即 z 维索引 dab、xy 维索引 dab 内像素。所有 dab 一次 dispatch 完成（若 dab 总数超 z 上限 65535，按批次拆）。

### 3.b 笔刷形状纹理 shapeTex 的生成与烘焙

shapeTex 是笔刷形状的灰度 alpha 纹理，两种来源：

1. **程序化烘焙（自研基线，圆形/方形）**：init 时用 compute 生成一张 R8 纹理（64×64 或 128×128，带 mipmap），把 hardness 边缘**烤进纹理**，运行时 raster shader 只需采样，无需每像素算 smoothstep（省计算，且 mipmap 天然抗走样）。

```glsl
// bake_shape.comp —— 程序化生成笔刷形状纹理（距离场 + hardness），一次烘焙
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, r8) uniform image2D u_ShapeOut;

layout(push_constant) uniform PC {
    float hardness;   // 烘焙进纹理的边缘硬度
    uint  shapeType;  // 0=圆, 1=方
} pc;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_ShapeOut);
    vec2 p = (vec2(px) + 0.5) / vec2(sz) * 2.0 - 1.0;

    float dist = (pc.shapeType == 1u) ? max(abs(p.x), abs(p.y)) : length(p);
    float a = 1.0 - smoothstep(pc.hardness, 1.0, dist);  // 边缘在纹理内已烘焙
    imageStore(u_ShapeOut, px, vec4(a));
}
```

2. **纹理笔刷（灰度图 shape，对齐画世界PRO shapeTex）**：从 mybrush/Krita `.myb` 预设读取灰度 stamp 图，`vkCmdCopyBufferToImage` 上传为 mipmap 纹理。这是「纹理笔刷」功能（功能清单 #2）的承载。

> **bake vs 运行时算**：烘焙把 smoothstep 成本移到 init；但「硬度随压力动态变化」（功能清单 #15 压力→硬度）需要**运行时** hardness，故 §3.a 里同时保留「运行时 smoothstep(d.hardness)」与「烘焙纹理」两条路径——纹理内烤基础硬度，运行时 hardness 作为额外调制乘上去。

### 3.c 合成 compute shader（premultiplied over）

对齐画世界PRO 的 blit 模型（`dst = src + dst*(1-src.a)`），并预留三个合成扩展位（图层能力）：

```glsl
// brush_composite.comp —— premultiplied over 合成到画布
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba8) uniform image2D u_Canvas;       // premultiplied 常驻
layout(set = 0, binding = 1) uniform sampler2D u_Stamp;             // stamp（premultiplied）
layout(set = 0, binding = 2) uniform sampler2D u_LayerMask;         // 图层蒙版（预留）
layout(set = 0, binding = 3) uniform sampler2D u_LockAlpha;         // 阿尔法锁定（预留）
layout(set = 0, binding = 4) uniform sampler2D u_SelMask;           // 选区裁剪（预留）

layout(push_constant) uniform PC {
    vec2  stampPos;    // stamp 在画布上的像素坐标
    vec2  stampSize;   // stamp 纹理像素尺寸
    float opacity;     // 全局不透明度
    uint  flags;       // bit0=lockAlpha, bit1=layerMask, bit2=selMask
} pc;

void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_Canvas);
    if (c.x >= sz.x || c.y >= sz.y) return;

    vec2 uv = (vec2(c) - pc.stampPos) / pc.stampSize;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return;

    vec4 src = texture(u_Stamp, uv);      // premultiplied stamp
    float a = src.a * pc.opacity;
    if (a <= 0.001) return;

    vec4 dst = imageLoad(u_Canvas, c);    // premultiplied canvas

    // ── 三个扩展位（对应画世界PRO 图层能力）──
    if ((pc.flags & 1u) != 0u) {          // lockAlpha：只在已画像素写
        if (dst.a <= 0.0) return;
    }
    if ((pc.flags & 2u) != 0u) {          // layerMask：蒙版调制 alpha
        a *= texture(u_LayerMask, uv).r;
        if (a <= 0.001) return;
    }
    if ((pc.flags & 4u) != 0u) {          // selMask：选区裁剪
        if (texture(u_SelMask, uv).r <= 0.5) return;
    }

    // premultiplied over：dst = src + dst*(1-src.a)
    vec4 outC = vec4(src.rgb + dst.rgb * (1.0 - a),
                     a + dst.a * (1.0 - a));
    imageStore(u_Canvas, c, outC);
}
```

**合成策略**（沿用规划 §4.5，画世界PRO 模型）：
- alpha 统一 **premultiplied** 存储，`over` 运算 = `src + dst*(1-src.a)`。
- **批量 dispatch**：一次笔触的所有 stamp 累积后按包围盒批量 dispatch，避免每 stamp 一次 dispatch 的 CPU/GPU 开销。
- **包围盒**：只 dispatch stamp 覆盖区域 `ceil(box/8)`，不全画布；大画布（≥2048²）再 tile 化。
- **stamp 纹理池**：stamp 纹理用环形池复用（TRANSFER_DST|SAMPLED），避免每次分配。

### 3.d 笔刷参数 GPU 侧传递（push constant / uniform / storage buffer）

三类传递机制按「变化频率」分工：

| 机制 | 承载内容 | 变化频率 | 理由 |
|---|---|---|---|
| **push constant** | composite 的 `stampPos/stampSize/opacity/flags`、raster 的 `boxSize/shapeType` | 每 stamp / 每 dispatch | ≤128B（Vulkan 保底 128B），更新零成本，无需 descriptor 更新，最贴合「每 stamp 变」的小参数 |
| **uniform buffer (UBO)** | 笔刷静态参数：压力曲线指数、spacing、scatter、hueJitter、baseRadius、hardnessBase | 每笔刷（换笔刷才更新） | 一笔刷一笔 UBO，换笔刷时 `vkUpdateDescriptorSets` 或 dynamic UBO 一次更新 |
| **storage buffer (SSBO)** | 点流、DabVertex 数组（append + atomic counter） | 每帧 / 每笔触 | 动态数量、可变长，UBO 装不下；readonly/writeonly 标记利于驱动优化 |

**关键设计**：把「压力曲线 / 间距 / 抖动」等手感参数**数据化成 LUT（查找表）**存进 UBO/SSBO（如 256 段 pressure→radius 曲线），而非硬编码 GLSL 常量。这样美术调参 = 改 LUT 数据，**无需重编译 shader**，直接热加载——这是「AI 不擅长手感、需美术迭代」约束下最重要的一条设计。

### 3.e 输入层（点流 → 每个 dab 的顶点属性如何生成）

从 `StrokePoint` 到 `DabVertex` 的参数层（形态一在 CPU，形态二在 `dab_generate.comp`）：

```
StrokePoint {x, y, pressure, tilt_x, tilt_y, t_us, is_predicted}
    │
    ├─ ① 预测：速度外推（自研 ~30 行，或 MotionEvent 历史）
    ├─ ② 平滑：one-euro / 滑动平均，消除硬件抖动（功能清单 #104）
    ├─ ③ 间距：step = max(1px, halfWidth * spacing)；两点间距离 > step 时等分插值补 dab
    ├─ ④ 压力→半线宽：halfWidth = baseRadius * pow(clamp(pressure,0,1), 1.5)
    │                  （对齐画世界PRO Tmb = pow(pressu.x, 1.5)）
    ├─ ⑤ 圆度/旋转：roundness 来自笔刷 + tilt；rotation 来自 tilt azimuth / 笔刷设置
    ├─ ⑥ 压力→不透明度/硬度：opacity = opacityBase * 压力曲线；hardness 可随压力
    └─ ⑦ 抖动/HSV 随机：jitter = scatter * (hash(seed)-0.5)；HSV 加 hash 抖动
                          seed 用「stroke 内 dab 序号」，保证确定性
```

**预测点覆盖策略**（同规划）：预测 dab 标 `is_predicted`，真实点到达后以真实 dab 重合成该段（预测只降观感延迟，不作为最终像素保留）。GPU 化的好处：预测修正 = 多 dispatch 一段，无 CPU stamp 回滚成本。

---

## 4. 数据流（触控 → 上屏完整生命周期）

```
触控笔接触（t=0）
  → MotionEvent 进入 Ink Stroke Modeler（含 pressure/tilt/t）
  → StrokeModeler::Update/Predict 平滑预测点流    ▸ ~8–12ms（输入管线，预测压低观感）
  → StrokePoint push ring_buffer                    ▸ ~0.05ms（无锁 SPSC）
  → 参数层生成 DabVertex 流（形态一 CPU / 形态二 GPU）▸ <0.1ms（每点几个 float）
  → DabVertex 写入 SSBO（一次 memcpy）               ▸ <0.1ms（原始属性远小于 stamp 纹理）
  → vkCmdDispatch dab_raster.comp（形状光栅化）      ▸ <1ms（GPU 并行，随 dab 尺寸/数量变）
  → vkCmdDispatch brush_composite.comp（over 合成）  ▸ <2ms（硬性预算，Vulkan timestamp 实测）
  → present.vert/frag → swapchain → v-sync 上屏      ▸ 0~16.6ms（60Hz）/ 0~8.3ms（120Hz）
```

**耗时预算表**：

| 阶段 | 预算 | 说明 |
|---|---|---|
| 输入 + Ink 建模/预测 | 8–12ms | 与路线无关的固定成本，预测掩盖 |
| 点流 → DabVertex（参数层） | <0.1ms | 轻量算术，非瓶颈 |
| DabVertex → GPU SSBO | <0.1ms | 取代原「stamp 上传 <1ms」——原始属性比 stamp 纹理小 1–2 个数量级 |
| dab_raster + composite（GPU） | <2ms | 硬性指标，timestamp query 实测 |
| present + v-sync | 0–8.3ms | 120Hz 屏 |
| **端到端（计算+合成+上屏）** | **~10–15ms** | **<30ms 达标** |

**为何能 <30ms 的关键论证**：
1. **消除 CPU stamp 生成**（原 libmypaint 单 stamp <3ms 的 CPU 预算整个消失）与 **CPU→GPU stamp 纹理上传**（原 <1ms）两道工序，代之以「一次属性上传 + 两次 dispatch」。
2. **Canvas 常驻 GENERAL 布局**，dab_raster → composite 之间无 image layout transition，barrier 最少。
3. **一次 submit 一条笔触**，GPU 连续执行，无 CPU 往返中断（对齐 Blender sculpt 的「dab 坐标喂 GPU、stroke 结束才回读」思路，原型甚至不回读）。
4. 剩余延迟是「输入 + v-sync」的固定成本，是所有路线共有的下限；120Hz 屏 + Ink 预测可把它压到 ~10ms 观感级。

---

## 5. 关键接口设计（IPaintKernel 在 GPU 化下的映射）

### 5.1 核心结论：`strokeTo` 不再有意义，演进为「GPU 顶点流提交」

原接口 `strokeTo → std::vector<StampData>` 隐含「CPU 物化 stamp 像素」。路线 B 下 stamp 从不落到 CPU，该语义失效。演进方向是**「提交点流 → GPU 生成 dab」**：

```cpp
// 演进后的 IPaintKernel（GPU 化形态）
class IPaintKernel {
public:
    virtual ~IPaintKernel() = default;

    // 创建笔刷：向 GPU 上传 shapeTex + 笔刷 UBO（含 LUT 曲线），返回句柄
    virtual BrushHandle createBrush(const BrushParams&) = 0;

    virtual void beginStroke(BrushHandle, const StrokePoint&) = 0;

    // ★ 演进核心：不再返回 StampData，而是把点流提交进 GPU SSBO
    //   （可批量 appendPoints，减少调用次数）
    virtual void submitPoints(BrushHandle,
                              const std::vector<StrokePoint>&) = 0;

    // 抬笔：封存本 stroke 的 dab 范围，产出「待执行 compute 命令」
    virtual void endStroke(BrushHandle) = 0;

    // ★ 新增：拉取本帧要执行的 compute 命令（dab_generate/raster 的 dispatch 描述），
    //   由 IRenderBackend 统一录制执行 —— 内核不直接碰 Vulkan device
    virtual GpuBrushCommands flushCommands() = 0;
};
```

### 5.2 为什么引入 `flushCommands()` 而非内核直接 dispatch

dab 生成与 `IRenderBackend` 的 Vulkan device/queue 高度耦合。若内核直接 `vkCmdDispatch`，会破坏「换渲染后端（Vulkan→Metal/bgfx）内核不动」的可插拔性。解法：内核产出**平台无关的 `GpuBrushCommands`**（SSBO 引用 + dispatch 尺寸 + barrier 描述），后端把它翻译成具体 API 调用。这样：

- 内核 = 纯算法（dab 属性生成、形状定义），平台无关，host 可单测。
- 后端 = API 执行者，保留可替换性。

### 5.3 中间过渡形态（AI 执行友好）

完全 GPU 化前，可保留一个「瘦 `strokeTo`」过渡接口：`strokeTo` 返回**轻量 `DabVertex` 列表**（不含像素，仅几个 float），后端把 `DabVertex` 上传 + dispatch raster。这与路线 E 的差异仅在于「stamp 像素在 GPU 生成 vs CPU 生成」，接口改动最小，便于对照测试与渐进替换。

---

## 6. 关键技术难点与解决方案

### 6.1 手感算法从零设计的难度（最大风险）

libmypaint 的手感来自 ~100 个设置 + 多年社区调校（压力曲线、间距、平滑、颜色动态）。路线 B 从零写，**最大的坑不是代码而是手感**。

**解决方案：不发明手感，移植手感公式。** libmypaint 是 ISC 许可（评审 §4.1 已确认），其**默认压力曲线、spacing、smoothing 参数可以合法照抄**作为起点。路线 B 与路线 E 的差别只是「这套公式跑在 CPU 还是 GPU」，不是「公式从哪来」。所以：
- 参数层的公式**照抄 libmypaint 默认值**（pressure→radius 的 pow 曲线、dabs_per_radius 的 spacing 等）；
- 用**数据化 LUT** 承载这些曲线，美术只调 LUT 不碰代码；
- 用路线 E 的 host 对照测试做**数值一致性验证**（同一输入点流，CPU dab 参数 vs GPU dab 参数 diff）。

### 6.2 压力曲线 / 抖动 / 纹理调制的实现

| 手感项 | 实现 | 对齐 |
|---|---|---|
| 压力→粗细 | `halfWidth = baseRadius * pow(pressure, 1.5)` | 画世界PRO `Tmb=pow(pressu,1.5)` |
| 压力→不透明度 | `opacity = opacityBase * 压力曲线(pressure)` | 功能清单 #14 |
| 抖动 Scatter | `jitter = (hash(seed)-0.5) * scatter * halfWidth`，确定性 hash | 功能清单 #11 |
| HSV 随机 | `hue += (hash(seed+1)-0.5)*hueJitter`，逐 dab 偏移 | 画世界PRO inColorHSVRand |
| 纹理调制 | `alpha = shape(SDF×hardness) × texA(shapeTex)` | 画世界PRO shapeTex |
| 圆度/旋转 | 椭圆 SDF：`q.y /= roundness` 再旋转 | 画世界PRO inRoundness |

### 6.3 GPU 随机数确定性（关键工程点）

抖动/HSV 随机若用原子计数器或 `rand()`，会导致**同一笔触两次绘制像素不同**，破坏：撤销重放、预测点覆盖修正、双平台一致性、对照测试。

**解法**：用**确定性 hash 种子**（PCG hash），种子 = `strokeId * 大常数 + dab 序号`，与执行顺序/线程调度无关：

```glsl
uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rand01(uint seed) { return float(pcg(seed)) / 4294967295.0; }
vec2  rand2 (uint seed) { return vec2(rand01(seed), rand01(seed + 1013904223u)); }
```

### 6.4 美术调参 AI 无法替代的兜底

AI 擅长「写公式、暴露参数」，无法判断「这支笔的手感好不好」。兜底三层：

1. **参数全部数据化（LUT + JSON）**：压力曲线/spacing/抖动幅度都从配置读取，美术改配置即生效，AI 不参与主观判断。
2. **host 端参数热调 harness**：PC 侧（ImGui）做一个「参数滑块 → 实时重画」的调试面板，美术拖动滑块看即时效果，迭代周期从「改代码重编译」降到「拖滑块」。
3. **以 libmypaint 默认手感为基线**：默认参数直接抄 ISC 的 libmypaint 默认值，保证「起步就不差」，AI 无需从零调。

---

## 7. 四维度评审（5 分制）

| 维度（权重） | 得分 | 理由 |
|---|---|---|
| 复杂度（25%） | **3.5** | 无交叉编译黑洞、无黑盒依赖，compute shader 是 AI 强项；但比路线 E 多一层「GPU 手感算法」要自己写对 |
| 性能（30%） | **5** | 唯一满分：全 GPU 无 CPU 瓶颈，dab 生成+合成都在 GPU，天花板即画世界PRO/Procreate |
| 可控性（25%） | **5** | 代码 100% 自研，改算法/加类型/调混合/换 API 全自由，最终形态完全掌控 |
| 时间（20%） | **2** | 从零写手感算法 + 美术调参迭代周期长，是最慢落地的路线 |
| **加权总分** | **4.03** | 第二，性能与可控性满分，时间拖后腿 |

---

## 8. 风险清单

| 风险 | 影响 | 缓解 / 兜底 |
|---|---|---|
| **手感不达标**（最大） | 高 | 兜底 = **降级到路线 E**：GPU 合成 shader（brush_composite）两路线共享，只把「GPU dab 生成」换成「CPU 移植 libmypaint dab 生成」，其余不动。架构上必须把「dab 生成」做成可插拔子模块 |
| GPU 手感算法写不对（压力/间距偏差） | 高 | **借 libmypaint 公式做 GPU 化起步**：参数层公式照抄 ISC 的 libmypaint 默认曲线，host 对照测试 diff dab 输出（同路线 E §4.5 的对照方法） |
| 抖动/HSV 随机非确定 | 中 | PCG hash 确定性种子（§6.3），与执行顺序解耦 |
| 低端 Mali GPU compute 性能不足 | 中 | 包围盒 dispatch + tile 化；baseline 锁 Adreno（Snapdragon） |
| stamp 纹理往返导致多一次 barrier | 低 | 单图层融合成一个 pass（§2.1 融合优化），省 stamp tile 往返 |
| 参数层 GPU 化后调参变慢 | 中 | 手感参数先留 CPU（形态一）+ LUT 热加载，冻结后再搬 GPU（形态二） |
| 美术调参 AI 无法替代 | 高 | 数据化参数 + host 热调 harness + libmypaint 默认基线（§6.4） |

---

## 9. 分阶段实施计划（AI 执行视角）

> 核心原则：**先出「能画」的原型看手感，再谈全 GPU 终局**。把「手感验证」前置，避免在错误的算法上做 GPU 优化。

| 阶段 | 目标 | 关键动作 | 验收标准 |
|---|---|---|---|
| **B0 · 形状烘焙 spike** | 验证 SDF + hardness 的 stamp 生成 | host 跑 `bake_shape.comp` + `dab_raster.comp`，用固定 DabVertex 光栅化一张 stamp，dump 成 PNG 看形状 | PNG 显示圆形/方形 stamp，hardness 0.2/0.8 边缘软硬肉眼可辨 |
| **B1 · 融合合成原型（先看手感）** | 单图层「能画」，最快闭环 | 参数层先放 **CPU 轻量实现**（照抄 libmypaint 默认公式）；融合版 composite shader（形状数学内联 + over 合成）；host GLFW 鼠标输入 | host 鼠标画出一条连续笔迹，压力曲线/间距/抖动生效，肉眼手感「能用」 |
| **B2 · 参数层数据化 + 热调** | 美术可调参 | 压力曲线/间距/抖动改成 LUT + JSON 配置；ImGui 参数面板热加载 | 拖滑块实时改变笔触粗细/间距/抖动，无需重编译 |
| **B3 · 确定性随机 + 对照测试** | 保证可复现 | PCG hash 种子；同一点流 CPU vs GPU dab 参数 diff | diff 误差 < 1e-4，同一笔触两次绘制像素一致 |
| **B4 · 全 GPU 化（dab_generate.comp）** | 参数层搬进 GPU | 写 `dab_generate.comp`，形态二（Input→Render 两线程）；Android Vulkan 上屏 | Android 平板手写流畅，端到端 <30ms，compute <2ms（timestamp 实测） |
| **B5 · 手感精调 + 美术验收** | 对齐画世界PRO | 美术在真机反复调参；高速摄影测延迟；必要时降级路线 E 兜底 | 美术认可手感；§3.3 全部指标达标；产出「B vs E」结论 |

**快速验证路径**：B1 是最关键的「看手感」节点——**第 2 周内必须出 B1**。若 B1 手感与路线 E 对照差距大，立即止损走路线 E（合成 shader 复用，仅 dab 生成回退 CPU）。B4 的「参数层 GPU 化」是纯性能优化，可在手感锁定后再做，不阻塞手感验证。

---

## 10. 结论

**这条路线何时选**：当「性能天花板」是首要目标、且能接受「手感从移植公式起步 + 美术反复调参」的时间成本时选路线 B。它是**路线 E 的 GPU 化演进终局**，不是 E 的替代——两条路线共享同一个 premultiplied over 合成 shader 与同一套「参数层手感公式」，唯一差别是「dab 形状光栅化跑在 CPU（E）还是 GPU（B）」。

**与路线 E 的关系**（演进路径）：

```
【首选】路线 E：白盒移植 libmypaint dab 算法 → CPU dab + Vulkan 合成
        │  2 周内验证手感 + 无交叉编译 + 全掌控
        ▼
【中间】路线 B 形态一：参数层仍 CPU（复用 E 的公式），光栅化搬 GPU
        │  手感算法不变，只换光栅化后端 → 性能↑，风险低
        ▼
【终局】路线 B 形态二：参数层也搬进 dab_generate.comp → 全 GPU
        │  性能天花板，对齐画世界PRO shapeTex 模型
```

**一句话建议**：路线 B 的架构必须把「dab 生成」做成可插拔子模块（CPU 参数层 / GPU dab_generate 两实现），这样「手感不达标 → 降级路线 E」的兜底是**一次模块切换**而非重构。推荐**以 E 为起点、B 为终局**推进，沿途在每个里程碑用对照测试守住「手感数值一致」，把「从零自研手感」的原始风险转嫁成「移植公式 + 逐步 GPU 化」的渐进工程。

---

## 参考

- [Blender Sculpt 社区 GPU compute dab 方案（dab 坐标喂 compute、stroke 结束回读）](https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490/4859)
- [azul-core `brush_dab_coverage(t, hardness) = 1 - smoothstep(hardness, 1, t)`（SDF hardness 公式佐证）](https://docs.rs/azul-core/latest/azul_core/resources/fn.brush_dab_coverage.html)
- DGCPaint_技术规划.md §8 附录 A（画世界PRO RDC 抓帧分析：shapeTex/距离场/hardness/blit 模型）
- docs/调研/笔刷渲染技术路线评审.md（四维度框架与路线 E/B 关系）
- docs/调研/绘画内核功能清单.md（笔刷+渲染功能项）
