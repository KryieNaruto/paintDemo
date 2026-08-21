# DGCamp Paint 原型

技术验证型原型：验证 **libmypaint 笔刷算法（白盒移植自研 C++） + Jetpack Ink 低延迟输入 + Vulkan Compute Shader GPU 合成** 三者组合能否达到 Procreate 级别的绘画手感。目标平台 **Android 平板 + PC**。

## 技术全景

```
┌───────────────────────────────────────────────────────────────┐
│  UI 层（插拔）：Compose(Kotlin) / ImGui(C++)                   │
├───────────────────────────────────────────────────────────────┤
│  引擎核心（3 线程）：Input → Brush(自研CPU) → Render(Vulkan)    │
├──────────────────┬────────────────────────────────────────────┤
│  绘画内核（插拔）  │  渲染后端（插拔）                            │
│  kernels/brush/  │  render/vulkan/                             │
│  （自研 C++）     │  （Vulkan Compute）                         │
├──────────────────┴────────────────────────────────────────────┤
│  平台层（插拔）：Android(ANativeWindow) / PC(GLFW)              │
└───────────────────────────────────────────────────────────────┘
```

**5 条技术路线评审结论**：主线走 **路线 E**（白盒移植 libmypaint → 自研 C++ 笔刷内核 + Vulkan Compute），终局演进到路线 B（全 GPU 内核）。详见 `docs/调研/路线整理.md`。

| 路线 | 加权分 | 定位 |
|------|--------|------|
| **E 白盒移植+Vulkan** | **4.18** | **主线起点（当前开发）** |
| B 自研GPU+Vulkan | 4.03 | 终局形态 |
| C libmypaint+Skia | 3.65 | 快速验证垫脚石 |
| D 自研+bgfx | 3.55 | iOS 扩展储备 |
| A 链接libmypaint | 3.33 | 对照基准/兜底 |

## 文档索引

| 文档 | 说明 |
|------|------|
| [DGCPaint_技术规划.md](DGCPaint_技术规划.md) | 主技术规划（v2.0 · 架构 + 路线 + 任务线） |
| [docs/任务线.md](docs/任务线.md) | 任务状态与进度（SOT） |
| [docs/调研/路线整理.md](docs/调研/路线整理.md) | 路线 A–E 分组与中间层复用分析 |
| [docs/调研/技术路线评审汇总.md](docs/调研/技术路线评审汇总.md) | 5 路线评审结论 |
| [docs/调研/路线E-白盒移植libmypaint-技术方案.md](docs/调研/路线E-白盒移植libmypaint-技术方案.md) | 路线 E 详细技术方案 |
| [docs/调研/绘画内核功能清单.md](docs/调研/绘画内核功能清单.md) | 功能优先级（对标 CSP/Procreate） |
| `.claude/skills/paint-dev/SKILL.md` | 5 阶段开发流水线编排 |

---

## 换设备快速搭建开发环境

> 目标：在新机器上从 clone 到能开工，**30 分钟内**完成。

### 1. Clone 仓库

```bash
git clone git@github.com:KryieNaruto/paintDemo.git
cd paintDemo
```

> 需要将新设备的 SSH 公钥（`~/.ssh/id_ed25519.pub`）添加到 GitHub 账号 [Settings → SSH and GPG keys](https://github.com/settings/keys)。

### 2. 配置 Git 身份

```bash
git config user.name  "<你的名字>"
git config user.email "<你的邮箱>"
```

### 3. 验证任务线系统

任务线系统自包含在仓库中，随 clone 一起到达：

```bash
python3 .exec/taskline.py status
```

应显示 22 条任务，首波可领任务（T0-1、T0-2、T1-1 等）。

### 4. 安装开发工具

#### 4.1 Android Studio（主 IDE，产出 .so + APK）

| 组件 | 版本要求 | 说明 |
|------|---------|------|
| Android Studio | Ladybug (2024.3) 及以上 | 最新稳定版 |
| Android SDK | compileSdk 35+ | SDK Manager 安装 |
| Android NDK | r27+（建议 r28） | SDK Manager 安装 |
| CMake | 3.22+（NDK 内置） | SDK Manager 安装 |
| JDK | 21 | Android Studio 自带或单独安装 |

**SDK Manager 必需组件**：
- `platform-tools`、`platforms;android-35`
- `ndk;28.x`、`cmake;3.31.x`
- `build-tools;35.x`

**环境变量**（建议写入 `~/.bashrc` 或 `~/.zshrc`）：
```bash
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/28.x.x
export PATH=$ANDROID_HOME/platform-tools:$PATH
```

#### 4.2 VS2022+（Windows 可选，host 快速验证 C++）

用于秒级编译 C++ 核心、跑单元测试，**不产出 Android .so**。

- 工作负载：「使用 C++ 的桌面开发」（MSVC + CMake 集成）
- 单个组件：「C++ CMake tools for Windows」
- 额外：[LunarG Vulkan SDK](https://vulkan.lunarg.com/)（提供 `vulkan-1.lib`、`glslc`）

#### 4.3 Linux 开发机（host 快速验证）

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake ninja-build libglfw3-dev libvulkan-dev

# 如需对照测试 oracle（host 版 libmypaint）
sudo apt install libglib2.0-dev libjson-c-dev
```

#### 4.4 物理测试平板

| 推荐 | 说明 |
|------|------|
| Galaxy Tab S9+ / S10+ | S Pen 原生压力+tilt，Vulkan 1.3，≥120Hz |
| 小米平板 6 Pro / 7 Pro | 性价比高，Vulkan 1.1+ |

**必备配置**：
- 开发者模式 + USB 调试（或 `adb pair` 无线调试）
- 关闭系统省电模式的 GPU 降频
- 固定横屏，关闭多窗口

### 5. 验证构建

```bash
# Host Linux 构建（路线 E）
cmake --preset host-linux
cmake --build --preset host-linux

# Android 构建
cmake --preset android-arm64
cmake --build --preset android-arm64
```

### 6. 开工

任意终端输入 **「开工 / 领任务」** 或 **`/paint-dev`**，进入 5 阶段流水线：

```
申领 → 计划 → 执行 → 测试 → 收尾 + 评审
```

---

## 目录结构

| 路径 | 说明 |
|------|------|
| `core/` | 平台无关核心（接口 + engine + ring buffer + 预测） |
| `kernels/brush/` | 路线 E：自研 C++ 笔刷内核 |
| `kernels/mypaint/` | 路线 A：链接 libmypaint（对照测试用） |
| `render/vulkan/` | 路线 A/E/B：Vulkan Compute 渲染后端 |
| `render/skia/` | 路线 C：Skia 渲染后端（备选） |
| `render/bgfx/` | 路线 D：bgfx 渲染后端（储备） |
| `platform/android/` | Android 平台层（ANativeWindow + JNI） |
| `platform/pc/` | PC 平台层（GLFW 窗口） |
| `ui/android/` | Compose UI（Kotlin） |
| `ui/pc/` | ImGui UI（C++） |
| `shaders/` | GLSL compute shader 源码 |
| `app/` | Android 工程（build.gradle.kts + Activity） |
| `tests/` | CTest host 单元测试 |
| `docs/` | 技术规划 + 调研 + 任务线 |
| `.claude/` | Claude Code 编排（skills + agents） |
| `.exec/taskline.py` | 任务线查询/认领/收尾脚本 |

---

## 路线切换

通过 CMake 选项切换不同路线组合：

```cmake
# 绘画内核（L5）
DGCPAIN_KERNEL_BRUSH=ON   # 路线 E：自研 C++ 内核（主线，默认）
DGCPAIN_KERNEL_MYPAINT=ON # 路线 A：链接 libmypaint
DGCPAIN_KERNEL_GPU=ON     # 路线 B：GPU compute 内核

# 渲染后端（L4）
DGCPAIN_RENDER_VULKAN=ON  # 路线 A/E/B：Vulkan Compute（默认）
DGCPAIN_RENDER_SKIA=ON    # 路线 C：Skia GLES3
DGCPAIN_RENDER_BGFX=ON    # 路线 D：bgfx 跨 API
```

路线切换**只换对应目录**，其余层（core/engine/输入/平台/UI）完全不动。

---

## 性能指标（目标）

| 指标 | 目标 | 测量方法 |
|------|------|---------|
| 端到端延迟（触控→显示） | < 30ms | 高速摄影 ≥240fps |
| 绘制中帧率 | ≥ 60fps（120Hz 屏 120fps） | AGI / Choreographer |
| Compute 合成耗时 | < 2ms | Vulkan timestamp query |
| Stamp 上传耗时 | < 1ms | CPU 打点 |
| 自研内核单 stamp | < 3ms | CPU 打点 |

---

## 许可

libmypaint 算法（ISC 许可）白盒移植为自研 C++ 代码，保留 ISC 版权声明。其余代码为 DGCamp Paint 项目所有。