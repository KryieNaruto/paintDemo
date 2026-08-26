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

# 下载单个 deb：依序尝试镜像 URL 池，成功即停。返回 0/1。
fetch_deb() {
  local name="$1" url_tpl="$2" version="$3"
  local cache="$DEPS_CACHE/$name.deb"
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
  local deb="$1"
  local tmp="$DEPS_CACHE/extract_$(basename "$deb" .deb)"
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

case "${1:-}" in
  --list)  cmd_list ;;
  --check) cmd_check ;;
  --fetch) fetch_deps ;;
  --help|-h|"") usage ;;
  *) err "未知参数: $1"; usage; exit 2 ;;
esac
