#!/usr/bin/env bash
# =============================================================================
# setup-env.sh — libdgc_paint SDK 一键环境搭建脚本（E0-2）
#
# 用法:
#   scripts/setup-env.sh          默认模式：探测 + 硬依赖缺失时尝试补缺（Linux/apt）
#   scripts/setup-env.sh --check  只探测不安装，输出缺项清单
#   scripts/setup-env.sh --help   打印用法
#
# 口径来源: docs/env/env-setup.md §1（E0-1 探测清单，单一事实来源）
# 依赖分级: 硬依赖缺失 → 非零退出并给安装指引；软依赖缺失 → 仅警告，0 退出。
# =============================================================================
set -euo pipefail

# ---------- 输出辅助 ----------
info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }

# ---------- 基础工具 ----------
has() { command -v "$1" >/dev/null 2>&1; }

is_linux() { [ "$(uname -s)" = "Linux" ]; }

# 从输出字符串提取首个点分数字版本号，如 "cmake version 3.22.1" -> "3.22.1"
extract_version() {
  local s="$1"
  if [[ "$s" =~ ([0-9]+(\.[0-9]+)+) ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  elif [[ "$s" =~ ([0-9]+) ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  fi
}

# 取点分版本号第 idx 段（0 起），缺失/非数字按 0 处理
ver_seg() {
  local v="$1" idx="$2" seg
  seg="$(printf '%s' "$v" | cut -d. -f"$((idx + 1))" 2>/dev/null || true)"
  case "$seg" in
    ''|*[!0-9]*) printf '0' ;;
    *) printf '%d' "$((10#$seg))" ;;
  esac
}

# 点分版本比较：a >= b 返回 0，否则返回 1
ver_ge() {
  local a="$1" b="$2" i sa sb
  for i in 0 1 2 3; do
    sa="$(ver_seg "$a" "$i")"
    sb="$(ver_seg "$b" "$i")"
    if [ "$sa" -gt "$sb" ]; then return 0; fi
    if [ "$sa" -lt "$sb" ]; then return 1; fi
  done
  return 0
}

# ---------- 探测结果存储 ----------
CHK_NAMES=()
CHK_LEVELS=()
CHK_STATUS=()
CHK_DETAILS=()
HARD_MISS=0
SOFT_MISS=0

record() { # name level status detail
  CHK_NAMES+=("$1")
  CHK_LEVELS+=("$2")
  CHK_STATUS+=("$3")
  CHK_DETAILS+=("$4")
}

# ---------- 各检查项（与 docs/env/env-setup.md §1 逐项对齐） ----------
probe_cmake() {
  local v vn
  if ! has cmake; then
    record "cmake" "硬" "MISS(硬)" "未安装（configure 必败）"
    HARD_MISS=$((HARD_MISS + 1)); return
  fi
  v="$(cmake --version 2>/dev/null | head -n1 || true)"
  vn="$(extract_version "$v")"
  if [ -n "$vn" ]; then
    if ver_ge "$vn" "3.22"; then
      record "cmake" "硬" "OK" "cmake $vn（≥ 3.22）"
    else
      record "cmake" "硬" "MISS(硬)" "cmake $vn 过旧（需 ≥ 3.22，建议 3.31+）"
      HARD_MISS=$((HARD_MISS + 1))
    fi
  else
    record "cmake" "硬" "OK" "已安装但版本无法确认（请手动确认 ≥ 3.22）"
  fi
}

probe_ninja() {
  local v
  if ! has ninja; then
    record "ninja" "硬" "MISS(硬)" "未安装（build 必败）"
    HARD_MISS=$((HARD_MISS + 1)); return
  fi
  v="$(ninja --version 2>/dev/null | head -n1 || true)"
  record "ninja" "硬" "OK" "ninja ${v:-（版本未知）}"
}

probe_compiler() {
  local v vn
  if ! has g++; then
    record "C++ 编译器 (g++)" "硬" "MISS(硬)" "未安装 g++（编译必败）"
    HARD_MISS=$((HARD_MISS + 1)); return
  fi
  v="$(g++ -dumpfullversion 2>/dev/null || g++ -dumpversion 2>/dev/null || true)"
  vn="$(extract_version "$v")"
  if [ -n "$vn" ]; then
    if ver_ge "$vn" "11"; then
      record "C++ 编译器 (g++)" "硬" "OK" "g++ $vn（GCC ≥ 11）"
    else
      record "C++ 编译器 (g++)" "硬" "MISS(硬)" "g++ $vn 过旧（需 GCC ≥ 11）"
      HARD_MISS=$((HARD_MISS + 1))
    fi
  else
    record "C++ 编译器 (g++)" "硬" "OK" "已安装但版本无法确认（请手动确认 ≥ 11）"
  fi
}

probe_vulkan() {
  local v vn
  if ! has vulkaninfo; then
    record "Vulkan" "软" "WARN(软)" "未安装 vulkaninfo（仅跑 Vulkan compute 需要）"
    SOFT_MISS=$((SOFT_MISS + 1)); return
  fi
  v="$(vulkaninfo --summary 2>/dev/null | grep -i -m1 'Vulkan Instance Version' || true)"
  v="${v##*: }"
  vn="$(extract_version "$v")"
  if [ -n "$vn" ]; then
    record "Vulkan" "软" "OK" "Vulkan $vn"
  else
    record "Vulkan" "软" "OK" "vulkaninfo 可用（版本未知）"
  fi
}

probe_ndk() {
  local ndk rev vn
  ndk="${ANDROID_NDK_HOME:-}"
  if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
    record "Android NDK" "软" "WARN(软)" "\$ANDROID_NDK_HOME 未设置或目录不存在（仅 android-arm64 preset 需要）"
    SOFT_MISS=$((SOFT_MISS + 1)); return
  fi
  rev="$(sed -n 's/^Pkg.Revision[[:space:]]*=[[:space:]]*//p' "$ndk/source.properties" 2>/dev/null || true)"
  if [ -n "$rev" ]; then
    vn="$(extract_version "$rev")"
    if ver_ge "$vn" "27"; then
      record "Android NDK" "软" "OK" "NDK r$rev @ $ndk（r27+，建议 r28）"
    else
      record "Android NDK" "软" "WARN(软)" "NDK r$rev 过旧 @ $ndk（建议 r27+/r28）"
      SOFT_MISS=$((SOFT_MISS + 1))
    fi
  else
    record "Android NDK" "软" "OK" "NDK @ $ndk（无法读取版本，请确认 r27+）"
  fi
}

probe_glslc() {
  local v
  if ! has glslc; then
    record "glslc" "软" "WARN(软)" "未安装（随 NDK / Vulkan SDK 提供，仅编译 shader 需要）"
    SOFT_MISS=$((SOFT_MISS + 1)); return
  fi
  v="$(glslc --version 2>/dev/null | head -n1 || true)"
  record "glslc" "软" "OK" "${v:-glslc 可用（版本未知）}"
}

probe_all() {
  CHK_NAMES=(); CHK_LEVELS=(); CHK_STATUS=(); CHK_DETAILS=()
  HARD_MISS=0; SOFT_MISS=0
  probe_cmake
  probe_ninja
  probe_compiler
  probe_vulkan
  probe_ndk
  probe_glslc
}

# ---------- 输出 ----------
print_check() {
  info "=== 环境探测结果（口径见 docs/env/env-setup.md §1）==="
  local i
  for i in "${!CHK_NAMES[@]}"; do
    printf '[%s] %s: %s\n' "${CHK_STATUS[$i]}" "${CHK_NAMES[$i]}" "${CHK_DETAILS[$i]}"
  done
  echo
  if [ "$HARD_MISS" -gt 0 ]; then
    err "硬依赖缺失 $HARD_MISS 项，host 编译将失败。"
  else
    info "硬依赖齐全；软依赖缺失不阻断 host 编译（Null 后端不硬依赖）。"
  fi
}

print_manual_guidance() {
  info "请按 docs/env/env-setup.md 手动补缺："
  info "  - Linux 硬依赖安装：§2.2  sudo apt install build-essential cmake ninja-build"
  info "  - 补缺说明：§2.4（软依赖按需补 libvulkan-dev / NDK r27+ / glslc）"
  info "  - Windows：§3（在「Developer Command Prompt for VS 2026」内执行）"
}

print_help() {
  cat <<'EOF'
用法: scripts/setup-env.sh [--check] [--help]

  一键搭建 libdgc_paint SDK 编译环境（换机后快速补缺）。

选项:
  --check    只探测不安装，输出缺项清单；硬依赖缺失时非零退出。
  -h, --help 打印本帮助。

默认行为:
  探测 cmake / ninja / C++ 编译器 / Vulkan / NDK / glslc；
  硬依赖缺失时，在 Debian/Ubuntu 系（存在 apt-get）尝试 sudo 自动安装
  build-essential cmake ninja-build（需 root 权限，安装前会明确声明）；
  非 apt 系统或安装失败则打印 docs/env/env-setup.md 对应小节指引并非零退出；
  仅软依赖（NDK / Vulkan / glslc）缺失时仅警告，0 退出。

口径来源: docs/env/env-setup.md §1（E0-1 探测清单）。
EOF
}

# ---------- 安装 ----------
attempt_apt_install() {
  if ! has sudo; then
    warn "未找到 sudo，无法自动安装。"
    print_manual_guidance
    return 1
  fi
  info "检测到 Debian/Ubuntu 系（apt-get），将使用 sudo 自动安装硬依赖：build-essential cmake ninja-build"
  info "（需要 root 权限；如需输入密码请键入，或 Ctrl-C 取消后按 docs/env/env-setup.md §2.2 手动安装）"
  if ! sudo apt-get update; then
    warn "apt-get update 失败（可能未授权 sudo 或离线）。"
    print_manual_guidance
    return 1
  fi
  if ! sudo apt-get install -y build-essential cmake ninja-build; then
    warn "apt-get install 失败。"
    print_manual_guidance
    return 1
  fi
  return 0
}

# ---------- 主流程 ----------
main() {
  local mode="install"
  if [ "$#" -gt 1 ]; then
    err "参数过多：$*（用法: setup-env.sh [--check]）"
    exit 2
  fi
  if [ "$#" -eq 1 ]; then
    case "$1" in
      --check) mode="check" ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数：$1（用法: setup-env.sh [--check]）"; exit 2 ;;
    esac
  fi

  if ! is_linux; then
    warn "当前非 Linux 主机（$(uname -s)）。本脚本主战场为 Linux host，跨平台自动安装不属本任务范围。"
    info "Windows 请打开「Developer Command Prompt for VS 2026」按 docs/env/env-setup.md §3 手动探测/安装。"
    exit 1
  fi

  probe_all
  print_check

  if [ "$mode" = "check" ]; then
    if [ "$HARD_MISS" -gt 0 ]; then
      print_manual_guidance
      exit 1
    fi
    exit 0
  fi

  # 默认（安装）模式
  if [ "$HARD_MISS" -gt 0 ]; then
    if has apt-get; then
      if ! attempt_apt_install; then
        exit 1
      fi
      info "自动安装完成，重新探测…"
      probe_all
      print_check
      if [ "$HARD_MISS" -gt 0 ]; then
        err "自动安装后仍有硬依赖缺失，请按指引手动补缺。"
        exit 1
      fi
    else
      warn "未检测到 apt-get，无法自动安装。"
      print_manual_guidance
      exit 1
    fi
  fi

  info "环境就绪：硬依赖齐全，可开始构建（CMake 工程由 E0-3 落地）。"
  exit 0
}

main "$@"
