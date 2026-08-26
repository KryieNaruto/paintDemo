# 三方库供给系统 · 设计（2026-08-25）

> 状态：设计草案（待审阅）
> 触发：build-pipeline / 需求「整理 paint-pc 与 paint-android 所需三方库，环境构建时一键拉取编译；大库用预编译包，小库直接编译」
> 范围：**SDK 主仓库（paintDemo）** 新增共享 deps 脚本 + 清单；**paint-pc / paint-android** 各 setup.sh 增强调用。不改 SDK 构建逻辑。

---

## 1. 背景与事实核查

### 用户已拍板（brainstorming 结论）
1. **大库预编译包来源**：Linux **无法使用 sudo** → 下载离线 deb 包本地安装；Windows → 直接 curl 下载编译产物。**两者都必须使用国内镜像源**，镜像源配置必须写入脚本（不依赖用户手动设）。
2. **小库交付**：维持 FetchContent 编译（GLFW/ImGui 源码编译）。
3. **交付形态**：共享 deps 脚本 + 清单（manifest）。
4. **共享脚本位置**：放 SDK 仓库（paintDemo）。
5. **Windows 大库**：尝试自动下载安装。
6. **小库镜像**：配置国内镜像加速 FetchContent（GitHub 慢）。

### 三方库清单（事实核查）

| 库 | 消费方 | 大小 | 供给 | Linux 来源 | Windows 来源 |
|---|---|---|---|---|---|
| GLFW 3.3.10 | paint-pc | 小 | FetchContent 编译 | GitHub（镜像加速） | 同 |
| ImGui v1.90.9 | paint-pc | 小 | FetchContent 编译 | GitHub（镜像加速） | 同 |
| Vulkan（头+loader） | SDK+paint-pc | 大 | 预编译 | 清华/阿里 deb 解包 → DGCPAIN_DEPS_ROOT | LunarG SDK exe |
| shaderc（libshaderc1+dev） | SDK host | 大 | 预编译 | 清华/阿里 deb | LunarG SDK 自带 |
| glslc | 构建期 shader | 大 | 预编译 | 清华/阿里 deb | LunarG SDK 自带 |
| Android SDK / NDK 28 | paint-android | 大 | SDK Manager | dl.google.com（国内可达） | 同 |
| nlohmann/json | SDK | 极小 | vendored | sdk/third_party | 同 |
| stb_image_write | SDK | 极小 | vendored | sdk/third_party | 同 |

### 镜像可行性（已实证验证，2026-08-25）
- 清华 tuna：`/ubuntu/pool/main/v/vulkan-loader/libvulkan-dev_1.4.341.0-1_amd64.deb`（1.6 MiB）✅
- 清华 tuna：`/ubuntu/pool/universe/s/shaderc/` 有 `libshaderc1` / `libshaderc-dev` / `glslc` deb（多版本 2023.8~2026.3）✅
- 阿里云：`/ubuntu/pool/universe/s/shaderc/` 同样有（镜像路径一致）✅
- 无 sudo 解包：`dpkg-deb -x <deb> <prefix>` → 头/库落在 `<prefix>/usr/include|lib` → SDK 的 `find_path/find_library` + `DGCPAIN_DEPS_ROOT` 已支持 ✅
- Ubuntu 24.04 noble：libshaderc1 版本 `2023.8-1build1`（glibc 兼容）；libvulkan-dev 版本 `1.3.275.0-1build1`（本机 loader 1.3.275 匹配）✅
- **本机 uid 1042（无 sudo）**，正好实测「无 sudo 离线安装」主线。

### 现状 vs 目标

| 现状 | 目标 |
|---|---|
| 大库靠系统包（需 sudo）/ 手动装 | 脚本从国内镜像 curl 离线包 → 无 sudo 解包到 deps |
| 镜像未配置，GitHub 直连慢 | 镜像 URL 池写入 manifest，脚本内置 |
| 消费者 setup.sh 各自为政 | 共享 fetch-deps.sh + manifest（单一事实来源） |
| shaderc/glslc 缺失时仅给指引 | 脚本自动拉取解包 |

---

## 2. 目标与验收

### 目标
在 SDK 主仓库提供一份**三方库清单**（manifest.yaml，单一事实来源）与**共享拉取脚本**（fetch-deps.sh）；paint-pc / paint-android 的 setup.sh 在构建前调用它，实现「一键拉取编译」：**大库从国内镜像拉预编译包解包到 deps、小库 FetchContent 源码编译（镜像加速）**。

### 验收标准（可度量）
1. **manifest.yaml**：覆盖上表全部 8 库；每库含 `name / size_class / platform / version / source_urls(镜像池) / extract_to / env_export`；镜像池含清华→阿里→官方 fallback。
2. **fetch-deps.sh --list**：打印清单，与 manifest 一致。
3. **fetch-deps.sh --check**：本机（无 sudo）实测输出各库「OK（已有）/ MISS（需拉）」；Vulkan/shaderc 应 MISS（本机缺 dev 头）。
4. **fetch-deps.sh --fetch**：从清华镜像 curl `libvulkan-dev` + `libshaderc1` + `libshaderc-dev` + `glslc` → `dpkg-deb -x` 解包到 `deps/` → 导出 `DGCPAIN_DEPS_ROOT`。**本机无 sudo 全程可跑**。
5. **闭环验证**：`DGCPAIN_DEPS_ROOT=deps/usr` 下跑 SDK host 测试（`test_offscreen` / `test_brush_offscreen`）**通过**（证明 Vulkan/shaderc 真实可用，非仅解包成功）。paint-pc `tests/smoke.sh` headless 离屏 PNG 生成成功。
6. **消费者接入**：paint-pc / paint-android setup.sh 在探测后调用 `sdk/scripts/fetch-deps.sh --fetch`（幂等：已满足则跳过）。
7. **镜像加速 FetchContent**：脚本提供 git 镜像 URL 配置（如 ghproxy 前缀），paint-pc CMake 拉 GLFW/ImGui 加速；失败回退官方。

### 非目标（本期，诚实标注）
- **不承诺 Windows LunarG SDK exe 全静默安装**：安装器交互/体积/版本匹配风险高。Windows 侧策略 = 探测 VULKAN_SDK 已装即用；缺失时脚本尝试镜像拉 **shaderc 预编译产物** + 给 LunarG SDK 国内加速安装指引（非全自动）。这是**务实取舍**，非回退设计。
- 不改 SDK 构建逻辑（复用 DGCPAIN_DEPS_ROOT / find_path / find_library）。
- 不迁移 Android SDK/NDK 供给（继续 SDK Manager，脚本仅探测+指引）。
- 不 vendor GLFW/ImGui 进仓库。

---

## 3. 架构

```
                    ┌───────────────────────────────┐
                    │ paintDemo (SDK 主仓库)        │
                    │  scripts/fetch-deps.sh         │  ← 共享入口（CLI）
                    │  deps/manifest.yaml            │  ← 单一事实来源
                    │  deps/README.md                │
                    └───────────┬───────────────────┘
                                │ 调用（submodule sdk/scripts/fetch-deps.sh）
              ┌─────────────────┼──────────────────┐
              ▼                 ▼                  ▼
   ┌──────────────────┐ ┌─────────────────┐ ┌──────────────────┐
   │ paint-pc setup.sh│ │ paint-android   │ │ SDK host 测试    │
   │ 探测→fetch→构建   │ │ setup.sh        │ │ DGCPAIN_DEPS_ROOT│
   │ FetchContent 拉  │ │ 探测→fetch→Gradle│ │ find_path 命中   │
   │ GLFW/ImGui(镜像) │ │                 │ │                  │
   └──────────────────┘ └─────────────────┘ └──────────────────┘
```

### 组件

**`deps/manifest.yaml`**（单一事实来源）
```yaml
mirrors:
  tuna: "https://mirrors.tuna.tsinghua.edu.cn"
  aliyun: "https://mirrors.aliyun.com"
deps:
  - name: vulkan
    size_class: big
    platform: [linux, win]
    version: "1.3.275.0-1build1"      # Ubuntu noble；Linux 用，匹配本机 loader
    source_urls:
      - "{tuna}/ubuntu/pool/main/v/vulkan-loader/libvulkan-dev_{version}_amd64.deb"
      - "{aliyun}/ubuntu/pool/main/v/vulkan-loader/libvulkan-dev_{version}_amd64.deb"
    extract_to: "deps/vulkan"
    env_export: "DGCPAIN_DEPS_ROOT"
    check: "include/vulkan/vulkan.h"   # 存在则视为已满足
  - name: shaderc
    size_class: big
    platform: [linux]
    version: "2023.8-1build1"
    debs: [libshaderc1, libshaderc-dev, glslc]
    source_urls: ["{tuna|aliyun}/ubuntu/pool/universe/s/shaderc/{pkg}_{version}_amd64.deb"]
    extract_to: "deps/shaderc"
    env_export: "DGCPAIN_DEPS_ROOT"    # 与 vulkan 同 prefix，解包到同一 deps/usr
    check: "lib/libshaderc_shared.so"
  - name: glfw / imgui
    size_class: small
    platform: [linux, win]
    fetch: "FetchContent"
    mirror: "ghproxy"                  # git clone 加速前缀
  - name: android_sdk / android_ndk
    size_class: big
    platform: [linux, win, mac]
    fetch: "sdkmanager"
    mirror: "dl.google.com"            # 国内可达，探测+指引
```

**`scripts/fetch-deps.sh`**（共享 CLI）
- 解析 manifest（`grep`/awk 或 shell 内 YAML 子集解析，避免引入依赖；若仓库已有 yq/其他则复用）。
- 子命令：`--list`（打印清单）/ `--check`（探测缺项）/ `--fetch`（拉取解包）/ `--help`。
- **`--fetch` 流程（大库）**：
  1. 按 `platform` 过滤当前 OS（Linux/Windows 判定复用现有 is_linux）。
  2. 对每个大库 `check` 命中 → 跳过（幂等）；否则进入拉取。
  3. 依序尝试 `source_urls` 镜像池：`curl -fL --retry 3 --connect-timeout 10 -o <deps>/cache/<name>.deb <url>`；第一个成功即停。
  4. `dpkg-deb -x <deb> <extract_to>`（Linux）→ 头/库落在 `<extract_to>/usr/...`。
  5. 汇总导出：`DGCPAIN_DEPS_ROOT=<deps>/vulkan/usr`（或合并统一 prefix，见 §3.4 决策）。
- **镜像加速小库**：`--fetch` 时若检测到 git，输出 `git config` 建议或直接配置 ghproxy 前缀 URL 供 FetchContent（paint-pc CMake 已有 FetchContent，脚本提供 `PC_FETCH_MIRROR` 环境变量注入）。

**`deps/README.md`**：用法、镜像源说明、版本固定策略、Windows 大库指引。

### 关键决策：deps 布局与 env 导出
- **统一 prefix**：所有 Linux 大库解包到**同一个** `deps/usr`（`dpkg-deb -x` 的 usr 目录直接合并），`DGCPAIN_DEPS_ROOT=deps/usr` 一次导出，SDK `find_path/find_library` 一次命中全部。避免多 prefix 变量。
- 布局：`deps/usr/{include,lib,...}`、`deps/cache/*.deb`、`deps/.fetch-marker`（记录已拉版本，幂等依据）。

### 消费者接入
- **paint-pc setup.sh**：`build_pc` 前调用 `sdk/scripts/fetch-deps.sh --fetch`；成功后导出 `DGCPAIN_DEPS_ROOT` 供 cmake。已有 `DGCPAIN_DEPS_ROOT`/`PC_X11_DEPS_ROOT` 覆盖逻辑保持。
- **paint-android setup.sh**：Gradle 构建前调用 `sdk/scripts/fetch-deps.sh --fetch`（Android 侧大库只有 SDK/NDK，探测即可；shaderc Android 走 glslc 预编译 SPIR-V，不需本脚本）。

### 错误处理
- 镜像全部失败 → 非零退出 + 列出所有尝试过的 URL + 提示手动下载路径（如实报告，非回退）。
- 解包失败（非 deb / 损坏）→ 非零退出 + 清理该 deb。
- 已满足（幂等）→ 0 退出打印 SKIP。
- 无 curl/dpkg-deb → 探测阶段已拦截（硬依赖）。

---

## 4. 分阶段

| 阶段 | 内容 | 产出 |
|---|---|---|
| P1 | manifest.yaml + fetch-deps.sh（--list/--check/--fetch）+ README | SDK 侧供给系统可用 |
| P2 | 本机（无 sudo）实测 --fetch 拉 vulkan+shaderc → SDK host 测试闭环 | 解包可编译证据 |
| P3 | paint-pc / paint-android setup.sh 接入 fetch-deps + 镜像加速 FetchContent | 双仓一键 |
| P4 | 测试门：headless 离屏 PNG + setup.sh --test + 幂等验证 | 全绿 |

---

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 镜像 URL 变更（deb 版本移除） | 镜像池多源 + 固定版本；版本缺失时 `--fetch` 报明确错误而非静默 |
| libshaderc1 版本与 glibc 不兼容 | 固定 noble (24.04) 版本；若宿主更老，提示换版本 |
| dpkg-deb -x 合并 usr 冲突 | 统一 prefix 天然合并；冲突时后者覆盖并打印警告 |
| Windows LunarG exe 静默不可靠 | 探测优先；缺失给指引 + 尝试 shaderc 预编译（诚实标注非全自动） |
| FetchContent GitHub 慢 | ghproxy 镜像前缀 + 失败回退官方 |
| 改动消费者 setup.sh 引入回归 | 各自测试门（setup.sh --test）守护 |

---

## 6. 验证方式
- **CLI 硬约束**：`fetch-deps.sh --list/--check/--fetch` 无头可跑。
- **离屏图像硬约束**：解包后 paint-pc `tests/smoke.sh` headless 离屏 PNG 生成 + 像素断言通过（Vulkan 真实后端）。
- 幂等：`--fetch` 跑两次第二次全 SKIP。
- 消费者：paint-pc `setup.sh --test`（含 smoke.sh）全绿；paint-android `setup.sh --check` 探测不回归。
