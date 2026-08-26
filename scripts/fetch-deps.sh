#!/usr/bin/env bash
# fetch-deps.sh — 三方库一键拉取（共享 CLI，paint-pc / paint-android setup.sh 调用）
#
# 用法:
#   scripts/fetch-deps.sh --list   打印三方库清单
#   scripts/fetch-deps.sh --check  探测缺哪些大库（本机）
#   scripts/fetch-deps.sh --fetch  拉取/解包缺失大库到 deps/usr
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
