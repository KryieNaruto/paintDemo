# 路线 E · 白盒移植 libmypaint → 自研 C++ 笔刷内核 + Vulkan Compute —— 详细技术方案

> **日期**：2026-08-20
> **范围**：绘画内核（笔刷 dab 生成）+ 渲染合成，双平台（Android 平板主 / PC）
> **关联文档**：`DGCPaint_技术规划.md`（三插拔接口 + §3.3 性能指标）、`docs/调研/笔刷渲染技术路线评审.md`（四维度框架）、`docs/调研/绘画内核功能清单.md`（功能优先级）
> **关键约束**：开发全程由 AI 实现。方案刻意把「移植正确性」落到**可机器 diff 的对照测试**，把「主观手感」留在 libmypaint 已验证的算法与 `.myb` 预设里，避开 AI 最弱项（交叉编译、无文档遗留代码、主观调参）。

---

## 1. 技术路线概览

**一句话定位**：把 libmypaint 的 `mypaint_brush_stroke_to()` dab 生成算法**逐函数白盒移植成自研 C++（去 glib，ISC 许可允许直接抄）**，dab 生成跑 CPU，合成走 Vulkan Compute，输入走 Jetpack Ink 点流——用「成熟算法照抄 + 全自研可控代码 + 原生 GPU 合成」三条腿同时踩中 AI 强项。

```
┌──────────────────────────────────────────────────────────────────┐
│  UI（插拔）：Compose(Kotlin) / ImGui(C++)                          │
└───────────────┬──────────────────────────────────────────────────┘
                │ 引擎控制 API（设笔刷/颜色/undo）
┌───────────────▼──────────────────────────────────────────────────┐
│  engine（平台无关核心 · 3 线程模型 + 预测 + ring buffer）           │
│   Input Thread ──▶ Brush Thread ──▶ Render Thread                 │
└───────┬──────────────────────────────┬───────────────────────────┘
        │ IPaintKernel                 │ IRenderBackend
┌───────▼────────────────────────────┐ ┌▼───────────────────────────┐
│ 自研 C++ 笔刷内核（★ 白盒移植）      │ │ Vulkan Compute 合成后端      │
│  Brush / SensorPack / Rng /        │ │  canvas storage image       │
│  MybParser（nlohmann/json）        │ │  stamp 纹理池 + staging 环  │
│  移植 mypaint-brush.c dab 算法      │ │  brush_composite.comp       │
└───────┬────────────────────────────┘ └▲───────────────────────────┘
        └────────────── IPlatform（surface / input / lifecycle）────┐
                                                          ┌─────────▼─────────┐
                                                          │ Android / PC 平台层 │
                                                          └───────────────────┘
```

**与路线 A（链接 libmypaint）的本质区别**：dab 算法从「外部黑盒 C 库」变成「仓库内自研 C++」，三件事随之改变——(1) 无 glib/json-c 交叉编译；(2) 代码全掌控，后续可 SIMD / GPU 化；(3) 对照测试对象是「参数序列」而非「链接是否成功」。

---

## 2. 总体架构设计

### 2.1 模块划分与目录（相对 `DGCPaint_技术规划.md §2.2` 的三处改动）

```
DGCPaintPrototype/
├── core/                              # 不变：接口 + engine + 预测 + ring buffer
│   ├── interfaces/i_paint_kernel.h    # ★ 本节 §5 细化签名
│   ├── interfaces/i_render_backend.h
│   ├── interfaces/i_platform.h
│   ├── types.h                        # StrokePoint / BrushParams / StampData / StampShape
│   ├── engine.h/.cpp                  # 3 线程模型编排
│   ├── stroke_predictor.h/.cpp        # 速度外推预测（见 §3e）
│   └── ring_buffer.h                  # SPSC 无锁队列
├── kernels/brush/                     # ★ 改：kernels/mypaint/ → kernels/brush/（纯 C++ 自研）
│   ├── CMakeLists.txt
│   ├── brush.h/.cpp                   # Brush 类（移植 mypaint-brush.c）
│   ├── brush_settings.h/.cpp          # SettingId 枚举 + BrushSetting（base_value + 响应曲线）
│   ├── brush_mapping.h/.cpp           # 响应曲线求值（对照 settings_base_values_have_changed 的 m/q/gamma）
│   ├── sensors.h/.cpp                 # 传感器包（pressure gain / speed1/2 滤波 / tilt / direction）
│   ├── rng.h/.cpp                     # IRandomSource 抽象 + Mt19937Random（对照 rng.c）
│   ├── myb_parser.h/.cpp              # .myb JSON 解析（nlohmann/json）
│   ├── brush_kernel.h/.cpp            # IPaintKernel 实现（对照 mypaint_kernel）
│   └── color.h                        # HSV↔RGB、sRGB 线性化（对照 helpers 的 hsv_to_rgb_float）
├── render/vulkan/                     # 不变（vk_backend / vk_canvas / vk_composite）
├── platform/{android,pc}/             # 不变
├── ui/{android,pc}/                   # 不变
├── shaders/brush_composite.comp       # ★ 扩展：程序化圆形 + 纹理 stamp + lock_alpha
├── third_party/
│   └── nlohmann/json.hpp              # ★ 替换 json-c（header-only，MIT）
├── tests/
│   ├── test_brush_parity.cpp          # ★ 新增：对照 libmypaint host oracle 的 diff 测试
│   ├── test_rng.cpp                   # ★ 新增：RNG 重放 + 统计
│   └── test_predictor.cpp
└── tools/
    └── dump_myb.c                     # ★ 新增：libmypaint host oracle（仅测试用，不随产品）
```

**删除**（路线 A 遗留，路线 E 不需要）：`third_party/libmypaint/`、`third_party/json-c/`、`tools/build_mypaint_android.sh`、`tools/build_mypaint_host.sh`（交叉编译整条任务线消失）。

### 2.2 与三插拔接口的映射

| 插拔接口 | 实现（路线 E） | 换路线的含义 |
|---|---|---|
| `IPaintKernel` | `kernels/brush/brush_kernel.cpp`（自研 C++） | 后续 GPU 化 = 新增 `kernels/brush_gpu/` 实现同一接口 |
| `IRenderBackend` | `render/vulkan/vk_backend.cpp` | 换 Metal/bgfx = 新增实现 |
| `IPlatform` | `platform/android/` + `platform/pc/` | 双平台已插拔 |

### 2.3 三线程模型

| 线程 | 职责 | 输入 → 输出 | 同步 |
|---|---|---|---|
| **Input Thread**（Android 上即主线程回调） | Jetpack Ink 点流捕获 + 预测外推 | `MotionEvent` → `StrokePoint[]` | SPSC ring buffer ①（无锁） |
| **Brush Thread** | 自研 C++ 内核 `Brush::strokeTo` | `StrokePoint` → `StampData[]` | SPSC ring buffer ② |
| **Render Thread** | stamp 上传 + Vulkan compute 合成 + present | `StampData[]` → 画布 image → swapchain | 每帧 fence + semaphore |

三个线程**流水线解耦**：Render 线程合成第 N 帧时，Brush 线程已在生成第 N+1 帧的 stamp。关键约束——Brush Thread 是 CPU 瓶颈点（单 stamp <3ms），必须严格控制在 ring buffer 消费速率内（见 §6.2）。

---

## 3. 核心模块详细设计

### 3a. 笔刷内核（移植 dab 算法）

移植目标：libmypaint `mypaint-brush.c` 中 `mypaint_brush_stroke_to()` → `count_dabs_to()` → `update_states_and_setting_values()` → `prepare_and_draw_dab()` 这条主链（MVP 不移植 smudge / 湿边 / posterize，留接口占位）。以下签名**逐函数对照原 C 源码**。

#### 3a.1 传感器输入（对照 `inputs[]` 数组 + `INPUT()` 宏）

```cpp
// 输入传感器 ID（对照 MYPAINT_BRUSH_INPUT_*，MVP 子集 + 可扩展）
enum class Input : uint8_t {
  Pressure, Speed1, Speed2, Random, Stroke,
  Direction, DirectionAngle, TiltDeclination, TiltAscension,
  ViewZoom, AttackAngle, BrushRadius, Custom, Count
};

// 一次采样（对照 mypaint_brush_stroke_to 入参，现代签名含 viewzoom/rotation）
struct StrokeInput {
  float x = 0, y = 0;        // 画布坐标（px）
  float pressure = 0;        // 0..1
  float xtilt = 0, ytilt = 0;// 笔倾斜分量（-1..1），内核内转 declination/ascension
  float dtime = 0;           // 距上次采样秒数；内核内 clamp 到 ≥0.0001
  float viewzoom = 1.0f;     // 缩放（影响速度归一化，见下）
  float viewrotation = 0.0f;
  float barrelRotation = 0.0f;
};
```

**传感器滤波（`update_states_and_setting_values` 内，精确对照源码）**：

```cpp
// 压力（可加 gain）
INPUT(Pressure) = pressure * expf(PRESSURE_GAIN_LOG);

// 速度归一化（先除以 viewzoom，再低通）
norm_dx = (x - lastX_) / dtime * viewzoom;
norm_dy = (y - lastY_) / dtime * viewzoom;
norm_speed = hypotf(norm_dx, norm_dy);

// speed1/speed2 指数低通（对照 STATE(NORM_SPEED1_SLOW) 递推）
fac1 = 1.0f - expDecay(SPEED1_SLOWNESS, dtime);      // expDecay(T,t)=expf(-t/T)，T≤0.001 时返 0
STATE(NormSpeed1Slow) += (norm_speed - STATE(NormSpeed1Slow)) * fac1;
fac2 = 1.0f - expDecay(SPEED2_SLOWNESS, dtime);
STATE(NormSpeed2Slow) += (norm_speed - STATE(NormSpeed2Slow)) * fac2;

// speed → 传感器值（预计算 log-linear 映射，见 §3a.3）
INPUT(Speed1) = logf(speedMappingGamma[0] + STATE(NormSpeed1Slow)) * m0 + q0;
INPUT(Speed2) = logf(speedMappingGamma[1] + STATE(NormSpeed2Slow)) * m1 + q1;
```

tilt 转换（对照源码开头）：`tilt_ascension = DEGREES(atan2f(-xtilt, ytilt))`，`tilt_declination = 90 - rad2deg(atan2f(hypotf(xtilt,ytilt), 1))`。direction 用**距离低通**（非时间低通）：`fac = 1 - expDecay(expf(DIRECTION_FILTER*0.5)-1, step_in_dabtime)`，维护 180° 对称的 `DIRECTION_DX/DY` 与 360° 的 `DIRECTION_ANGLE_DX/DY` 两个向量。

#### 3a.2 dab 数量计算（对照 `count_dabs_to`，★ 源码确认为「求和」非「取最大」）

```cpp
// 返回值是「本段要画的 dab 个数（含小数）」
float Brush::countDabsTo(float x, float y, float dt) const {
  float dx = x - lastX_, dy = y - lastY_;
  // 椭圆 dab 时距离按椭圆比修正
  float dist = (actualEllipticalDabRatio_ > 1.0f)
             ? sqrtf(dx*dx + dy*dy * actualEllipticalDabRatio_)
             : hypotf(dx, dy);

  float res1 = dist / actualRadius_          * state(DabsPerActualRadius); // 按实际半径
  float res2 = dist / baseRadius_            * state(DabsPerBasicRadius);  // 按基础半径
  float res3 = dt                            * state(DabsPerSecond);       // 按时间
  float res4 = res1 + res2 + res3;           // ★ 求和，非 max
  if (std::isnan(res4) || res4 < 0.0f) res4 = 0.0f;
  return res4;
}
```

> 说明：`dabs_per_basic_radius` / `dabs_per_actual_radius` / `dabs_per_second` 三个设置**相加**决定每段 dab 数（DeepWiki 的「取最大值」是高层概述，与源码不符，本方案以源码 `res4 = res1+res2+res3` 为准）。`.myb` 里的 `spacing` 是 legacy 名称，翻译到 `dabs_per_basic_radius ≈ 1/spacing`。

主循环（对照 `mypaint_brush_stroke_to` 尾部）：

```cpp
void Brush::strokeTo(const StrokeInput& in, std::vector<StampData>& out) {
  if (resetRequested_ || in.dtime > maxDtime_) reset();
  float dabsMoved = state(PartialDabs);                 // 累计小数 dab 余量
  float dabsTodo  = countDabsTo(in.x, in.y, in.dtime);
  while (dabsMoved + dabsTodo >= 1.0f) {                // 对照 while(dabs_moved+dabs_todo>=1.0)
    float frac = (1.0f - dabsMoved) / dabsTodo;
    // 在 last→in 之间插值出 dab 位置，更新传感器状态
    updateStatesAndSettingValues(interp(last_, in, frac), in.dtime * frac);
    state(Flip) *= -1;                                  // 镜像抖动交替
    prepareAndDrawDab(in, out);                         // 产出 1 个 StampData
    rng_->nextUniform();                                // 对照 random_input = rng_double_next(rng)
    dabsMoved = 0.0f;
    dabsTodo = countDabsTo(in.x, in.y, in.dtime);       // 用剩余时间重算
  }
  state(PartialDabs) = dabsMoved + dabsTodo;            // 保留小数余量
  updateStatesAndSettingValues(in, in.dtime);           // 推进位置到真实点
}
```

#### 3a.3 设置映射（响应曲线，对照 `mapping` + `settings_base_values_have_changed`）

每个设置 = `base_value` + 若干输入响应曲线。曲线由控制点 `(x, y, slope)` 定义，求值 = 分段插值（libmypaint 用线性段 + 斜率平滑）。

```cpp
struct MappingPoint { float x, y, slope; };              // slope 默认 0 = 分段线性

struct BrushSetting {
  float baseValue = 0.0f;
  std::array<std::vector<MappingPoint>, (size_t)Input::Count> curves; // 每个 input 一条
};

// 求值：input 值 → 曲线输出（再叠加 base_value，具体叠加方式按设置类型）
float Brush::mappingValue(SettingId id, Input in) const;
```

**核心设置（MVP，对照 `MYPAINT_BRUSH_SETTING_*`）**：

| SettingId | 语义 | 映射公式（源码） |
|---|---|---|
| `RadiusLogarithmic` | 半径（对数域） | `base_radius = expf(baseValue)`；`radius_log = baseValue + 曲线(压力/速度/随机)`；`actual_radius = clamp(expf(radius_log), RADIUS_MIN, RADIUS_MAX)` |
| `Hardness` / `Softness` | 边缘软硬 / 外缘软化 | clamp 到 [0,1]，双旋钮定义 alpha 斜坡（见 §3d 栅格化） |
| `Opaque` + `OpaqueMultiply` + `OpaqueLinearize` | 不透明度 | `opaque = clamp(opaque * OpaqueMultiply, 0, 1)`；linearize 时 `dabs_per_pixel=(DabsPerActualRadius+DabsPerBasicRadius)*2`，`alpha_dab = 1 - powf(1-opaque, 1/dabs_per_pixel)` |
| `DabsPerBasicRadius` / `DabsPerActualRadius` / `DabsPerSecond` | dab 密度 | 见 §3a.2 |
| `ColorH/S/V` | 基础色（HSV） | `hsv_to_rgb_float()` 转 RGB |
| `ChangeColorH` / `ChangeColorHSV_S` / `ChangeColorV` / `ChangeColorL` / `ChangeColorHSL_S` | 颜色抖动 | HSV/HSL 分量随机偏移（用 `rng` 随机） |
| `Speed1Gamma` / `Speed2Gamma` / `Speed1Slowness` / `Speed2Slowness` | 速度滤波 | 预计算 `y=log(gamma+x)*m+q`（锚点 fix1_x=45, fix1_y=0.5, fix2_x=45, fix2_dy=0.015），低通见 §3a.1 |
| `PressureGainLog` | 压感增益 | `pressure * expf(gain)` |
| `DirectionFilter` | 方向滤波 | 距离低通 |
| `OffsetX/Y` `OffsetAngle*` `OffsetMultiplier` `OffsetBySpeed` `OffsetByRandom` | dab 偏移/抖动 | `directional_offsets()` + 高斯随机偏移 |
| `DabRatio` / `DabAngle` | 椭圆压扁 / 旋转 | 影响合成 shader 的 UV 变换 |
| `SnapToPixel` / `AntiAliasing` / `LockAlpha` | 像素对齐 / AA / 阿尔法锁 | 见 §3d |

#### 3a.4 dab 形状与颜色调制（对照 `prepare_and_draw_dab`）

```cpp
void Brush::prepareAndDrawDab(const StrokeInput& in, std::vector<StampData>& out) {
  float opaque = clamp(mapping(Opaque) * state(OpaqueMultiply), 0, 1);
  // ... linearize（见上表）
  float radius = clamp(expf(radiusLog), RADIUS_MIN, RADIUS_MAX);
  // 偏移：directional_offsets + 速度偏移 + 高斯随机偏移（rand_gauss）
  float h = base(ColorH), s = base(ColorS), v = base(ColorV);
  hsvToRgb(h, s, v);                    // 对照 hsv_to_rgb_float
  // 颜色动态：ChangeColorH/HSV_S/V（+ 可选 HSL L/S），linear 时先 sRGB 线性化再抖
  out.push_back(StampData{
    .x = x, .y = y, .radius = radius,
    .hardness = clamp(hardness), .softness = clamp(softness),
    .opacity = opaque, .r = r, .g = g, .b = b,
    .dabRatio = state(DabRatio), .dabAngle = state(DabAngle),
    .lockAlpha = state(LockAlpha), .shape = shape_   // 程序化圆 / 纹理 id
  });
}
```

**关键决策：dab 输出是「参数」不是「位图」**。libmypaint 库本身只产 dab 参数（x/y/radius/opacity/hardness/color…），栅格化发生在 surface 回调（在 mypaint GUI 的 `pixops.c`，**不在库内**）。因此 `StampData` 携带参数，栅格化交给渲染后端。这带来两个好处：(1) 对照测试 diff 的是「dab 参数序列」，精确、可机器比较；(2) stamp 位图可复用（同半径/硬度圆形只烘焙一次）。

#### 3a.5 RNG（对照 `rng.c`）

```cpp
class IRandomSource {                       // 可注入 → 对照测试重放
public:
  virtual ~IRandomSource() = default;
  virtual double nextUniform() = 0;         // [0,1) 对照 rng_double_next
  virtual double nextGauss() = 0;           // 标准正态 对照 rand_gauss
};
class Mt19937Random : public IRandomSource {
  std::mt19937 gen_;
  std::uniform_real_distribution<double> uni_{0.0, 1.0};
  std::normal_distribution<double> gauss_{0.0, 1.0};
public:
  explicit Mt19937Random(uint32_t seed) : gen_(seed) {}
  double nextUniform() override { return uni_(gen_); }
  double nextGauss() override { return gauss_(gen_); }
};
class ReplayRandom : public IRandomSource { // 测试用：回放 libmypaint 捕获的随机序列
  const std::vector<double>& seq_; size_t i_ = 0;
  double nextUniform() override { return seq_[i_++]; }
  double nextGauss() override { return seq_[i_++]; }
};
```

### 3b. 去 glib 替换清单

| glib 符号 | 在 libmypaint 中的用途 | std/C++ 替代 | 说明 |
|---|---|---|---|
| `gboolean` / `gdouble` / `gfloat` / `gint` / `guint` | 宏类型 | `bool` / `double` / `float` / `int32_t` / `uint32_t` | 纯类型别名，零成本 |
| `g_random_double()` / `GRand` | 随机数（json 与部分辅助） | `std::mt19937` + `std::uniform_real_distribution` | brush 主链用的是 libmypaint 自带 `rng.c`（`Rng` 结构体），一并移植或用 `Mt19937Random` 替换 |
| `g_random_double_range(a,b)` | 区间随机 | `std::uniform_real_distribution(a,b)` | |
| `GList` / `g_list_append` | 设置/输入链表 | `std::vector` | 遍历语义不变，去掉指针链 |
| `g_malloc` / `g_new` / `g_free` | 内存 | `new`/`delete`、`std::unique_ptr`、RAII | |
| `g_ascii_strtod` | 字符串→double（.myb 解析） | `std::strtod` | 行为一致（locale-independent） |
| `g_ascii_dtostr` / `g_ascii_formatd` | double→字符串 | `std::to_chars` / `snprintf` | 仅 .myb 导出用，MVP 可不做 |
| `g_assert` / `g_assert_not_reached` | 断言 | `assert` / `std::abort` | |
| `g_snprintf` | 格式化 | `std::snprintf` | |
| `G_IS_INFINITY` / `G_IS_NAN` | 浮点判断 | `std::isinf` / `std::isnan` | |
| `g_strdup` / `g_str_equal` | 字符串 | `std::string` / `operator==` | |
| `g_ascii_strcasecmp` | 忽略大小写比较 | `std::equal` + `std::tolower`（或手写） | .myb 输入名匹配用 |
| `g_ascii_strdown` | 转小写 | `std::transform` + `std::tolower` | |
| `G_MININT` 等常量 | 极值 | `std::numeric_limits<T>` | |
| `g_get_user_config_dir` | 配置路径 | 平台层 `IPlatform` 提供 | 文件读写非本原型范围 |

**结论**：glib 是「类型别名 + 随机数 + 链表 + 字符串」四类用途，全部有 1:1 的 std 替代，无交叉编译黑洞——因为**根本不链接 glib**。`mypaint-config.h` 的 `MYPAINT_CONFIG_USE_GLIB` 宏在自研代码中直接删除（不留 glib 头文件依赖，这是与路线 A「vendor config.h + 保留 glib 头」的最大区别）。

### 3c. .myb 预设解析器

`.myb` 是 JSON（version 3），用 header-only `nlohmann/json`（MIT）替代 json-c。

**JSON schema（精确）**：

```json
{
  "version": 3,
  "comment": "我的笔刷",
  "group": "basic",
  "parent_brush_name": "deevad/basic_round",   // 可选，继承父笔刷
  "settings": {
    "radius_logarithmic": {
      "base_value": 2.5,
      "inputs": {
        "pressure": [ [0.0, 0.2, 0.0], [0.5, 0.0, 0.0], [1.0, 1.0, 0.0] ]
      }
    },
    "hardness": { "base_value": 0.8 },
    "opaque":   { "base_value": 0.9, "inputs": { "pressure": [[0.0,0.0,0],[1.0,1.0,0]] } }
  }
}
```

**解析流程**：

```
1. nlohmann::json::parse(字符串)          # 异常时返回「非法预设」错误码
2. 校验 version == 3；读 group/comment/parent_brush_name
3. 若 parent_brush_name 非空 → 递归加载父预设（先父后子，子覆盖）
4. 遍历 settings：
   a. 名称（snake_case 字符串）→ SettingId 查表（无则跳过并记录 warning，向前兼容）
   b. 读 base_value
   c. 遍历 inputs：input 名 → Input 查表；每个控制点读 [x, y, slope]（slope 缺省 0）
   d. 调 brush.setBaseValue(id, v) + brush.setMappingPoint(id, input, i, x, y, slope)
5. 返回 BrushHandle（或 loadBrush 直接构造 Brush）
```

**设置名 ↔ 枚举映射表**用 `std::unordered_map<std::string, SettingId>` 静态初始化（AI 可机械化生成，~100 个设置名，MVP 先映射 §3a.3 的 ~25 个，其余进「未支持」集合并在加载时记录 warning，保证 `deevad/*.myb` 等主流预设可加载）。`mypaint-brushes` 预设目录作为 APK assets / PC 资源目录随包分发。

### 3d. Vulkan 合成管线

#### 3d.1 资源

| 资源 | 规格 | 说明 |
|---|---|---|
| **Canvas storage image** | `VK_FORMAT_R8G8B8A8_UNORM`（MVP）/ `R16G16B16A16_SFLOAT`（高精度预留），usage `STORAGE|SAMPLED|TRANSFER_DST`，布局**常驻 GENERAL** | 单图层一张，避免每帧 transition |
| **Stamp 纹理池** | `VK_FORMAT_R8_UNORM` 灰度 alpha（圆形 mask），`VK_FORMAT_R8G8B8A8_UNORM`（纹理笔刷），尺寸 64/128/256，usage `SAMPLED|TRANSFER_DST` | **圆形 mask 只烘焙一次**（按 `(radius 分桶, hardness 分桶)` 键），纹理笔刷每 brush 一张 |
| **Staging buffer 池** | `HOST_VISIBLE|HOST_COHERENT`，环形 N=4~8 slot，每 slot = 最大 stamp 字节 | `vkCmdCopyBufferToImage` 逐 stamp 批量上传 |
| **Descriptor** | set0：canvas(image) + stamp(sampler) | compute 用 |
| **同步** | semaphore(acquire→compute→draw→present) + fence | 见 `技术规划 §4.7` |

#### 3d.2 stamp 栅格化与 alpha（libmypaint 语义）

圆形 dab 的 alpha 是「硬度/软化双旋钮」径向斜坡（对照 mypaint `pixops` 的 piecewise-linear）：

```
dist = 距圆心归一化距离（0..1）
alpha(dist) = 1                                 当 dist <= hardness
            = (1 - dist) / (1 - hardness)       当 hardness < dist < 1   （hardness 斜坡）
外圈 softness：在 dist≈1 处再延一段软化斜坡到 0（softness>0 时）
```

两条实现路径（均写入 `brush_composite.comp`，用 push constant `mode` 切换）：

- **路径 A（GPU 程序化，MVP 首选）**：圆形 alpha 直接在 shader 内用 `dist = length(uv)` + `smoothstep` 计算，**不上传逐 dab 位图**，只传 `(radius, hardness, softness)`。零逐-dab 上传，stamp 上传耗时近乎 0，天然命中 P4<1ms。
- **路径 B（CPU 位图 / 纹理笔刷）**：纹理笔刷（`.myb` 带 stamp 纹理的）预烘焙 mask 进 stamp 纹理池，shader 采样 `u_Stamp` 并按 `dabRatio/dabAngle` 做 UV 旋转缩放。

#### 3d.3 `brush_composite.comp`（扩展版）

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform image2D u_Canvas;
layout(set = 0, binding = 1) uniform sampler2D  u_Stamp;   // 纹理笔刷用

layout(push_constant) uniform PC {
    vec2  stampCenter;   // 圆心（画布像素）
    float radius;        // 半径（px）
    float hardness;      // 0..1
    float softness;      // 0..1
    vec4  color;         // 预乘前基础色（straight RGB + a=opacity）
    float opacity;       // opaque
    float dabRatio;      // 椭圆压扁
    float dabAngle;      // 旋转
    uint  mode;          // 0=程序化圆, 1=纹理 stamp
    uint  lockAlpha;     // 阿尔法锁定
    vec2  stampSize;     // mode=1 时纹理尺寸
} pc;

void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_Canvas);
    if (c.x >= sz.x || c.y >= sz.y) return;

    // 椭圆变换：先旋转再压扁
    vec2 d = vec2(c) - pc.stampCenter;
    float ang = -pc.dabAngle;
    vec2 q = vec2(d.x*cos(ang)-d.y*sin(ang), d.x*sin(ang)+d.y*cos(ang));
    q.y /= max(pc.dabRatio, 0.001);

    float a = 0.0;
    if (pc.mode == 0u) {
        float r = length(q) / pc.radius;
        // 双旋钮：hardness 内实心，hardness→1 线性斜坡，softness 软化外缘
        float edge = mix(pc.hardness, 1.0, 1.0 - pc.softness);
        a = 1.0 - smoothstep(edge, 1.0, r);
    } else {
        vec2 uv = (q / pc.radius * 0.5 + 0.5);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return;
        a = texture(u_Stamp, uv).r;
    }

    a *= pc.opacity;
    if (a <= 0.001) return;

    vec4 canvas = imageLoad(u_Canvas, c);
    if (pc.lockAlpha == 1u && canvas.a <= 0.001) return;   // 阿尔法锁定：只在已画区写
    // 预乘 alpha over：dst = src + dst*(1-src.a)
    vec4 outC = vec4(pc.color.rgb * a + canvas.rgb * (1.0 - a),
                     a + canvas.a * (1.0 - a));
    imageStore(u_Canvas, c, outC);
}
```

#### 3d.4 批量 dispatch + 包围盒

- Brush Thread 产出的 `StampData[]` 进入 Render Thread 队列。
- Render Thread 每帧：把**本帧所有 stamp 合批**，统一在一个 command buffer 里录 N 次 dispatch（每次只 dispatch stamp 的**包围盒** `ceil(bbox/8)`，非全画布）。
- 屏障策略：一帧内 `1 次 TRANSFER barrier（上传）→ N 次 dispatch（无中间屏障，同 image 顺序访问）→ 1 次 COMPUTE→FRAGMENT barrier → present draw`。**不要每个 stamp 一个 barrier**（那是延迟大头）。
- 大 stamp（radius 大）与小 stamp 混批时按半径降序排，先大后小，减少 overdraw。
- 4K 大画布：`ceil(box/8)` 单 dispatch 上限控制在 ≤ 128×128 workgroup，超过则 tile 化（对应功能清单 #119）。

### 3e. 输入层（Jetpack Ink 点流 + 自研速度外推预测）

**接入**（对照 `技术规划 §4.6`）：只用 Ink 的 `strokes` 模块（纯数据层），`StrokeInputBatch` 提供含时间戳/压力/倾斜/方向的点流，**不引入 `ink-rendering`**（渲染由 Vulkan 接管）。预测层在 graphics-core 前缓冲渲染内部、未暴露独立 API，故采用**自研速度外推**。

```cpp
// core/stroke_predictor.h（~30 行，host 可单测）
struct StrokePredictor {
  // 状态：上一真实点 + 速度（像素/秒）
  float lastX = 0, lastY = 0, lastT = 0, vx = 0, vy = 0;
  // 预测：在当前时刻 T 外推到 T + horizon（默认 16ms ≈ 1 帧@60Hz，120Hz 用 8ms）
  StrokePoint predict(const StrokePoint& latest, uint64_t now_us, float horizon_ms);
};
```

**算法**：速度用最近 2~3 个真实点的一阶差分 + 指数平滑；预测点 `p = latest + v * horizon`；预测点 `is_predicted = true`。

**`isPredicted` 标记策略（Procreate 同款）**：

```
真实点到达 → 立即生成真实 stamp（is_predicted=false）
每帧（Input Thread 帧回调）→ 外推 1 个预测点 → 生成预测 stamp（is_predicted=true）
下一真实点到达 → 预测段作废：
   方案①（推荐 MVP）：预测 stamp 不写最终画布，只写「预测临时层」，真实点到达时整段重合成覆盖
   方案②（简化）：预测 stamp 也合成，但真实 stamp 到达后对该段重画一遍覆盖（预测只降低观感延迟）
```

两种方案的共同点：**预测 stamp 最终都会被真实 stamp 覆盖，不作为最终像素保留**。MVP 用方案②（省一张临时层），若出现拖影再切方案①。

---

## 4. 数据流（一条笔迹从触控到上屏，含耗时预算）

```
[触控笔按下] MotionEvent
  │  ① Ink strokes 捕获（压力/倾斜/时间戳）             ~1–3ms（PointerEventPass.Initial 直连）
  ▼
[Input Thread]
  │  ② 预测外推（速度外推 1 点）                        <0.1ms
  │  ③ push ring_buffer ①（StrokePoint，SPSC 无锁）    ~0.01ms
  ▼
[Brush Thread]
  │  ④ Brush::strokeTo → 生成 N 个 StampData            N × <3ms（P5；N 通常 1–4）
  │  ⑤ push ring_buffer ②（StampData）                 ~0.01ms
  ▼
[Render Thread]
  │  ⑥ stamp 上传：staging → vkCmdCopyBufferToImage     <1ms（P4；程序化圆则 ≈0）
  │  ⑦ brush_composite.comp 批量 dispatch + 包围盒      <2ms（P3）
  │  ⑧ present.vert/frag 画到 swapchain → vkQueuePresent 1 帧（60Hz=16.7ms / 120Hz=8.3ms）
  ▼
[屏幕]
```

**端到端预算拆解（<30ms 目标）**：①+② ~2ms → ③ 队列等待 ~0 → ④⑤ ~3–12ms（CPU 瓶颈，需控 N）→ ⑥ <1ms → ⑦ <2ms → ⑧ 8.3ms@120Hz。**关键**：三线程流水线把 ④⑤⑥⑦ 与 ⑧ 并行，实际端到端 ≈ 输入侧延迟 + max(brush+upload+compute, present) + 传输延迟。要稳 <30ms，④ 的「单次采样 N 个 stamp」必须上限受控（dab 密度设置 + §6.2 的冗余跳过）。

---

## 5. 关键接口设计（`core/interfaces/i_paint_kernel.h` + `core/types.h`）

```cpp
// ── types.h 共享类型（相对 技术规划 §4.0 扩展）──────────────────
struct StrokePoint {
  float x, y, pressure, tilt_x, tilt_y;
  uint64_t t_us;
  bool is_predicted;
};

struct BrushParams {
  float radius_log;       // 半径（对数域，对照 RadiusLogarithmic）
  float hardness;         // 0..1
  float opacity;          // 0..1
  float color_h, color_s, color_v;
  // 颜色/纹理按需扩
};

enum class StampShape : uint8_t { Circle = 0, Texture = 1 };  // 程序化圆 / 纹理

struct StampData {
  float x, y;             // 画布坐标（px）
  float radius;           // 实际半径（px）
  float hardness, softness;
  float opacity;          // 0..1
  float r, g, b;          // straight RGB（shader 内乘 alpha 转预乘）
  float dab_ratio;        // 椭圆压扁 ≤1
  float dab_angle;        // 旋转
  bool  lock_alpha;
  bool  is_predicted;     // ★ 预测点标记（预测 stamp 会被真实 stamp 覆盖）
  StampShape shape;
  uint32_t texture_id;    // shape==Texture 时指向 stamp 纹理池槽位
};

// ── IPaintKernel（插拔点①，自研 C++ 内核实现）──────────────────
using BrushHandle = uint32_t;   // 跨 JNI 用整型句柄，避免对象跨 ABI

class IPaintKernel {
public:
  virtual ~IPaintKernel() = default;

  // 从 BrushParams 创建（UI 面板直接调参）或从 .myb JSON 加载
  virtual BrushHandle createBrush(const BrushParams&) = 0;
  virtual BrushHandle loadBrush(const char* myb_json) = 0;   // .myb 预设（§3c）
  virtual void         destroyBrush(BrushHandle) = 0;

  virtual void beginStroke(BrushHandle, const StrokePoint&) = 0;       // 落笔：reset + 首点
  virtual std::vector<StampData> strokeTo(BrushHandle, const StrokePoint&) = 0; // 行笔
  virtual void endStroke(BrushHandle) = 0;                             // 抬笔：收尾
};
```

**与 `技术规划 §4.0` 的差异**：新增 `loadBrush`（.myb 预设是功能清单 #3「必须」）；`StampData` 从 4 字段扩展到完整 dab 参数 + `is_predicted` + `shape`（预测覆盖与程序化/纹理合成需要）。`IRenderBackend` / `IPlatform` 签名不变（沿用 `技术规划 §4.0`）。

---

## 6. 关键技术难点与解决方案

### 6.1 移植正确性（★ 最大风险）—— 对照测试方案

**问题**：浮点精度、随机数序列、设置映射细节任何一处偏差都会导致「手感不同」。

**方案：host oracle 对照 diff dab 参数序列**（不做像素 diff，因为 libmypaint 库不产像素）：

```
测试期引入 libmypaint host 版（Linux，带 glib 编译容易）作为「oracle」，只用于测试，不随产品：
  1. tools/dump_myb.c：自定义 MyPaintSurface（draw_dab 回调打印 x/y/radius/opacity/hardness/softness/color/dab_ratio/dab_angle）
     + 打印每次 rng_double_next/rand_gauss 的返回值序列
  2. 固定输入脚本：构造一条确定性 stroke（固定点序列 + 压力 + dtime），跑 libmypaint，dump 出：
     a. dab 参数序列 JSON
     b. 随机数序列
  3. 自研 C++ 内核读同一输入，用 ReplayRandom 回放 (b) 的随机序列，产出 dab 参数序列
  4. 逐字段 diff：浮点容差 1e-5（相对），全部通过 = 移植正确
```

- **为何可行**：dab 生成是**确定性算法**（输入 + RNG 序列 → 输出），注入相同 RNG 后其余部分应 bit 级一致。RNG 差异被 `ReplayRandom` 隔离，不污染算法对比。
- **测试形态**：`tests/test_brush_parity.cpp`（CTest，host 跑，CI 可自动回归）；随机序列独立用 `test_rng.cpp` 验证 `Mt19937Random` 的统计均匀性/无偏。
- **渐近验收**：先「单 dab」（固定输入 → 1 dab）→「单段」（countDabsTo 触发多 dab）→「整条 stroke」（含 reset/追赶/锥形）→「多设置」（radius_log + color 抖动 + offset）。

### 6.2 CPU dab 性能（P5 <3ms / stamp）

**问题**：Brush Thread 是 CPU 瓶颈。快速笔触一次采样可能触发 4+ dab（`dabs_per_actual_radius` 默认 4），每 dab 含 hsv 转换 + 响应曲线求值 + 随机数。

**方案**：
1. **预计算缓存**：响应曲线求值结果、`expf(radius_log)`、HSV→RGB 转换按输入量化缓存（采样 256 级 LUT）。
2. **冗余 dab 跳过**：相邻 dab 距离 < 0.5px 且半径/透明度/颜色几乎不变时合并（对齐「追赶/锥形」功能 #24/#25）。
3. **SIMD**：HSV→RGB、sRGB 线性化可 `-march` / NEON 向量化（自研代码，改得动）。
4. **帧预算上限**：每采样 cap N（如 ≤8），超出则降 dab 密度（丢的是细腻度，保的是帧率）。
5. **终局**：见 §6.5 GPU 化，dab 生成搬进 compute，CPU 归零。

### 6.3 随机数确定性

**问题**：`std::mt19937` 与 libmypaint 自带 `rng.c` 算法不同，同一 seed 产出不同序列。

**方案**：`IRandomSource` 接口把「随机源」与「算法」解耦（§3a.5）——生产用 `Mt19937Random`（可 seed 复现、统计均匀），测试用 `ReplayRandom`（回放 oracle 序列）。「同 seed 复现同一笔迹」由 mt19937 的确定性保证；「与 libmypaint 逐位一致」由 ReplayRandom 保证，二者分离，互不冲突。

### 6.4 预测点重合成（拖影风险）

见 §3e 的 `isPredicted` 策略：预测 stamp 与真实 stamp 都在画布上 over 合成，但预测 stamp 位置由外推得到、真实点到达时**对预测段整段重画覆盖**。为支持「重画覆盖」，Brush Thread 需按段保留最近 K 个真实 stamp 的可重放记录（环形历史缓冲，真实点到达时重发该段）。若出现拖影，切方案①（预测临时层，真实点到达后 discard 整层重合成）。

### 6.5 后续 GPU 化路径（E → B 的平滑演进）

**为什么能平滑**：dab 算法代码是**自研**（ISC 允许改），把 `Brush::strokeTo` 的每 dab 计算搬进 compute shader 是「改自己代码」而非「换依赖」。

```
路线 E（本方案）：
  Brush Thread: countDabsTo + mapping + 颜色调制 → StampData 参数（CPU）
  Render Thread: brush_composite.comp 程序化圆/纹理 over（GPU）

GPU 化（阶段演进，可选）：
  ① 把「dab 形状栅格化」先搬到 GPU（§3d 路径 A 已是程序化，本步完成大半）
  ② 把「响应曲线求值 + 颜色调制」搬进 compute：stamp 参数改由 GPU 从点流算
  ③ 把「countDabsTo + 传感器滤波」搬进 compute：点流直接进 GPU buffer
  ④ 全 GPU 形态 ≈ 路线 B，但算法语义与 .myb 兼容性不变（同一套公式）
```

**触发条件**：CPU dab 成为帧率瓶颈（P5 达标但 Brush Thread 忙不过来），或 4K 大画布 + 高 dab 密度压测不过 P6。

---

## 7. 四维度评审（5 分制）

| 维度（权重） | 得分 | 一句理由 |
|---|---|---|
| **复杂度**（25%） | **3.5** | 无 glib/json-c 交叉编译，移植是 AI 最强的「写清晰代码 + 原码对照 + 机器 diff」；扣分在需理解 C 算法与浮点细节 |
| **性能**（30%） | **4.5** | CPU dab + Vulkan compute 与路线 A 同级，且代码自研可 SIMD/GPU 化，逼近 5 的上限明确 |
| **可控性**（25%） | **5** | 算法 100% 自研、ISC 允许任意改，改笔刷/加类型/调混合/搬 GPU 全自由 |
| **时间**（20%） | **3.5** | 移植 1–2 周，有源码 + DeepWiki + hokusai/rustport 先例，比交叉编译与从零都快 |

**加权总分 ≈ 4.18**（与评审结论一致，当前最优）。

---

## 8. 风险清单与缓解（含切换闸门）

| # | 风险 | 影响 | 缓解 | 切换闸门（触发则换路线） |
|---|---|---|---|---|
| R1 | 移植正确性（浮点/随机/映射偏差） | 中 | §6.1 host oracle 对照 diff，CI 自动回归 | 对照测试连续 2 周无法收敛到 1e-5 → 降级回路线 A（链接 libmypaint） |
| R2 | CPU dab 性能不达 P5/P6 | 中 | §6.2 LUT/SIMD/合并/帧预算上限 | 压测 4K + 高密度持续 5min 掉帧或单 stamp >3ms 且优化无效 → 提前 GPU 化（§6.5） |
| R3 | 随机数序列与 libmypaint 不一致 | 低 | §6.3 IRandomSource 注入 + ReplayRandom | 若必须逐位复刻 → 直接移植 libmypaint `rng.c`（几十行，算法公开） |
| R4 | `.myb` 解析不兼容主流预设 | 低 | nlohmann/json + 名称查表 + 未支持设置 warning 向前兼容 | 主流 `deevad/*.myb` 加载失败 → 用 libmypaint 的 `mypaint_brush_from_json_string` 作 oracle 对照解析结果 |
| R5 | 预测点拖影 | 中 | §3e/§6.4 isPredicted 重合成，先简化后临时层 | 拖影肉眼可见且方案②无效 → 方案①预测临时层 |
| R6 | Compute 在低端 Mali GPU 性能不足 | 中 | 包围盒 + tile + 程序化圆（零上传）；baseline 锁 Adreno | Mali 端 <2ms 不达标 → 降为 fragment blit（对齐画世界PRO OpenGL 模型）或锁高端设备 |
| R7 | TextureView 多一次合成拷贝导致延迟超标 | 中 | 沿用 `技术规划 §4.3` 评估 | 端到端 >30ms 且定位在合成拷贝 → SurfaceView 独立 surface |

---

## 9. 分阶段实施计划（AI 执行视角，每阶段带验收标准）

| 阶段 | 内容 | 验收标准（可机器判定） |
|---|---|---|
| **E0 · 移植 spike** | 移植 `countDabsTo` + 传感器滤波 + `prepareAndDrawDab` 圆形 dab（去 glib）；搭建 host oracle `dump_myb.c` + `ReplayRandom` | 单 dab / 单段 / 整条 stroke 三级对照测试 diff ≤1e-5 全绿（`ctest`） |
| **E1 · 接口层 + 骨架** | 三插拔接口 + `StampData`/`StrokeInput` 完整类型 + CMake 三 preset | host-windows / host-linux / android-arm64 三 preset 配置通过，PC 可执行 + Android `.so` 空壳编译 |
| **E2 · 渲染后端** | `vk_backend/vk_canvas/vk_composite` + staging 环 + `brush_composite.comp`（程序化圆 + 纹理 + lock_alpha） | offscreen 固定 StampData 合成出笔刷痕迹，`vkCmdDispatch` 用 Vulkan timestamp query 测得 <2ms |
| **E3 · 笔刷内核实现 IPaintKernel** | `brush_kernel` 实现 `createBrush/loadBrush/beginStroke/strokeTo/endStroke` + `myb_parser`（nlohmann/json）+ 打包 mypaint-brushes assets | JNI 调 `loadBrush("deevad/basic_round.myb")` → `strokeTo` 产出 StampData → 送入 vk_composite 可画（offscreen） |
| **E4 · 平台层 + UI** | Android TextureView/ANativeWindow/swapchain + Compose UI；PC GLFW + ImGui | 双平台 Vulkan 画布上屏，UI 可切换笔刷/颜色 |
| **E5 · 输入集成** | Ink strokes 点流 + `stroke_predictor` 速度外推 + isPredicted 重合成 + PC 鼠标/数位笔 | 双平台笔迹跟随良好，高速摄影测无明显可感知延迟，无拖影 |
| **E6 · 全链路压测 + 性能报告** | 大画布/快速笔触压测 + AGI/RenderDoc/高速摄影测 §3.3 全部指标 | P1<30ms、P2 60/120fps、P3<2ms、P4<1ms、P5<3ms、P6/P7 通过，产出性能报告 |

**里程碑**：M2（offscreen 可画，E3 末）可先于 M1（双平台上屏，E4 末）；M3（全链路 + 报告，E6 末）。

---

## 10. 结论

**可行，且是当前最优**。路线 E 把 libmypaint 的 dab 算法白盒移植成自研 C++（去 glib，ISC 许可合规），合成走 Vulkan Compute——算法成熟度（Krita/MyPaint 验证过的手感）、实现可控性（全自研可优化/GPU 化）、AI 执行友好度（写清晰代码 + 原码对照 + 机器 diff，避开交叉编译）三者兼得，加权 4.18 高于「链接 libmypaint」（3.33）与「纯自研 GPU」（4.03）。

**何时选它**：当前（原型期，需快速验证「libmypaint 算法 + Ink + Vulkan compute 能否达 Procreate 级手感」这一核心技术假设）即选它；它同时是通往全 GPU 的最稳路径。

**如何演进到全 GPU**：dab 算法代码自研（ISC 可改），把 `countDabsTo` → 传感器滤波 → 响应曲线 → 颜色调制逐段搬进 compute shader，最终达全 GPU 形态（≈路线 B），`.myb` 兼容性与算法语义全程不变——这是「改自己代码」的平滑演进，而非「换依赖」的断崖迁移。

---

> **文档版本**：v1.0
> **编制依据**：`DGCPaint_技术规划.md`、`docs/调研/笔刷渲染技术路线评审.md`、`docs/调研/绘画内核功能清单.md`；libmypaint `mypaint-brush.c` 源码（`count_dabs_to` / `update_states_and_setting_values` / `prepare_and_draw_dab` 精确公式）、DeepWiki Brush Engine、hokusai/rustport 先例
> **关键事实订正**：dab 数量为三设置**求和**（`res4 = res1+res2+res3`），非「取最大值」（以源码为准）
> **输出目录**：docs/调研/
