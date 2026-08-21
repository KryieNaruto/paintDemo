#!/usr/bin/env bash
# 探测编译本仓库 libdgc_paint SDK 所需的 host 工具链。
# 缺 NDK / Vulkan / GLFW / JDK 不得导致失败。
# 版本下限与退出码见 docs/env/toolchain.md（与下列常量相同）。

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=22

set -u

usage() {
  cat <<EOF
用法: check-env.sh [-h|--help]
  探测编译本仓库 libdgc_paint SDK 所需的 host 工具链。
  从任意目录调用均可（以脚本位置定位仓库根）。

  必需（缺则 exit 1）：CMake >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}、Ninja、C++ 编译器
  可选（缺则 WARN，仍 exit 0）：ANDROID_NDK_HOME / Vulkan
  不作为失败项：GLFW / JDK / Android Studio（消费者侧，见 docs/git/）

  退出码:
    0  必需项齐（NDK/Vulkan 可仅 WARN）
    1  缺 cmake / cmake < ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} / 缺 ninja / 缺 C++ 编译器
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

REQUIRED_FAIL=0

log() {
  local st="$1"
  local name="$2"
  local detail="$3"
  printf '%-4s  %-8s  %s\n' "$st" "$name" "$detail"
}

fail() {
  log FAIL "$1" "$2"
  REQUIRED_FAIL=1
}

first_line() {
  local text="$1"
  printf '%s' "${text%%$'\n'*}"
}

command_path() {
  command -v "$1" 2>/dev/null || true
}

echo "SDK root: ${SDK_ROOT}"

# --- CMake（必需）---
CMAKE_BIN="$(command_path cmake)"
if [[ -z "$CMAKE_BIN" ]]; then
  fail cmake "not found (need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR})"
else
  cmake_out="$(cmake --version 2>&1)" || true
  cmake_line="$(first_line "$cmake_out")"
  cmake_major=""
  cmake_minor=""
  if [[ "$cmake_line" =~ ([0-9]+)\.([0-9]+) ]]; then
    cmake_major="${BASH_REMATCH[1]}"
    cmake_minor="${BASH_REMATCH[2]}"
  fi
  if [[ -z "$cmake_major" ]]; then
    fail cmake "unreadable version: ${cmake_line} (${CMAKE_BIN})"
  else
    cmake_ok=0
    if (( cmake_major > CMAKE_MIN_MAJOR )); then
      cmake_ok=1
    elif (( cmake_major == CMAKE_MIN_MAJOR && cmake_minor >= CMAKE_MIN_MINOR )); then
      cmake_ok=1
    fi
    if (( cmake_ok )); then
      log OK cmake "${cmake_major}.${cmake_minor} (${CMAKE_BIN})"
    else
      fail cmake "${cmake_major}.${cmake_minor} < ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} (${CMAKE_BIN})"
    fi
  fi
fi

# --- Ninja（必需）---
NINJA_BIN="$(command_path ninja)"
if [[ -z "$NINJA_BIN" ]]; then
  NINJA_BIN="$(command_path ninja-build)"
fi
if [[ -z "$NINJA_BIN" ]]; then
  fail ninja "not found (tried ninja, ninja-build)"
else
  ninja_out="$("$NINJA_BIN" --version 2>&1)" || true
  ninja_line="$(first_line "$ninja_out")"
  if [[ -n "$ninja_line" ]]; then
    log OK ninja "${ninja_line} (${NINJA_BIN})"
  else
    log OK ninja "${NINJA_BIN}"
  fi
fi

# --- C++ 编译器（必需；不编译测试 .cpp）---
cxx_ok=0
if [[ -n "${CXX:-}" ]]; then
  cxx_candidates=("$CXX")
else
  cxx_candidates=(g++ clang++ cl)
fi

for cand in "${cxx_candidates[@]}"; do
  cand_bin="$(command_path "$cand")"
  if [[ -z "$cand_bin" ]]; then
    continue
  fi
  base="$(basename "$cand_bin")"
  base="${base%.exe}"
  # MSVC cl: command -v 即可，不要无参数调用 cl（常非零退出）。
  if [[ "$base" == "cl" ]]; then
    cxx_ok=1
    log OK cxx "${cand_bin}"
    break
  fi
  if "$cand_bin" --version >/dev/null 2>&1 || "$cand_bin" -v >/dev/null 2>&1; then
    cxx_ok=1
    cxx_out="$("$cand_bin" --version 2>&1)" || true
    cxx_line="$(first_line "$cxx_out")"
    if [[ -n "$cxx_line" ]]; then
      log OK cxx "${cxx_line} (${cand_bin})"
    else
      log OK cxx "${cand_bin}"
    fi
    break
  fi
done

if (( ! cxx_ok )); then
  if [[ -n "${CXX:-}" ]]; then
    fail cxx "CXX=${CXX} not found or not runnable"
  else
    fail cxx "not found (tried g++, clang++, cl)"
  fi
fi

# --- NDK（可选）---
ndk_home="${ANDROID_NDK_HOME:-}"
if [[ -z "$ndk_home" ]]; then
  ndk_home="${ANDROID_NDK:-}"
fi
if [[ -z "$ndk_home" ]]; then
  log WARN ndk "NDK 未配置，host 可先编"
elif [[ ! -d "$ndk_home" ]]; then
  log WARN ndk "path not a directory: ${ndk_home}"
else
  ndk_props="${ndk_home}/source.properties"
  if [[ -f "$ndk_props" ]]; then
    ndk_rev=""
    while IFS= read -r line || [[ -n "$line" ]]; do
      case "$line" in
        Pkg.Revision*)
          ndk_rev="${line#*=}"
          ndk_rev="${ndk_rev#"${ndk_rev%%[![:space:]]*}"}"
          ndk_rev="${ndk_rev%"${ndk_rev##*[![:space:]]}"}"
          ;;
      esac
    done < "$ndk_props"
    if [[ -n "$ndk_rev" ]]; then
      log OK ndk "Pkg.Revision=${ndk_rev} (${ndk_home})"
    else
      log WARN ndk "source.properties has no Pkg.Revision (${ndk_home})"
    fi
  else
    log WARN ndk "no source.properties (${ndk_home})"
  fi
fi

# --- Vulkan（可选；Null 后端不需要，L4 才硬依赖）---
vulkan_found=0
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists vulkan 2>/dev/null; then
  vk_ver="$(pkg-config --modversion vulkan 2>/dev/null || true)"
  vk_cflags="$(pkg-config --cflags vulkan 2>/dev/null || true)"
  detail="pkg-config vulkan"
  if [[ -n "$vk_ver" ]]; then
    detail="${detail} ${vk_ver}"
  fi
  if [[ -n "$vk_cflags" ]]; then
    detail="${detail} (${vk_cflags})"
  fi
  log OK vulkan "$detail"
  vulkan_found=1
fi

if (( ! vulkan_found )); then
  vulkan_sdk="${VULKAN_SDK:-}"
  vk_candidates=()
  if [[ -n "$vulkan_sdk" ]]; then
    vk_candidates+=(
      "${vulkan_sdk}/Include/vulkan/vulkan.h"
      "${vulkan_sdk}/include/vulkan/vulkan.h"
      "${vulkan_sdk}/Lib/vulkan-1.lib"
      "${vulkan_sdk}/lib/libvulkan.so"
      "${vulkan_sdk}/lib/libvulkan.dylib"
    )
  fi
  vk_candidates+=(
    /usr/include/vulkan/vulkan.h
    /usr/local/include/vulkan/vulkan.h
    /usr/lib/libvulkan.so
    /usr/lib/x86_64-linux-gnu/libvulkan.so
    /usr/lib/aarch64-linux-gnu/libvulkan.so
    /usr/local/lib/libvulkan.so
  )
  for vk_path in "${vk_candidates[@]}"; do
    if [[ -f "$vk_path" || -L "$vk_path" ]]; then
      log OK vulkan "$vk_path"
      vulkan_found=1
      break
    fi
  done
fi

if (( ! vulkan_found )); then
  log WARN vulkan "not found (Null backend does not need it; L4 does)"
fi

# --- 消费者侧：不作为失败项 ---
log INFO glfw/jdk "消费者侧，见 docs/git/（paint-android / paint-pc）；不探测为失败项"

if (( REQUIRED_FAIL != 0 )); then
  echo "check-env: required host tools missing or too old; see docs/env/toolchain.md"
  exit 1
fi
exit 0
