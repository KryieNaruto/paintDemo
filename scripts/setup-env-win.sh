#!/usr/bin/env bash
# =============================================================================
# setup-env-win.sh — libdgc_paint SDK Windows（Git Bash / MSYS2）一键搭建脚本
#
# 用法:
#   scripts/setup-env-win.sh            默认：探测 + 配置 host-windows + 构建 + ctest
#   scripts/setup-env-win.sh --check    只探测不安装，输出缺项清单
#   scripts/setup-env-win.sh --android  探测 + 配置 android-arm64 + 编 libdgc_paint.so
#   scripts/setup-env-win.sh --help     打印用法
#
# 前提: 在 Git Bash（MSYS2 / MINGW64）内运行；已装 Visual Studio 2026，含
#       「使用 C++ 的桌面开发」工作负载（MSVC + CMake + Ninja）。
# 原理: 脚本用 vswhere 定位 VS2026 的 vcvarsall.bat，经 cmd 抓取 MSVC 环境变量
#       （PATH / INCLUDE / LIB / LIBPATH）注入当前 bash 会话，使 cl.exe / cmake / ninja
#       在 Git Bash 内可直接使用（不必手动开 Developer Command Prompt）。
#
# 口径来源: docs/env/env-setup.md §1（E0-1 探测清单，单一事实来源）+ §3（Windows）。
# 依赖分级: 硬依赖缺失 → 非零退出并给安装指引；软依赖缺失 → 仅警告，0 退出。
# =============================================================================
set -euo pipefail

# ---------- 输出辅助 ----------
info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }

# ---------- 基础工具 ----------
has() { command -v "$1" >/dev/null 2>&1; }

is_windows_gitbash() {
  case "$(uname -s)" in
    MINGW*|MSYS*) return 0 ;;
    *) return 1 ;;
  esac
}

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

# Windows 环境变量路径规范化：C:\... 保留；/c/... POSIX 形式转 Windows 形式并重新 export
normalize_win_path_var() { # varname -> 输出 Windows 形式路径
  local name="$1" val
  val="${!name:-}"
  [ -z "$val" ] && return 1
  case "$val" in
    [A-Za-z]:*) ;; # 已是 C:\... Windows 形式
    /*) val="$(cygpath -w "$val")" ;;
  esac
  export "$name=$val"
  printf '%s' "$val"
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

# ---------- MSVC 环境注入（vswhere + vcvarsall） ----------
# vswhere 必须带 -prerelease，否则 2026 Insiders 被默认过滤。
find_vswhere() {
  local c
  for c in \
    "${LOCALAPPDATA:-}/Programs/Microsoft Visual Studio/Installer/vswhere.exe" \
    "/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"; do
    if [ -f "$c" ]; then printf '%s' "$c"; return 0; fi
  done
  if has vswhere; then printf '%s' "$(command -v vswhere)"; return 0; fi
  return 1
}

setup_msvc_env() {
  local vswhere vsdir vcvars_bash vcvars_win tmp line k v seg conv newpath
  vswhere="$(find_vswhere)" || {
    warn "未找到 vswhere.exe（VS2026 Installer 缺失或未装 VS）"
    return 1
  }
  vsdir="$("$vswhere" -prerelease -latest -products '*' \
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
      -property installationPath 2>/dev/null | tail -1)"
  [ -n "$vsdir" ] || {
    warn "vswhere 未返回 VS 安装路径（未装 VS2026 或缺 VC 工作负载）"
    return 1
  }
  vcvars_bash="$(cygpath -u "$vsdir")/VC/Auxiliary/Build/vcvarsall.bat"
  [ -f "$vcvars_bash" ] || {
    warn "找不到 vcvarsall.bat @ $vcvars_bash"
    return 1
  }
  vcvars_win="$(cygpath -w "$vcvars_bash")"

  tmp="$(mktemp)"
  # 经 cmd 抓取 vcvarsall x64 导出的环境变量（call ... && set 输出 VAR=value）
  cmd //c "call \"$vcvars_win\" x64 >nul 2>&1 && set" 2>/dev/null \
      | sed 's/\r$//' > "$tmp" || {
    rm -f "$tmp"; warn "vcvarsall 调用失败（$vcvars_win）"; return 1
  }
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    k="${line%%=*}"; v="${line#*=}"
    case "$k" in
      PATH)
        newpath=""
        local IFS=';'
        for seg in $v; do
          [ -z "$seg" ] && continue
          conv="$(cygpath -u "$seg" 2>/dev/null || printf '%s' "$seg")"
          newpath="$newpath:$conv"
        done
        export PATH="$newpath:$PATH"
        ;;
      INCLUDE|LIB|LIBPATH|VCToolsInstallDir|WindowsSdkDir|VisualStudioVersion)
        export "$k=$v"
        ;;
    esac
  done < "$tmp"
  rm -f "$tmp"

  if [ -z "${INCLUDE:-}" ]; then
    warn "MSVC 环境变量 INCLUDE 为空，vcvarsall 注入可能失败"
    return 1
  fi
  return 0
}

# ---------- 各检查项（与 docs/env/env-setup.md §1 / §3.3 对齐） ----------
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
    record "ninja" "硬" "MISS(硬)" "未安装（build 必败；VS 自带或独立装）"
    HARD_MISS=$((HARD_MISS + 1)); return
  fi
  v="$(ninja --version 2>/dev/null | head -n1 || true)"
  record "ninja" "硬" "OK" "ninja ${v:-（版本未知）}"
}

probe_cl() {
  local out v
  if ! has cl; then
    record "MSVC 编译器 (cl)" "硬" "MISS(硬)" "vcvarsall 注入后仍无 cl.exe（VC 工作负载缺失）"
    HARD_MISS=$((HARD_MISS + 1)); return
  fi
  out="$(cl 2>&1 | head -n1 || true)"
  v="$(printf '%s' "$out" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)"
  if [ -n "$v" ]; then
    record "MSVC 编译器 (cl)" "硬" "OK" "cl $v"
  else
    record "MSVC 编译器 (cl)" "硬" "OK" "cl 可用（版本未知）"
  fi
}

probe_vulkan() {
  local vk vkpos
  vk="${VULKAN_SDK:-}"
  if [ -z "$vk" ]; then
    record "Vulkan SDK" "软" "WARN(软)" "\$VULKAN_SDK 未设置（装 LunarG SDK 自动设置；仅跑 Vulkan compute 需要）"
    SOFT_MISS=$((SOFT_MISS + 1)); return
  fi
  vkpos="$(normalize_win_path_var VULKAN_SDK)"
  if [ -f "$(cygpath -u "$vkpos")/Include/vulkan/vulkan.h" ]; then
    record "Vulkan SDK" "软" "OK" "VULKAN_SDK @ $vkpos"
  else
    record "Vulkan SDK" "软" "WARN(软)" "VULKAN_SDK=$vkpos 但缺 Include/vulkan/vulkan.h"
    SOFT_MISS=$((SOFT_MISS + 1))
  fi
}

probe_ndk() {
  local ndk ndkpos rev
  ndk="${ANDROID_NDK_HOME:-}"
  if [ -z "$ndk" ]; then
    record "Android NDK" "软" "WARN(软)" "\$ANDROID_NDK_HOME 未设置（仅 --android 需要）"
    SOFT_MISS=$((SOFT_MISS + 1)); return
  fi
  ndkpos="$(normalize_win_path_var ANDROID_NDK_HOME)"
  rev="$(sed -n 's/^Pkg.Revision[[:space:]]*=[[:space:]]*//p' \
        "$(cygpath -u "$ndkpos")/source.properties" 2>/dev/null || true)"
  if [ -n "$rev" ]; then
    record "Android NDK" "软" "OK" "NDK r$rev @ $ndkpos（r27+，建议 r28）"
  else
    record "Android NDK" "软" "WARN(软)" "NDK @ $ndkpos（读不到版本，确认 r27+）"
    SOFT_MISS=$((SOFT_MISS + 1))
  fi
}

probe_glslc() {
  local vk
  if has glslc; then
    record "glslc" "软" "OK" "$(glslc --version 2>/dev/null | head -n1 || echo glslc 可用)"
    return
  fi
  vk="${VULKAN_SDK:-}"
  if [ -n "$vk" ] && [ -f "$(cygpath -u "$vk")/Bin/glslc.exe" ]; then
    record "glslc" "软" "OK" "LunarG SDK Bin/glslc.exe"
    return
  fi
  record "glslc" "软" "WARN(软)" "未找到（随 LunarG SDK / NDK 提供，仅编译 shader 需要）"
  SOFT_MISS=$((SOFT_MISS + 1))
}

probe_all() {
  CHK_NAMES=(); CHK_LEVELS=(); CHK_STATUS=(); CHK_DETAILS=()
  HARD_MISS=0; SOFT_MISS=0
  probe_cmake
  probe_ninja
  probe_cl
  probe_vulkan
  probe_ndk
  probe_glslc
}

# ---------- 输出 ----------
print_check() {
  info "=== 环境探测结果（口径见 docs/env/env-setup.md §1 / §3）==="
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
  info "  - VS2026 + 「使用 C++ 的桌面开发」工作负载（含 MSVC/CMake/Ninja）：§3.2"
  info "  - Vulkan（软）：装 LunarG Vulkan SDK（vulkan.lunarg.com），装后 \$VULKAN_SDK 自动设置：§3.1"
  info "  - NDK（软）：Android Studio → SDK Manager → SDK Tools 装 ndk;28.x，设 \$ANDROID_NDK_HOME：§3.2"
}

print_help() {
  cat <<'EOF'
用法: scripts/setup-env-win.sh [--check] [--android] [--help]

  Windows（Git Bash / MSYS2）一键搭建 libdgc_paint SDK 编译环境。
  自动注入 MSVC 环境（vswhere + vcvarsall），无需手动开 Developer Command Prompt。

选项:
  --check    只探测不安装，输出缺项清单；硬依赖缺失时非零退出。
  --android  探测 + 配置 android-arm64 + 编 libdgc_paint.so（需 $ANDROID_NDK_HOME）。
  -h, --help 打印本帮助。

默认行为:
  探测 cmake / ninja / MSVC(cl) / Vulkan / NDK / glslc；
  硬依赖齐全时：配置 host-windows（探测到 $VULKAN_SDK → 开 Vulkan 后端，否则回退
  DGCPAIN_RENDER_VULKAN=OFF 用 Null 后端）→ 构建 → 运行 ctest。

口径来源: docs/env/env-setup.md §1（E0-1 探测清单）+ §3（Windows）。
EOF
}

# ---------- 构建 ----------
build_host() {
  local vulkan_flag=OFF
  if [ -n "${VULKAN_SDK:-}" ]; then vulkan_flag=ON; fi
  info "配置 host-windows（DGCPAIN_RENDER_VULKAN=$vulkan_flag）…"
  cmake --preset host-windows "-DDGCPAIN_RENDER_VULKAN=$vulkan_flag" || return 1
  info "构建 …"
  cmake --build --preset host-windows || return 1
  info "运行 ctest …"
  ctest --test-dir build/host-windows --output-on-failure || return 1
  return 0
}

build_android() {
  if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    err "ANDROID_NDK_HOME 未设置（仅编 android-arm64 需要）。"
    return 1
  fi
  info "配置 android-arm64 …"
  cmake --preset android-arm64 || return 1
  info "构建 …"
  cmake --build --preset android-arm64 || return 1
  local so
  so="$(find build/android-arm64 -name 'libdgc_paint.so' | head -1)"
  if [ -z "$so" ]; then
    err "未找到 libdgc_paint.so"
    return 1
  fi
  info "产出: $so"
  file "$so"
  return 0
}

# ---------- 主流程 ----------
main() {
  local mode="install"
  if [ "$#" -gt 1 ]; then
    err "参数过多：$*（用法: setup-env-win.sh [--check] [--android]）"
    exit 2
  fi
  if [ "$#" -eq 1 ]; then
    case "$1" in
      --check)  mode="check" ;;
      --android) mode="android" ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数：$1（用法: setup-env-win.sh [--check] [--android]）"; exit 2 ;;
    esac
  fi

  if ! is_windows_gitbash; then
    warn "当前非 Git Bash / MSYS2（$(uname -s)）。Windows 一键搭建请在 Git Bash 内运行本脚本；"
    info "Linux host 请用 scripts/setup-env.sh。"
    exit 1
  fi

  # 关闭 MSYS 对 Windows exe 参数的自动路径转换（保持 C:\... / -D... 原样）
  export MSYS_NO_PATHCONV=1
  export MSYS2_ARG_CONV_EXCL='*'

  info "定位 Visual Studio MSVC 环境（vswhere + vcvarsall）…"
  if ! setup_msvc_env; then
    err "无法注入 MSVC 环境。请确认 VS2026 已装「使用 C++ 的桌面开发」；"
    err "或在「Developer Command Prompt for VS 2026」内手动运行（docs/env/env-setup.md §3.3）。"
    exit 1
  fi
  info "MSVC 环境已注入当前会话。"

  probe_all
  print_check

  if [ "$mode" = "check" ]; then
    if [ "$HARD_MISS" -gt 0 ]; then
      print_manual_guidance
      exit 1
    fi
    exit 0
  fi

  if [ "$HARD_MISS" -gt 0 ]; then
    err "硬依赖缺失 $HARD_MISS 项，无法一键构建。请按指引补缺后重跑。"
    print_manual_guidance
    exit 1
  fi

  if [ "$mode" = "android" ]; then
    build_android || exit 1
    info "完成：android-arm64 libdgc_paint.so 已编出。"
    exit 0
  fi

  build_host || exit 1
  info "环境就绪：host-windows 构建 + 测试通过。"
  exit 0
}

main "$@"
