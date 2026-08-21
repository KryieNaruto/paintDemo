# SDK host 工具链：探测规则与版本下限

本文件是换机文档的数字来源（SOT），与 [`scripts/check-env.sh`](../../scripts/check-env.sh) 顶部常量一致。

**范围**：编译本仓库 `libdgc_paint` SDK 所需的 **host** 工具。不含 UI 消费者。

本文件只定义「探测什么、退出码是什么」。安装步骤：

- Linux apt 包 → [`docs/env/linux-host.md`](linux-host.md)（E0-2）
- 命令行 NDK → **E0-3**
- Windows VS2026 + LunarG Vulkan SDK → **E0-4**

---

## 一条命令

在仓库内任意目录：

```bash
bash scripts/check-env.sh
```

脚本以自身路径定位 SDK 根，不依赖调用时的当前工作目录。

---

## 版本下限（与脚本常量相同）

脚本顶部：

| 常量 | 值 |
|------|-----|
| `CMAKE_MIN_MAJOR` | `3` |
| `CMAKE_MIN_MINOR` | `22` |

即 **CMake ≥ 3.22**（含 3.31、**4.x**）。规划建议 host 独立安装 3.31+，探测门禁仍是 3.22。

NDK 规划建议 **r27+（建议 r28）**。脚本有则打印 `Pkg.Revision`，**不因缺失或版本偏低而失败**。

---

## 三档探测

### 必需（缺 → 非零退出）

| 项 | 如何判定 |
|----|----------|
| CMake ≥ 3.22 | `cmake --version` 第一行的主.次数字比较。无 cmake，或可解析版本 &lt; 3.22，同等失败。 |
| Ninja | `ninja`；若无则试 `ninja-build`。缺则失败。 |
| C++ 编译器 | 若已设 `CXX` 则只用它；否则依次试 `g++` / `clang++` / `cl`。能找到即可（MSVC `cl` 以 `command -v` 为准）。**不**编译测试 `.cpp`。 |

### 有则探测、无则警告（仍 0 退出）

| 项 | 如何判定 |
|----|----------|
| NDK | 只读 `ANDROID_NDK_HOME`，未设再看 `ANDROID_NDK`。不扫盘，不枚举 `ANDROID_SDK_ROOT/ndk/*`。目录存在则打印 `source.properties` 的 `Pkg.Revision`；无该文件则打印路径并 WARN。未配置：WARN「NDK 未配置，host 可先编」。 |
| Vulkan 头或 `libvulkan` | `pkg-config vulkan`，或 `VULKAN_SDK` 下的头/库，或常见路径（如 `/usr/include/vulkan/vulkan.h`）。无则 WARN（Null 后端不需要；L4 才硬依赖）。 |

### 非失败项（不要当成门禁）

GLFW / JDK / Android Studio 属于 `paint-android` / `paint-pc` 消费者，见 [`docs/git/`](../git/README.md)。脚本固定打印一行 INFO，**不因缺失失败**。不必把 `java` / GLFW 列为探测失败项。

---

## 退出码

| 码 | 含义 |
|----|------|
| 0 | 必需项齐（NDK / Vulkan 可仅 WARN） |
| 1 | 缺 cmake / cmake &lt; 3.22 / 缺 ninja / 缺 C++ 编译器 |

每项一行：`OK` / `WARN` / `FAIL` / `INFO` + 工具名 + 版本或路径。

---

## 明确不做

- 不 `sudo apt install …`（安装步骤见 [`linux-host.md`](linux-host.md)）
- 不创建仓库根 `CMakeLists.txt` / `CMakePresets.json`（B1-2）
- 不创建 `ui/` `platform/` `app/`
- 不把 GLFW / JDK / Android Studio / Compose / ImGui 列为 SDK 必需
