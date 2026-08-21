# Linux host 工具链安装（Debian/Ubuntu / WSL Ubuntu）

本文件只写 **怎么装**。版本数字与探测退出码以 [`docs/env/toolchain.md`](toolchain.md) 为准（CMake 探测门禁 **≥ 3.22**；规划建议 host 独立安装 3.31+，探测仍按 3.22）。

**范围**：编译本仓库 `libdgc_paint` SDK 所需的 Linux host 编译器与 Vulkan **头文件**。不含 UI 消费者。

**不要**把 `libglfw3-dev` / GLFW / JDK / Android Studio 当作本仓库必需（GLFW 属于 `paint-pc` 消费者）。

---

## 任务书四包（Debian/Ubuntu）

```bash
sudo apt update
sudo apt install --no-install-recommends build-essential cmake ninja-build libvulkan-dev
```

| 包 | 作用 |
|----|------|
| `build-essential` | 提供 `g++`（及 `make`）；验收看的是 `g++` |
| `cmake` | 发行版包须 ≥ 3.22 |
| `ninja-build` | Debian 包名；二进制可能是 `ninja`（`check-env.sh` 先试 `ninja` 再 `ninja-build`） |
| `libvulkan-dev` | 提供 `/usr/include/vulkan/vulkan.h` 与 pc 文件 |

可重复安装：仓库内 [`scripts/setup-linux-host.sh`](../../scripts/setup-linux-host.sh)（**需要 sudo**；非 Linux / 非 WSL 会拒绝执行，不要在 Windows/Git Bash 里跑 `apt`）。

Windows 工作区检出时，仓库根 [`.gitattributes`](../../.gitattributes) 把 `*.sh` 固定为 LF，这样在 WSL 里可以直接 `bash scripts/check-env.sh`。若仍看到 `$'\r': command not found`，先确认该文件未被本地改回 CRLF。

**明确不装**：`libglfw3-dev`、JDK、Android Studio。

`pkg-config`：任务书允许用它发现 Vulkan 头；`libvulkan-dev` 通常会带上头文件。若要用 `pkg-config --exists vulkan`，可再装 `pkg-config`。它不是第五个任务书必需包，也不是 glfw 一类的消费者包。

无 GPU / 无 Vulkan ICD **不视为失败**。验收只探头文件或 `pkg-config`，不要求能创建 `VkInstance`。

---

## Windows 开发者：WSL Ubuntu

本机若是 Windows，用 **WSL Ubuntu 22.04+**（20.04 默认 cmake 往往低于 3.22）。

```powershell
wsl -l -v
# 没有 Ubuntu 时：
wsl --install -d Ubuntu
```

若系统提示重启，重启后再打开 Ubuntu 完成首次用户名/密码。

确认发行版是 Linux：

```powershell
wsl -d Ubuntu -- uname -s
# 期望：Linux
```

worktree / 仓库在 WSL 里走 `/mnt/<盘符>/...`。路径含空格必须加引号，例如：

```bash
cd "/mnt/d/qsw/canvas demo/paintDemo/.worktrees/E0-2"
```

包装在 WSL 发行版根文件系统里即可；探测只要 WSL `PATH` 上有 `g++` / `cmake` / `ninja`。

---

## 装完探测

在仓库根（或任意目录，脚本按自身位置定位仓库根）：

```bash
bash scripts/check-env.sh
```

期望：

- 整体 **exit 0**
- `g++`（脚本打印的 `cxx`）/ `cmake`（≥ 3.22）/ `ninja` 为 **OK**
- Vulkan 为 **OK**（`pkg-config vulkan` 或找到 `vulkan/vulkan.h`）
- NDK 可 WARN（归 E0-3）
- 不得因无 GLFW 失败

---

## cmake 过旧时

探测门禁仍是 **3.22**。若发行版 `cmake --version` 低于 3.22，改用 [Kitware apt 源](https://apt.kitware.com/) 或官方二进制升到 3.22+（规划建议 host 独立装 3.31+）。

---

## 其他发行版（本机验收仍是 WSL Ubuntu + apt）

本仓库验收路径是 **WSL Ubuntu + 上面四包**。其他发行版等价包名仅作参考：

| 发行版 | 等价安装 |
|--------|----------|
| Fedora | `sudo dnf install gcc-c++ cmake ninja-build vulkan-loader-devel` |
| Arch | `sudo pacman -S base-devel cmake ninja vulkan-headers vulkan-icd-loader` |

---

## 明确不做（归后续任务）

- 不执行 `cmake --preset`、不编译 `core/`、不创建仓库根 `CMakeLists.txt` / `CMakePresets.json`（**B1-2**）
- 不写 NDK 安装验收（**E0-3**）
- 不写 VS2026 核对表（**E0-4**）
- 不创建 `ui/` / `platform/` / `app/`
