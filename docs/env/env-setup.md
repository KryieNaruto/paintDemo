# 环境搭建说明书（SDK 编译环境）

> 适用范围：本仓库 `libdgc_paint` SDK 的 **host 编译环境**（Linux host + Windows VS2026）。
> 不含消费者（Compose / GLFW / ImGui / JDK）的安装——那些属于 UI 层，本 SDK 不依赖（输入平滑已下沉到 SDK 内核 `core/stroke_predictor`）。
> 依据：`DGCPaint_技术规划.md` §1、§2；`docs/调研/路线整理.md` §7；任务书 `docs/tasks/detail/环境搭建与项目骨架.md` E0-1。
> 换机后照本文档即可搭好 SDK 编译环境。本文档不建工程，不新增任何 `CMakeLists.txt`。

---

## 0. 依赖分级总表（全文统一口径）

> 两档措辞全文一致：
> - **硬依赖** = 缺失则 host 编译**必败**（configure / build 直接报错）。
> - **软依赖** = 缺失仅**警告**，不阻断编译（Null 后端不硬依赖）。

| 依赖 | 级别 | 版本下限 | 缺失后果 | 说明 |
|---|---|---|---|---|
| C++ 编译器（`g++`/`gcc` 或 MSVC `cl.exe`） | **硬** | GCC ≥ 11（建议最新），MSVC 为 VS2026 自带 | 编译必败 | 编译 C++ 核心 |
| `cmake` | **硬** | **≥ 3.22**（host 建议独立装 **3.31+**） | configure 必败 | 多 toolchain + presets 关键 |
| `ninja` | **硬** | 任意较新版本 | build 必败 | 生成器统一用 Ninja |
| Vulkan（`libvulkan-dev` / LunarG Vulkan SDK + 驱动） | **软** | Vulkan 1.1+（minSdk 30 覆盖） | 仅警告 | 跑 Vulkan compute 才需要；Null 后端不硬依赖 |
| Android NDK（`$ANDROID_NDK_HOME`） | **软** | **r27+（建议 r28）** | 仅警告 | 编 `android-arm64` preset 才需要；自带 CMake、glslc（shaderc）、Vulkan 头 |
| `glslc` / shaderc | **软** | 随 NDK / Vulkan SDK 提供 | 仅警告 | 编译 GLSL compute shader 才需要 |

> **软/硬边界依据**：以「Null 后端不硬依赖」为准——NDK、Vulkan、glslc/shaderc 一律**软依赖**，缺失仅提示、绝不阻断 host 编译。不要把 NDK 或 Vulkan 误标为硬依赖。

---

## 1. 探测命令清单（E0-2 `--check` 的单一事实来源）

> 本文档固化的探测清单即 **E0-2 `scripts/setup-env.sh --check`** 的口径来源，二者必须一致。下表「级别」列即为脚本判空后的输出级别。

| 检查项 | 命令 | 级别 | 有则输出 | 无则输出 |
|---|---|---|---|---|
| cmake | `which cmake` → `cmake --version` | 硬 | 版本号（需 ≥ 3.22） | 硬错误 |
| ninja | `which ninja` → `ninja --version` | 硬 | 版本号 | 硬错误 |
| C++ 编译器 | `which g++`（Linux）/ `where cl`（Windows，需在 VS 开发者命令行内） | 硬 | 版本（`g++ --version` / `cl`） | 硬错误 |
| Vulkan | `vulkaninfo`（Linux）/ `glslc --version`（Windows） | 软 | 版本号 | 警告 |
| NDK | `echo $ANDROID_NDK_HOME` → `ls $ANDROID_NDK_HOME` | 软 | 路径（r27+，建议 r28） | 警告 |
| glslc | `glslc --version` | 软 | 版本号 | 警告 |

---

## 2. Linux host 环境搭建

### 2.1 版本下限

| 组件 | 下限 | 建议 |
|---|---|---|
| C++ 编译器 | `g++` / `gcc` ≥ 11 | 发行版自带最新 |
| cmake | **3.22** | host 独立装 **3.31+** |
| ninja | 较新即可 | 发行版自带 |
| Vulkan（软） | 1.1+ | `libvulkan-dev` + 显卡驱动 |
| NDK（软） | r27 | r28 |

### 2.2 安装依赖

```bash
# 硬依赖（缺一不可）
sudo apt update
sudo apt install build-essential cmake ninja-build

# 软依赖（可选，跑 Vulkan compute 才需要）
sudo apt install libvulkan-dev
# 可选：装显卡驱动后 vulkaninfo 才能列出 device
```

> host 建议独立安装 cmake 3.31+（发行版仓库可能只有旧版，低于 3.22 会 configure 失败）。

### 2.3 探测（换机后先跑）

```bash
which cmake ninja g++
cmake --version      # 需 ≥ 3.22
g++ --version
ninja --version

# 软依赖：有则报版本、无则警告（不失败）
vulkaninfo 2>/dev/null || echo "WARN: Vulkan 未安装（仅跑 Vulkan compute 需要）"
echo "ANDROID_NDK_HOME=${ANDROID_NDK_HOME:-（未设置）}"
[ -n "$ANDROID_NDK_HOME" ] && ls "$ANDROID_NDK_HOME" || echo "WARN: NDK 未配置（仅 android-arm64 preset 需要）"
glslc --version 2>/dev/null || echo "WARN: glslc 未安装（仅编译 shader 需要）"
```

### 2.4 补缺

缺硬依赖 → 照 2.2 安装；缺软依赖 → 仅记录警告，可先跳过，需要 Vulkan/NDK 时再补。

---

## 3. Windows（VS2026）环境搭建

### 3.1 版本下限

| 组件 | 下限 |
|---|---|
| Visual Studio | **VS2026**（或更高） |
| 工作负载 | 「使用 C++ 的桌面开发」+「C++ CMake tools for Windows」 |
| Ninja | VS 自带 |
| CMake | 3.22+（VS 自带，建议 3.31+） |
| Vulkan（软） | [LunarG Vulkan SDK](https://vulkan.lunarg.com/)（Windows 版，提供 `vulkan-1.lib`、`glslc`/`glslangValidator`） |

### 3.2 安装步骤

1. 安装 **VS2026**，勾选工作负载：
   - 「使用 C++ 的桌面开发」（MSVC + CMake 集成）
   - 「C++ CMake tools for Windows」
   - Ninja 由 VS 自带，无需单独装。
2. 安装 [LunarG Vulkan SDK](https://vulkan.lunarg.com/)（**软依赖**，跑 Vulkan compute 才需要），装后 `glslc`、`vulkan-1.lib` 可用。

### 3.3 探测（在「Developer Command Prompt for VS 2026」内）

```bat
where cmake
cmake --version        rem 需 ≥ 3.22
where cl
cl
where ninja
ninja --version

rem 软依赖：有则报版本、无则警告
glslc --version || echo WARN: glslc 未安装（LunarG Vulkan SDK）
echo %ANDROID_NDK_HOME%   rem 无则警告，仅 android-arm64 preset 需要
```

> VS2026 用 `CMakePresets.json`（E0-3 落地）一键切换 host / android 配置：打开项目根目录即自动识别 presets，无需手动配 toolchain。

### 3.4 Windows 一键搭建（PowerShell / `setup-env.ps1`）

在 **PowerShell 5.1+ / PowerShell Core 7+** 内运行 `scripts/setup-env.ps1`（原生，免装 Git Bash；无头可跑）：

```powershell
.\scripts\setup-env.ps1              # 探测 + 补缺指引 + 拉取 paint-pc + 构建 + 离屏 PNG 验证
.\scripts\setup-env.ps1 --check      # 只探测不安装，输出缺项清单
.\scripts\setup-env.ps1 -SkipBuild   # 探测 + 拉取，跳过构建/验证
.\scripts\setup-env.ps1 -Help        # 打印帮助
```

脚本行为（W1）：
- 用 `vswhere` 定位 VS2026 安装与 `vcvars64.bat`；探测 cmake / ninja / MSVC(cl) / git / Vulkan SDK（`$env:VULKAN_SDK`）/ glslc，硬/软分级与 §0 总表一致。
- **paint-pc 离屏渲染走真实 `VkBackend` → Vulkan SDK 为硬依赖**（链接 `vulkan-1.lib` 必败），与 §0「SDK Null 后端软依赖」口径不同——这是「消费者真实绘制」与「SDK 编译」的分界。
- 硬依赖缺失给精确安装指引（VS installer `modify --add` 补工作负载 / LunarG Vulkan SDK 下载页 / git 安装）并非零退出；软依赖（glslc）仅警告。
- 拉取：`git clone --recurse-submodules` paint-pc + submodule 钉 `9e6eefb`（含 B3-1 真实内核）。
- 构建：`vcvars64` 环境内 `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF` + `cmake --build`（与 paint-pc 现有口径一致）。
- 验证：`build\paint_pc.exe --headless out.png` + **System.Drawing 读 PNG 断言真实笔迹像素**（四角众数背景 + 隔 3 采样，`dark>50` 判 PASS）。此逻辑已对真实内核 PNG（`dark=973`）与空笔迹反例（`dark=0`）交叉验证一致。

> 前置：VS2026 + 「使用 C++ 的桌面开发」+「C++ CMake tools for Windows」工作负载；Vulkan SDK（硬，paint-pc 真实绘制）；git。本机为 Linux 时脚本会退出（仅 Windows 平台有意义）。

---

## 4. 换机步骤（clone → 探测 → 补缺）

1. **配 SSH key**：把本机 SSH 公钥加到 GitHub [Settings → SSH and GPG keys](https://github.com/settings/keys)。
2. **Clone**：

   ```bash
   git clone git@github.com:KryieNaruto/paintDemo.git
   cd paintDemo
   ```

3. **探测**：按第 1 节清单（Linux 见 2.3 / Windows 见 3.3）跑命令，记录硬/软缺失项。
4. **补缺**：
   - Linux：`sudo apt install build-essential cmake ninja-build`（缺软依赖时按需补 `libvulkan-dev` 等）。
   - Windows：VS2026 installer 补工作负载；NDK 用 SDK Manager 补 `ndk;28.x`、`cmake;3.31.x`。

> 之后 E0-2 的 `scripts/setup-env.sh` 会把这些探测/补缺自动化；未就绪前按本文档手工执行。

---

## 5. 明确非必需（消费者依赖，不要求装）

以下组件属于**消费者（UI 层）**，本 SDK **不依赖、不要求安装**，只提示不要求：

| 组件 | 归属 | 说明 |
|---|---|---|
| GLFW | 消费者 | PC 窗口/输入，SDK 不依赖；不要把 `libglfw3-dev` 当本仓库必需 |
| JDK | 消费者 | AGP 要求（JDK 21），与 SDK host 编译无关 |
| Android Studio GUI | 消费者 | 出 `.so` / APK 用，SDK host 编译不需要 |
| Jetpack Compose | 消费者 | Android UI 层 |
| ImGui | 消费者 | PC UI 层 |

---

## 6. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `cmake --version` < 3.22 | cmake 过旧 | Linux 独立装 3.31+；Windows 用 VS 自带或升级 CMake tools |
| `ninja: command not found` | ninja 缺失（硬） | `sudo apt install ninja-build`；Windows 用 VS 自带 Ninja |
| `cmake --preset android-arm64` 报 NDK 找不到 | `ANDROID_NDK_HOME` 未设或 NDK 缺失 | 设 `ANDROID_NDK_HOME` 指向 NDK r27+/r28；无 NDK 则此 preset 不适用（属软依赖，非 host 编译阻断） |
| `vulkaninfo` 无 device / `glslc` 缺失 | Vulkan 软依赖缺失 | 仅警告；Null 后端不需要，跑 Vulkan compute 前再补（`libvulkan-dev` / LunarG SDK） |
| 找不到 `cl.exe` | 未在 VS 开发者命令行内 | 用「Developer Command Prompt for VS 2026」跑探测/构建 |

---

## 7. 自检（对照验收标准）

- [x] 含 Linux + Windows 两套步骤与版本下限（§2 / §3）。
- [x] 明确区分「硬依赖缺失（编译必败）」与「软依赖缺失（警告即可）」（§0 总表 + 全文两档措辞）。
- [x] 未新增工程 `CMakeLists.txt`（本文档纯文档，不建工程）。
- [x] 明确不列 GLFW / JDK 为 SDK 必需（§5）。
