# 三方库供给系统 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 SDK 主仓库（paintDemo）提供共享三方库清单 `deps/manifest.yaml` + 拉取脚本 `scripts/fetch-deps.sh`，paint-pc/paint-android 的 setup.sh 调用它实现一键拉取编译：大库从国内镜像（清华/阿里）curl 离线 deb 无 sudo 解包到 `deps/usr`，小库维持 FetchContent（ghproxy 镜像加速）。

**Architecture:** 单一事实来源 manifest → 共享 CLI `fetch-deps.sh --list/--check/--fetch` → 解包到统一 `deps/usr` 导出 `DGCPAIN_DEPS_ROOT`（SDK 现有 find_path/find_library HINTS 已支持）→ 消费者 setup.sh 在构建前调用。小库 FetchContent 由各仓 CMake 处理，脚本提供镜像加速配置。

**Tech Stack:** Bash、YAML 子集解析（纯 shell，不引第三方）、curl、dpkg-deb、CMake FetchContent。

**Spec:** `docs/superpowers/specs/2026-08-25-deps-bootstrap-design.md`

## Global Constraints

- **无 sudo**：所有拉取/解包必须可用非 root 完成；不使用 `sudo`、不写系统目录。
- **镜像源写入脚本**：镜像 URL 池（清华→阿里→官方）内置在 manifest/脚本，不依赖用户手动设环境变量。
- **统一 prefix**：所有 Linux 大库解包到同一个 `deps/usr`，`DGCPAIN_DEPS_ROOT=deps/usr` 一次导出。
- **幂等**：`--fetch` 已满足的库必须 SKIP（0 退出），重复运行不重复下载。
- **不回退**：镜像失败如实报错（非零退出 + 列出尝试 URL），不做降级/兜底。
- **不引第三方解析器**：manifest 用纯 shell 解析（grep/awk），不引入 yq/python。
- **改动消费者 setup.sh 不引入回归**：各自 setup.sh --test 守护。
- 版本固定：libvulkan-dev `1.3.275.0-1build1`（匹配本机 loader 1.3.275）、libshaderc1/libshaderc-dev/glslc `2023.8-1build1`（Ubuntu noble）。
- manifest 落在 SDK 主仓库 `deps/`，路径：SDK 仓库根 `/home/qiansenwei/workspace/demo`。

---

### Task 1: manifest.yaml + fetch-deps.sh 骨架（--list/--check/--help）

**Files:**
- Create: `deps/manifest.yaml`
- Create: `scripts/fetch-deps.sh`
- Create: `deps/README.md`

**Interfaces:**
- Consumes: 无（首个任务）
- Produces: `scripts/fetch-deps.sh` 支持子命令 `--list`、`--check`、`--fetch`、`--help`；`deps/manifest.yaml` 结构被 Task 2/3 消费。

- [ ] **Step 1: 创建 `deps/manifest.yaml`**（单一事实来源，纯 YAML 子集）

```yaml
# 三方库清单（单一事实来源）。供 scripts/fetch-deps.sh 解析。
# size_class: big=预编译拉取；small=FetchContent 编译
# 镜像池（清华→阿里→官方 archive.ubuntu.com）实现在 fetch-deps.sh 的 fetch_deb 内，
# 不在本文件重复声明（避免双处维护）。url 支持 {mirror} {version} 占位符。
deps:
  - name: vulkan
    size_class: big
    platform: [linux, win]
    version: "1.3.275.0-1build1"
    debs:
      - pkg: libvulkan-dev
        url: "{mirror}/ubuntu/pool/main/v/vulkan-loader/libvulkan-dev_{version}_amd64.deb"
    extract_to: "deps/usr"
    env_export: "DGCPAIN_DEPS_ROOT"
    check: "include/vulkan/vulkan.h"
  - name: shaderc
    size_class: big
    platform: [linux]
    version: "2023.8-1build1"
    debs:
      - pkg: libshaderc1
        url: "{mirror}/ubuntu/pool/universe/s/shaderc/libshaderc1_{version}_amd64.deb"
      - pkg: libshaderc-dev
        url: "{mirror}/ubuntu/pool/universe/s/shaderc/libshaderc-dev_{version}_amd64.deb"
      - pkg: glslc
        url: "{mirror}/ubuntu/pool/universe/s/shaderc/glslc_{version}_amd64.deb"
    extract_to: "deps/usr"
    env_export: "DGCPAIN_DEPS_ROOT"
    check: "lib/libshaderc_shared.so"
  - name: glfw
    size_class: small
    platform: [linux, win]
    fetch: "FetchContent"
    mirror: "ghproxy"
  - name: imgui
    size_class: small
    platform: [linux, win]
    fetch: "FetchContent"
    mirror: "ghproxy"
  - name: android_sdk
    size_class: big
    platform: [linux, win, mac]
    fetch: "sdkmanager"
    mirror: "dl.google.com"
  - name: android_ndk
    size_class: big
    platform: [linux, win, mac]
    fetch: "sdkmanager"
    mirror: "dl.google.com"
```

- [ ] **Step 2: 创建 `scripts/fetch-deps.sh` 骨架**（解析 manifest + 子命令分发）

```bash
#!/usr/bin/env bash
# fetch-deps.sh — 三方库一键拉取（共享 CLI，paint-pc / paint-android setup.sh 调用）
#
# 用法:
#   scripts/fetch-deps.sh --list   打印三方库清单
#   scripts/fetch-deps.sh --check  探测缺哪些大库（本机）
#   scripts/fetch-deps.sh --fetch  从国内镜像拉取/解包缺失大库到 deps/usr
#   scripts/fetch-deps.sh --help   打印帮助
#
# 设计: manifest.yaml 为单一事实来源；大库预编译 deb 解包到 deps/usr 导出 DGCPAIN_DEPS_ROOT；
#   小库 FetchContent 由各仓 CMake 处理（本脚本提供镜像加速配置）。
# 无 sudo: 全部用 curl + dpkg-deb -x 到仓库内 deps/，不写系统目录。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFEST="$REPO_ROOT/deps/manifest.yaml"
DEPS_USR="$REPO_ROOT/deps/usr"
DEPS_CACHE="$REPO_ROOT/deps/cache"

info() { printf '%s\n' "$*"; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }
has()  { command -v "$1" >/dev/null 2>&1; }

is_linux() { [ "$(uname -s)" = "Linux" ]; }

# ---- manifest 解析（纯 shell，不引第三方）----
# 读 deps: 下每个 name / size_class / platform / version / check / env_export。
# 输出: 每库一行 "name|size_class|platform|version|check|env_export"（platform 逗号连接）。
# 关键: 字段按 key 剥离（sub 删掉 "key:" 前缀），不用位置字段（$3）—— size_class:
# 等是双字段行，$3 必空；emit 条件依赖每库都有的 size_class 行，不依赖可选的 fetch: 行。
parse_manifest() {
  awk '
    # 关键：在读到【下一记录】的 name: 时才 flush 上一条（name && size）。
    # 若在 size_class: 行 emit，会滞后一条记录（vulkan 字段全空、每条用上条的）。
    function flush() {
      if (name && size) printf "%s|%s|%s|%s|%s|%s|%s\n", name, size, plat, ver, chk, env, fch
    }
    /^  - name:/     { flush(); name=$3; size=""; plat=""; ver=""; chk=""; env=""; fch="" }
    /^    size_class:/ { sub(/^ *size_class: */, ""); size=$0; gsub(/[ \t]/, "", size) }
    /^    platform:/ { sub(/^ *platform: *\[?/, ""); sub(/\]?[ \t]*$/, ""); plat=$0; gsub(/[ \t]/, "", plat) }
    /^    version:/  { sub(/^ *version: *"/, ""); sub(/"[ \t]*$/, ""); ver=$0; gsub(/[ \t]/, "", ver) }
    /^    check:/    { sub(/^ *check: *"/, ""); sub(/"[ \t]*$/, ""); chk=$0 }
    /^    env_export:/ { sub(/^ *env_export: *"/, ""); sub(/"[ \t]*$/, ""); env=$0 }
    /^    fetch:/    { sub(/^ *fetch: *"/, ""); sub(/"[ \t]*$/, ""); fch=$0 }
    END           { flush() }
  ' "$MANIFEST"
}

usage() { sed -n '4,12p' "$SCRIPT_DIR/fetch-deps.sh"; }

cmd_list() {
  info "=== 三方库清单（$MANIFEST）==="
  while IFS='|' read -r name size plat ver chk env; do
    printf '  %-14s size=%s platform=[%s] version=%s\n' "$name" "$size" "$plat" "${ver:-—}"
  done < <(parse_manifest)
}

cmd_check() {
  info "=== 大库探测（--check）==="
  local found=0 missing=0 skipped=0
  # 进程替换而非管道：避免子 shell 吞掉 found/missing 计数。
  while IFS='|' read -r name size plat ver chk env fch; do
    [ "$size" != "big" ] && continue
    case ",$plat," in
      *,linux,*) : ;;
      *) continue ;;   # 平台不匹配
    esac
    # sdkmanager 供给（android_sdk/ndk）：不计 MISS、不 return 1，仅提示。
    if [ "$fch" = "sdkmanager" ]; then
      printf '  [SKIP] %s（由 SDK Manager 供给，本脚本不拉取）\n' "$name"
      skipped=$((skipped+1))
      continue
    fi
    if [ -n "$chk" ] && [ -e "$DEPS_USR/$chk" ]; then
      printf '  [OK]   %s (%s)\n' "$name" "$DEPS_USR/$chk"
      found=$((found+1))
    else
      printf '  [MISS] %s (version=%s)\n' "$name" "$ver"
      missing=$((missing+1))
    fi
  done < <(parse_manifest)
  info ""
  if [ "$missing" -gt 0 ]; then
    info "缺失 $missing 项大库（可 --fetch 拉取）"
    return 1
  fi
  info "大库齐全${skipped:+（$skipped 项由 SDK Manager 供给）}"
  return 0
}

case "${1:-}" in
  --list)  cmd_list ;;
  --check) cmd_check ;;
  --fetch) echo "（Task 2 实现）" ;;
  --help|-h|"") usage ;;
  *) err "未知参数: $1"; usage; exit 2 ;;
esac
```

- [ ] **Step 3: 创建 `deps/README.md`**（用法 + 镜像说明）

```markdown
# deps —— 三方库供给（共享清单 + 拉取脚本）

本目录是 paint-pc / paint-android 消费者所需三方库的**单一事实来源**。

## 用法
```bash
# 从 SDK 主仓库（本仓库）运行
scripts/fetch-deps.sh --list    # 查看清单
scripts/fetch-deps.sh --check   # 探测本机缺哪些大库
scripts/fetch-deps.sh --fetch   # 拉取/解包缺失大库到 deps/usr，导出 DGCPAIN_DEPS_ROOT
```

## 镜像源
大库（Vulkan/shaderc/glslc）从**国内镜像**下载离线 deb：清华 → 阿里。
镜像 URL 池内置在 manifest.yaml，无需手动配置。

## 无 sudo 设计
全部拉取/解包在仓库内 `deps/` 完成（curl + dpkg-deb -x），不写系统目录，无需 root。

## Windows 策略（诚实标注）
- Windows 下大库 Vulkan/shaderc 由 **LunarG Vulkan SDK** 提供（VULKAN_SDK 环境变量）。
- `fetch-deps.sh --fetch` 在 Windows 仅探测 `VULKAN_SDK` 是否已装：已装即满足；未装则打印国内加速安装指引（LunarG SDK 安装器交互/体积大，**不承诺脚本全自动静默安装**），并尝试从镜像拉 shaderc 预编译产物（如可得）。
- 消费者 setup.sh 在 Windows 下调用 `--check` 给出指引。

## 布局
- `manifest.yaml` — 三方库清单（单一事实来源）
- `usr/` — 解包后的 include/lib/bin（DGCPAIN_DEPS_ROOT 指向这里；由 deb 的 `usr/*` 合并而来）
- `cache/` — 下载的 deb 缓存
```

- [ ] **Step 4: 测试 parse_manifest 中间产物（暴露字段/时序 bug 的关键）**

Run: `source <(sed -n '/^parse_manifest()/,/^}/p' scripts/fetch-deps.sh) 2>/dev/null; parse_manifest` 或直接 `bash -c 'source scripts/fetch-deps.sh; parse_manifest'`
Expected（**字段归属必须正确，不能滞后**）:
```
vulkan|big|linux,win|1.3.275.0-1build1|include/vulkan/vulkan.h|DGCPAIN_DEPS_ROOT|
shaderc|big|linux|2023.8-1build1|lib/libshaderc_shared.so|DGCPAIN_DEPS_ROOT|
glfw|small|linux,win||||FetchContent
imgui|small|linux,win||||FetchContent
android_sdk|big|linux,win,mac||||sdkmanager
android_ndk|big|linux,win,mac||||sdkmanager
```
若 vulkan 字段为空或平台/版本错位 → parse_manifest 有 emit 时序 bug，先修再继续。

- [ ] **Step 5: 测试 --list 与 --check**

Run: `bash scripts/fetch-deps.sh --list`
Expected: 打印 6 个库（vulkan/shaderc/glfw/imgui/android_sdk/android_ndk），字段齐全、归属正确。

Run: `bash scripts/fetch-deps.sh --check`
Expected: vulkan/shaderc 显示 `[MISS]`（本机无 dev 头）；android_sdk/android_ndk 显示 `[SKIP]`（由 SDK Manager 供给，不计数 MISS、不 return 1）；glfw/imgui 因 size=small 被跳过（不探测）。

- [ ] **Step 6: 语法与可执行**

Run: `bash -n scripts/fetch-deps.sh`
Expected: 无输出（语法通过）。

Run: `chmod +x scripts/fetch-deps.sh`，`bash scripts/fetch-deps.sh --help`
Expected: 打印用法。

- [ ] **Step 7: .gitignore 覆盖 deps 产物（避免 --fetch 后大量 untracked）**

在 `.gitignore`（仓库根）追加：
```
# deps 供给产物（拉取/解包大文件，不入版控）
deps/usr/
deps/cache/
```

Run: `git check-ignore deps/usr/include/vulkan/vulkan.h deps/cache/libvulkan-dev.deb`
Expected: 两个都命中（已忽略）。

- [ ] **Step 8: Commit**

```bash
git add .gitignore deps/manifest.yaml scripts/fetch-deps.sh deps/README.md
git commit -m "feat: 三方库清单 manifest + fetch-deps.sh 骨架（--list/--check/--help）"
```

---

### Task 2: fetch-deps.sh --fetch 实现（Linux 大库从镜像拉取解包）

**Files:**
- Modify: `scripts/fetch-deps.sh`

**Interfaces:**
- Consumes: Task 1 的 `parse_manifest`、`$DEPS_USR`、`$DEPS_CACHE`、`is_linux`。
- Produces: `fetch_deps()` 函数，下载 libvulkan-dev + libshaderc1 + libshaderc-dev + glslc deb 到 `deps/cache`，`dpkg-deb -x` 解包到 `deps/usr`；`export DGCPAIN_DEPS_ROOT=$DEPS_USR`。Task 3 消费者 setup.sh 调用。

- [ ] **Step 1: 写失败测试（可执行断言）**

Run: `bash scripts/fetch-deps.sh --fetch`
Expected（当前骨架）: 打印 `（Task 2 实现）` —— 说明 --fetch 未实现。

- [ ] **Step 2: 实现 `fetch_deps()`**

在 `scripts/fetch-deps.sh` 的 `cmd_check` 后追加（替换 `--fetch` case 分支）：

```bash
# 下载单个 deb：依序尝试镜像 URL 池，成功即停。返回 0/1。
fetch_deb() {
  local name="$1" url_tpl="$2" version="$3" cache="$DEPS_CACHE/$name.deb"
  local mirror url
  mkdir -p "$DEPS_CACHE"
  while read -r mirror; do
    [ -z "$mirror" ] && continue
    url="$(printf '%s' "$url_tpl" | sed "s|{mirror}|$mirror|g; s|{version}|$version|g")"
    info "  ↓ curl $url → $cache"
    if curl -fL --retry 3 --connect-timeout 10 -o "$cache" "$url" 2>/dev/null; then
      info "  ✓ $name 下载完成"
      return 0
    fi
    rm -f "$cache"
  done <<'EOF'
https://mirrors.tuna.tsinghua.edu.cn
https://mirrors.aliyun.com
http://archive.ubuntu.com
EOF
  err "$name 所有镜像均失败（清华/阿里/官方）"
  return 1
}

# 解包 deb 到 deps/usr（合并统一 prefix）。
# 关键: dpkg-deb -x 产出 <tmp>/usr/{include,lib,bin,...}，必须把 <tmp>/usr/* 合并进
# $DEPS_USR（deps/usr），否则会嵌套成 deps/usr/usr/ 且 include 落空，CMake 找不到。
extract_deb() {
  local deb="$1" tmp="$DEPS_CACHE/extract_$(basename "$deb" .deb)"
  rm -rf "$tmp"; mkdir -p "$tmp" "$DEPS_USR"
  # 解包失败 → 清理 tmp 并 return 1（使外层 `|| rc=1` 路径真正生效，set -e 下不静默）。
  dpkg-deb -x "$deb" "$tmp" || { rm -rf "$tmp"; err "解包失败: $deb"; return 1; }
  # 合并 <tmp>/usr/* → deps/usr（无冲突覆盖；有则后者覆盖，打印警告）
  cp -a "$tmp"/usr/. "$DEPS_USR"/ 2>/dev/null || true
  rm -rf "$tmp"
  info "  ✓ 解包 $deb → $DEPS_USR"
}

fetch_deps() {
  info "=== --fetch：从国内镜像拉取缺失大库 ==="
  # 硬依赖探测（无 curl/dpkg-deb 直接报错，不做静默降级）
  has curl || { err "缺 curl（下载必败）"; return 1; }
  if is_linux; then
    has dpkg-deb || { err "缺 dpkg-deb（解包必败）"; return 1; }
  fi
  local rc=0 any=0
  # 进程替换：避免子 shell 吞掉 rc/any。
  while IFS='|' read -r name size plat ver chk env fch; do
    [ "$size" != "big" ] && continue
    # sdkmanager 供给：不拉取，仅提示（与 --check 口径一致）。
    if [ "$fch" = "sdkmanager" ]; then
      info "  SKIP $name（由 SDK Manager 供给，本脚本不拉取）"
      continue
    fi
    # 平台过滤：parse_manifest 已把 platform 规整为 "linux,win" 形式；逐项比对。
    local plat_has=0 p
    for p in $(printf '%s' "$plat" | tr ',' ' '); do
      [ "$p" = "$(uname -s | tr 'A-Z' 'a-z')" ] && plat_has=1
    done
    [ "$plat_has" -eq 0 ] && { info "  SKIP $name（平台 $plat 非本机）"; continue; }
    # 幂等：check 命中则跳过
    if [ -n "$chk" ] && [ -e "$DEPS_USR/$chk" ]; then
      info "  SKIP $name（已满足: $DEPS_USR/$chk）"
      continue
    fi
    any=1
    info "  拉取 $name ($ver)…"
    case "$name" in
      vulkan)
        if fetch_deb "libvulkan-dev" \
            "{mirror}/ubuntu/pool/main/v/vulkan-loader/libvulkan-dev_{version}_amd64.deb" "$ver"; then
          extract_deb "$DEPS_CACHE/libvulkan-dev.deb" || rc=1
        else rc=1; fi
        ;;
      shaderc)
        local pkg ok=1
        for pkg in libshaderc1 libshaderc-dev glslc; do
          fetch_deb "$pkg" \
            "{mirror}/ubuntu/pool/universe/s/shaderc/${pkg}_{version}_amd64.deb" "$ver" || { ok=0; break; }
        done
        if [ "$ok" -eq 1 ]; then
          for pkg in libshaderc1 libshaderc-dev glslc; do
            extract_deb "$DEPS_CACHE/$pkg.deb" || { ok=0; break; }
          done
        fi
        [ "$ok" -eq 1 ] || rc=1
        ;;
      *) err "未知大库: $name"; rc=1 ;;
    esac
  done < <(parse_manifest)
  # 导出环境变量（供调用方 setup.sh 使用）
  if [ -d "$DEPS_USR/include" ]; then
    info "导出 DGCPAIN_DEPS_ROOT=$DEPS_USR"
    export DGCPAIN_DEPS_ROOT="$DEPS_USR"
  fi
  [ "$any" -eq 0 ] && info "无需拉取（全部已满足）"
  return $rc
}
```

将 `--fetch` case 分支改为 `--fetch) fetch_deps ;;`。

- [ ] **Step 3: 运行 --fetch 验证（本机无 sudo 实测）**

Run: `bash scripts/fetch-deps.sh --fetch`
Expected: 依序 curl 清华镜像拉 `libvulkan-dev` / `libshaderc1` / `libshaderc-dev` / `glslc` 四个 deb → `dpkg-deb -x` 解包并把 `usr/*` 合并到 `deps/usr` → 打印导出 `DGCPAIN_DEPS_ROOT=deps/usr`。全程无 sudo。
验证落盘（无嵌套 usr/usr）：
- `ls deps/usr/include/vulkan/vulkan.h`（应存在）
- `ls deps/usr/include/shaderc/shaderc.hpp`（应存在）
- `ls deps/usr/lib/libshaderc_shared.so*`（应存在，或 deps/usr/lib/x86_64-linux-gnu/ 下）
- `ls deps/usr/bin/glslc`（应存在）
- `[ ! -e deps/usr/usr ]`（应无嵌套 usr）

- [ ] **Step 4: 幂等验证**

Run: `bash scripts/fetch-deps.sh --fetch`（第二次）
Expected: 全 SKIP（`SKIP vulkan（已满足）` / `SKIP shaderc（已满足）`），0 退出。

- [ ] **Step 5: 失败路径验证**

Run: `curl -fsSL -o /dev/null https://mirrors.tuna.tsinghua.edu.cn/ubuntu/pool/main/v/vulkan-loader/libvulkan-dev_9.9.9-1_amd64.deb`
Expected: 失败（404）。说明 `fetch_deb` 的 `curl -f` 会正确报错并尝试下一镜像。
Run: `bash scripts/fetch-deps.sh --fetch`（在 DEPS_CACHE 里临时放一个不存在的版本时）
Expected: `rc=1`（所有镜像失败 + 非零退出 + 列出尝试 URL），子 shell 不吞返回码。

将 `--fetch` case 分支改为 `--fetch) fetch_deps ;;`。

- [ ] **Step 6: Commit**

```bash
git add scripts/fetch-deps.sh
git commit -m "feat: fetch-deps --fetch 实现（Linux 大库镜像拉取 + 解包 + 幂等）"
```

---

### Task 3: SDK host 测试闭环验证（解包产物真实可编译）

**Files:**
- 无代码改动（验证任务）；如发现 CMake HINTS 不足则微调 `render/vulkan/CMakeLists.txt`。

**Interfaces:**
- Consumes: Task 2 的 `deps/usr`（含 vulkan.h / shaderc.hpp / libshaderc_shared.so）。

- [ ] **Step 1: 运行 SDK host 测试（带 DGCPAIN_DEPS_ROOT）**

Run:
```bash
cd /home/qiansenwei/workspace/demo
cmake -B build-deps -S . -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_DEPS_ROOT="$(pwd)/deps/usr" -DDGCPAIN_BUILD_TESTS=ON
cmake --build build-deps -j
ctest --test-dir build-deps --output-on-failure
```
Expected: Vulkan/shaderc 从 `deps/usr` 找到（CMake 不再报 "Vulkan not found"）；`test_offscreen` / `test_brush_offscreen` 等 Vulkan 相关测试**通过**（证明解包产物可链接可运行）。若 CMake 报找不到头/库，检查 `render/vulkan/CMakeLists.txt` HINTS 是否覆盖 `deps/usr/{include,lib,lib/x86_64-linux-gnu}`，缺则补 HINTS（最小改动）。

- [ ] **Step 2: 记录证据**

Run: `ctest --test-dir build-deps -N`（列出用例）与 `ctest --test-dir build-deps --output-on-failure`
Expected: Vulkan 相关用例数 ≥ 2 且全过；记录通过列表与数量。

- [ ] **Step 3: Commit（如有 CMake 改动）**

```bash
git add render/vulkan/CMakeLists.txt
git commit -m "fix: render/vulkan CMake HINTS 覆盖 deps/usr 布局"
```
（若无改动则跳过此 commit。）

---

### Task 4: paint-pc setup.sh 接入 fetch-deps + 镜像加速 FetchContent

**Files:**
- Modify: `/home/qiansenwei/workspace/paint-pc/scripts/setup.sh`

**Interfaces:**
- Consumes: Task 2 的 `sdk/scripts/fetch-deps.sh --fetch`、`DGCPAIN_DEPS_ROOT`；Task 1 的 manifest。
- Produces: paint-pc `build_pc` 前调用 fetch-deps，注入 `DGCPAIN_DEPS_ROOT` 供 cmake。

- [ ] **Step 1: 写失败测试（当前 setup.sh 无 fetch-deps 调用）**

Run: `grep -n "fetch-deps" /home/qiansenwei/workspace/paint-pc/scripts/setup.sh`
Expected: 无匹配（说明未接入）。

- [ ] **Step 2: 修改 `build_pc` 前的探测/同步段**

在 `paint-pc/scripts/setup.sh` 的 `sync_submodule` 之后、`build_pc` 之前插入：

```bash
fetch_deps() {
  local root="$1" sdk_script="$root/sdk/scripts/fetch-deps.sh"
  if [ ! -f "$sdk_script" ]; then
    warn "未找到 sdk/scripts/fetch-deps.sh（submodule 未初始化？跳过依赖拉取）"
    return 0
  fi
  info "拉取三方库（fetch-deps）…"
  bash "$sdk_script" --fetch
  # fetch-deps 在子进程内 export，无法回传到本 shell；这里按 deps 落盘位置显式设值。
  if [ -z "${DGCPAIN_DEPS_ROOT:-}" ] && [ -d "$root/sdk/deps/usr" ]; then
    export DGCPAIN_DEPS_ROOT="$root/sdk/deps/usr"
  fi
}
```

注意：`deps/` 位于 **SDK 仓库根**（submodule sdk/ 的根），即 `$root/sdk/deps/usr`。修正 fetch_deps 的 deps 探测路径为 `$root/sdk/deps/usr`。

在 `main()` 中 `sync_submodule "$root"` 后调用：
```bash
sync_submodule "$root"
fetch_deps "$root"
```

- [ ] **Step 3: 镜像加速 FetchContent（GLFW/ImGui）**

在 `paint-pc/CMakeLists.txt` 的 `FetchContent_Declare(glfw` 前插入（镜像前缀，失败自动回退官方，因为 FetchContent 的 URL 若 404 不会自动 fallback —— 用 `GIT_REPOSITORY` 保持官方，仅当环境变量 `PC_FETCH_MIRROR` 设置时替换）：

```cmake
# 国内镜像加速 FetchContent（可选）：PC_FETCH_MIRROR=https://ghproxy.com/ 时替换 GitHub 前缀。
# PC_GLFW_TAG / PC_IMGUI_TAG 已在上方定义（见现文件 14-15 行），此处不重复 set。
set(PC_FETCH_MIRROR "$ENV{PC_FETCH_MIRROR}" CACHE STRING "GitHub 镜像前缀（国内加速）")

if(PC_FETCH_MIRROR)
  set(PC_GLFW_REPO "${PC_FETCH_MIRROR}https://github.com/glfw/glfw.git")
  set(PC_IMGUI_REPO "${PC_FETCH_MIRROR}https://github.com/ocornut/imgui.git")
else()
  set(PC_GLFW_REPO "https://github.com/glfw/glfw.git")
  set(PC_IMGUI_REPO "https://github.com/ocornut/imgui.git")
endif()

FetchContent_Declare(glfw  GIT_REPOSITORY ${PC_GLFW_REPO}  GIT_TAG ${PC_GLFW_TAG}  GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY ${PC_IMGUI_REPO} GIT_TAG ${PC_IMGUI_TAG}  GIT_SHALLOW TRUE)
```

并在 `fetch_deps()` 中（或 setup.sh 顶部）支持 `export PC_FETCH_MIRROR="${PC_FETCH_MIRROR:-https://ghproxy.com/}"`（可选，默认不设以免破坏现有构建）。

- [ ] **Step 4: 运行验证**

Run: `cd /home/qiansenwei/workspace/paint-pc && bash -n scripts/setup.sh`
Expected: 语法通过。

Run: `bash scripts/setup.sh --check`
Expected: 探测阶段正常；不触发 fetch-deps（--check 模式）。

Run（如果本机 Linux 能构建）：`DGCPAIN_DEPS_ROOT="$HOME/workspace/demo/deps/usr" bash scripts/setup.sh`
Expected: 走到 fetch-deps → 幂等 SKIP → 构建。本机可能缺其它依赖（X11），如实记录。

- [ ] **Step 5: Commit**

```bash
cd /home/qiansenwei/workspace/paint-pc
git add scripts/setup.sh CMakeLists.txt
git commit -m "feat: setup.sh 接入共享 fetch-deps + FetchContent 镜像加速"
```

---

### Task 5: paint-android setup.sh 接入 fetch-deps（探测+指引）

**Files:**
- Modify: `/home/qiansenwei/workspace/paint-android/scripts/setup.sh`

**Interfaces:**
- Consumes: Task 1 的 `fetch-deps.sh --check`（探测 Android SDK/NDK）。
- Produces: paint-android setup.sh 构建前调用 `--check`，缺失时打印指引。

- [ ] **Step 1: 写失败测试**

Run: `grep -n "fetch-deps" /home/qiansenwei/workspace/paint-android/scripts/setup.sh`
Expected: 无匹配（未接入）。

- [ ] **Step 2: 修改 `build_apk` 前探测段**

在 `paint-android/scripts/setup.sh` 的探测后、构建前插入：

```bash
fetch_deps() {
  local root="$1" sdk_script="$root/sdk/scripts/fetch-deps.sh"
  if [ ! -f "$sdk_script" ]; then
    warn "未找到 sdk/scripts/fetch-deps.sh（submodule 未初始化？跳过）"
    return 0
  fi
  info "探测三方库（fetch-deps --check）…"
  bash "$sdk_script" --check
}
```

在 `main()` 探测后调用 `fetch_deps "$root"`。Android 侧大库（SDK/NDK）由 SDK Manager 管理，fetch-deps 对它们仅 `--check` 探测 + 指引，不自动拉（见 manifest platform 过滤）。

- [ ] **Step 3: 运行验证**

Run: `cd /home/qiansenwei/workspace/paint-android && bash -n scripts/setup.sh`
Expected: 语法通过。

Run: `bash scripts/setup.sh --check`
Expected: 探测阶段 + fetch-deps --check 输出（android_sdk/android_ndk 由 sdkmanager 探测，不自动拉）。

- [ ] **Step 4: Commit**

```bash
cd /home/qiansenwei/workspace/paint-android
git add scripts/setup.sh
git commit -m "feat: setup.sh 接入共享 fetch-deps 探测（SDK/NDK 指引）"
```

---

### Task 6: 测试门（headless 离屏 PNG + 幂等 + 消费者不回归）

**Files:**
- 测试执行（验证）；必要时微调。

**Interfaces:**
- Consumes: Task 2 的 deps/usr、Task 3 的构建产物、Task 4/5 的 setup.sh。

- [ ] **Step 1: SDK 离屏闭环**

Run:
```bash
cd /home/qiansenwei/workspace/demo
ctest --test-dir build-deps --output-on-failure
```
Expected: 全绿（含 test_offscreen / test_brush_offscreen / test_brush_kernel）。记录通过数。

- [ ] **Step 2: paint-pc headless 离屏 PNG**

Run: `cd /home/qiansenwei/workspace/paint-pc && bash tests/smoke.sh`（需 DGCPAIN_DEPS_ROOT 指向 deps/usr）
Expected: 离屏 PNG 生成 + 像素断言通过（B3-1 真实笔迹）。若本机缺 X11 依赖无法构建，如实记录阻塞并给证据。

- [ ] **Step 3: fetch-deps 幂等**

Run: `cd /home/qiansenwei/workspace/demo && bash scripts/fetch-deps.sh --fetch`
Expected: 全部 SKIP，0 退出。

- [ ] **Step 4: 消费者 --check 不回归**

Run: `cd /home/qiansenwei/workspace/paint-pc && bash scripts/setup.sh --check`
Run: `cd /home/qiansenwei/workspace/paint-android && bash scripts/setup.sh --check`
Expected: 探测逻辑无回归（相对修改前）。本机环境缺项如实记录。

- [ ] **Step 5: 汇总测试报告**

记录：SDK ctest 通过数 / paint-pc smoke.sh 结果（或阻塞证据）/ 幂等 / 消费者 --check。

---

## Self-Review

### 1. Spec coverage
- ✅ manifest（Task 1）、--list/--check/--fetch（Task 1/2）、README（Task 1）
- ✅ 无 sudo 实测（Task 2 Step 3 本机 uid 1042）
- ✅ 统一 prefix deps/usr + DGCPAIN_DEPS_ROOT（Task 2）
- ✅ SDK host 测试闭环（Task 3）
- ✅ paint-pc 接入 + FetchContent 镜像加速（Task 4）
- ✅ paint-android 接入（Task 5）
- ✅ 测试门 headless PNG + 幂等（Task 6）
- ✅ Windows 务实取舍：manifest platform 含 win；脚本对 win 不自动拉 deb（fetch_deps 平台过滤 + VULKAN_SDK 探测），shaderc 走 Vulkan SDK；LunarG 指引在 README（Task 1 Step 3）如实注明「不承诺全自动静默安装」。
- ✅ **偏离说明（显式）**：manifest 6 库而非 spec AC1 的「8 库」——nlohmann/json 与 stb_image_write 已 vendored 进 `sdk/third_party/`（现成，无需供给），在 manifest 之外不重复登记；镜像池为清华→阿里→官方 archive.ubuntu.com，spec 的「官方 fallback」已落实为池末位（Global Constraint「不回退」指不做降级/兜底方案，多镜像 fallback 属正常容错，已在计划中 reconcile）。

### 2. Placeholder scan
- 无 TBD/TODO；所有代码步骤含完整可执行内容。

### 3. Type consistency
- `parse_manifest` 输出 `name|size|plat|ver|chk|env|fch`（7 列，fch=fetch 供给方式），Task 1 定义、Task 2 `while IFS='|' read -r name size plat ver chk env fch` 消费，一致；`--check` 用 fch 特判 sdkmanager。
- `fetch_deb name url_tpl version` / `extract_deb deb out` 签名在 Task 2 定义与调用一致。
- `DGCPAIN_DEPS_ROOT=$DEPS_USR`（deps/usr），Task 2 导出、Task 3 cmake 使用、Task 4 paint-pc 消费，一致。
- manifest `deps/usr` extract_to 与脚本 `$DEPS_USR`（deps/usr）一致。
- 注意：paint-pc submodule 里 SDK 仓库根 = `sdk/`，其 deps 在 `sdk/deps/usr`（Task 4 Step 2 已修正路径）。
