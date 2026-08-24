# DGCamp Paint SDK（paintDemo）

本仓库是绘画内核 **SDK 基座**：产出 `libdgc_paint`（`.so` / `.a` / `.dll` / `.dylib`）和唯一公开头 `sdk_api/dgc_paint_c_api.h`。

**不含 UI 消费者。** Android Compose、PC ImGui/GLFW、JNI 分别在独立仓库，通过 git submodule 引用本库（路径固定 `sdk/`）。拓扑见 [`docs/git/README.md`](docs/git/README.md)。

技术验证目标：libmypaint 算法（白盒移植）+ 低延迟输入（由消费者送入 C API）+ Vulkan Compute 合成，能否达到 Procreate 级手感。主线 **路线 E**，终局 **路线 B**。详见 [`docs/调研/路线整理.md`](docs/调研/路线整理.md)（含 §7 C API）。

| 路线 | 加权分 | 定位 |
|------|--------|------|
| **E 白盒移植+Vulkan** | **4.18** | **SDK 内部默认实现（后续任务）** |
| B 自研GPU+Vulkan | 4.03 | 终局形态 |
| C libmypaint+Skia | 3.65 | 快速验证垫脚石 |
| D 自研+bgfx | 3.55 | iOS 扩展储备 |
| A 链接libmypaint | 3.33 | 对照基准/兜底 |

换路线 = 换 SDK **内部** `kernels/` / `render/`；**C API 不变**，消费者代码零改动。

## 文档索引

| 文档 | 说明 |
|------|------|
| [DGCPaint_技术规划.md](DGCPaint_技术规划.md) | 主技术规划（架构 + 路线） |
| [docs/tasks/任务线.md](docs/tasks/任务线.md) | 任务状态 SOT（脚本申领） |
| [docs/tasks/detail/环境搭建与项目骨架.md](docs/tasks/detail/环境搭建与项目骨架.md) | E0 线任务书：环境说明书 + 一键脚本 + 项目目录 |
| [docs/tasks/detail/共同基座.md](docs/tasks/detail/共同基座.md) | C API + 内部 core 任务书 |
| `docs/env/env-setup.md` | 环境搭建说明书（E0-1 产出） |
| `scripts/setup-env.sh` | 换机一键脚本（E0-2 产出） |
| [docs/调研/路线整理.md](docs/调研/路线整理.md) | 路线分组 + §7 SDK/C API |
| [docs/调研/技术路线评审汇总.md](docs/调研/技术路线评审汇总.md) | 5 路线评审结论 |
| [docs/调研/路线E-白盒移植libmypaint-技术方案.md](docs/调研/路线E-白盒移植libmypaint-技术方案.md) | 路线 E 详细技术方案 |
| `.claude/skills/paint-dev/SKILL.md` | 5 阶段开发流水线编排 |

## 消费者如何引用

```bash
# 先自行创建 KryieNaruto/paint-android 或 paint-pc 空库，再：
git clone git@github.com:KryieNaruto/paint-android.git
cd paint-android
/path/to/paintDemo/scripts/bootstrap-consumer.sh
# 钉 tag 后提交 .gitmodules
```

CMake：`add_subdirectory(sdk)`，链接 `dgc_paint`，只 `#include "dgc_paint_c_api.h"`。模板：[docs/git/templates/](docs/git/templates/)。

待建远端：`paint-android`、`paint-pc`（本环境不能代建 GitHub 仓库）。

---

## 换设备搭建 SDK 开发环境

> 目标：clone 本仓库后能编 `libdgc_paint` 并跑 host ctest。GLFW / Compose / JDK **不是** SDK 必需（属于消费者）。

### 1. Clone

```bash
git clone git@github.com:KryieNaruto/paintDemo.git
cd paintDemo
```

将本机 SSH 公钥加到 GitHub [Settings → SSH and GPG keys](https://github.com/settings/keys)。

### 2. Git 身份

```bash
git config user.name  "<你的名字>"
git config user.email "<你的邮箱>"
```

### 3. 探测 host 工具链

`scripts/setup-env.sh` 由 **E0-2** 产出，未认领前不存在。可先手工探测：

```bash
which cmake ninja g++   # 或 gcc / clang++
```

cmake ≥ 3.22、ninja、C++ 编译器为 host 必需；NDK / Vulkan 未装仅警告（Null 后端不硬依赖）。完整版本下限见 **E0-1** 产出的 `docs/env/env-setup.md`。

### 4. 验证任务线

```bash
python3 .exec/taskline.py status
```

应显示 **15** 条任务。首波可领：`E0-1`、`G0-1`。

### 5. 安装 SDK 工具链

环境搭建由 **E0 线**覆盖（见 [`docs/tasks/detail/环境搭建与项目骨架.md`](docs/tasks/detail/环境搭建与项目骨架.md)）：
- **E0-1** 产出环境搭建说明书 `docs/env/env-setup.md`（Linux + Windows 步骤、版本下限）。
- **E0-2** 产出换机一键脚本 `scripts/setup-env.sh`（探测 + 补缺安装）。

尚未认领这些任务时，工具链可能未就绪；换机后跑 E0-2 脚本或照 E0-1 说明书手动搭建。

不要把 `libglfw3-dev` 当作本仓库必需。

### 6. 验证构建（项目骨架 E0-3 落地后）

```bash
cmake --preset host-linux
cmake --build --preset host-linux
ctest --test-dir build/host-linux --output-on-failure

# NDK 已配置时
cmake --preset android-arm64
cmake --build --preset android-arm64
```

`CMakePresets.json` 由 **E0-3**（基础项目目录建设）产出；尚未实现时上述 preset 还不存在。

### 7. 开工

任意终端输入 **「开工 / 领任务」** 或 **`/paint-dev`**：

```
申领 → 计划 → 执行 → 测试 → 收尾 + 评审
```

---

## 目录结构（本仓库）

| 路径 | 说明 |
|------|------|
| `sdk_api/` | 唯一对外 C ABI（`dgc_paint_c_api.h`） |
| `core/` | 内部：类型、插拔接口、engine、ring buffer、predictor |
| `kernels/` | L5 内部插拔（占位；默认 Null） |
| `render/` | L4 内部插拔（占位；默认 Null） |
| `shaders/` | 后续 Vulkan/bgfx shader |
| `tests/` | host ctest（C API / engine，headless） |
| `docs/tasks/` | 任务线 SOT + 任务书 |
| `docs/git/` | 消费者 submodule 约定与模板（**G0-1** 产出） |
| `docs/env/` | 环境搭建说明书 `env-setup.md`（**E0-1** 产出） |
| `docs/调研/` | 路线与评审 |
| `scripts/` | `setup-env.sh`（**E0-2** 产出）等 |
| `.exec/taskline.py` | 任务申领脚本 |

**不在本仓库**：`ui/`、`platform/`、`app/`。

## 路线切换（SDK 内部）

```cmake
DGCPAIN_KERNEL_BRUSH=ON    # 路线 E（默认）
DGCPAIN_KERNEL_MYPAINT=ON  # 路线 A
DGCPAIN_KERNEL_GPU=ON      # 路线 B
DGCPAIN_RENDER_VULKAN=ON   # A/E/B
DGCPAIN_RENDER_SKIA=ON     # C
DGCPAIN_RENDER_BGFX=ON     # D
```

## 性能指标（目标）

| 指标 | 目标 | 测量方法 |
|------|------|---------|
| 端到端延迟（触控→显示） | < 30ms | 高速摄影 ≥240fps |
| 绘制中帧率 | ≥ 60fps（120Hz 屏 120fps） | AGI / Choreographer |
| Compute 合成耗时 | < 2ms | Vulkan timestamp query |
| Stamp 上传耗时 | < 1ms | CPU 打点 |
| 自研内核单 stamp | < 3ms | CPU 打点 |

端到端延迟在消费者 + SDK 联调时测；SDK 内先打 compute/stamp 点。

## 许可

libmypaint 算法（ISC 许可）白盒移植为自研 C++ 代码，保留 ISC 版权声明。其余代码为 DGCamp Paint 项目所有。
