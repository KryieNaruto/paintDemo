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
| [docs/tasks/detail/开发与测试环境.md](docs/tasks/detail/开发与测试环境.md) | SDK 工具链任务书 |
| [docs/tasks/detail/共同基座.md](docs/tasks/detail/共同基座.md) | C API + 内部 core 任务书 |
| [docs/git/README.md](docs/git/README.md) | 消费者 submodule 约定 |
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

### 3. 验证任务线

```bash
python3 .exec/taskline.py status
```

应显示 **14** 条任务。首波可领：`E0-1`、`E0-2`、`E0-3`、`E0-4`、`G0-1`。

### 4. 安装 SDK 工具链

#### 4.1 Linux host

```bash
sudo apt install build-essential cmake ninja-build libvulkan-dev
```

不要把 `libglfw3-dev` 当作本仓库必需。探测脚本见任务 E0-1（`scripts/check-env.sh`）。

#### 4.2 命令行 NDK（编 Android `.so`）

| 组件 | 版本 |
|------|------|
| Android NDK | r27+（建议 r28） |
| CMake | 3.22+ |
| ANDROID_NDK_HOME | 指向 NDK 根目录 |

Android Studio / Gradle / Compose / JDK 21 属于 **paint-android** 消费者。

```bash
export ANDROID_NDK_HOME=/path/to/ndk
```

#### 4.3 Windows（编 `dgc_paint.dll`，【人工】）

- **VS2026**：工作负载「使用 C++ 的桌面开发」+「C++ CMake tools for Windows」+ Ninja
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/)（`vulkan-1.lib`、`glslc`）

#### 4.4 真机 / 性能测试

平板与 AGI/RenderDoc 用于**消费者联调**，不阻塞 SDK host 构建。清单见任务 E0-5 / E0-6。

### 5. 验证构建（基座 CMake 落地后）

```bash
cmake --preset host-linux
cmake --build --preset host-linux
ctest --test-dir build/host-linux --output-on-failure

# NDK 已配置时
cmake --preset android-arm64
cmake --build --preset android-arm64
```

当前任务线尚未实现 B1-2 时，上述 preset 还不存在。

### 6. 开工

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
| `docs/git/` | 消费者 submodule 约定与模板 |
| `docs/调研/` | 路线与评审 |
| `scripts/` | `check-env.sh`、`bootstrap-consumer.sh` |
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
