#!/usr/bin/env bash
#
# bootstrap-consumer.sh — 在「已存在的空克隆」里把 paintDemo SDK 加为 git submodule（路径固定 sdk/）。
#
# 只操作当前消费者目录；不读写本 SDK 仓库（paintDemo）内的任何文件。
set -euo pipefail

# 默认 submodule URL 以任务书为准（HTTPS）。本仓库 origin 可能是 SSH，见 docs/git/README.md 说明。
SDK_URL="${DGCPAIN_SDK_URL:-https://github.com/KryieNaruto/paintDemo.git}"
SUBMODULE_PATH="sdk"
TAG=""
COMMIT=""

usage() {
  cat <<'EOF'
bootstrap-consumer.sh — 在「已存在的空克隆」里把 paintDemo SDK 加为 submodule（路径 sdk/）

用法:
  bootstrap-consumer.sh [选项]

前置条件（重要）:
  1. 先自己在 GitHub 建一个【空】消费者仓库。本脚本不代建仓库：
       gh repo create KryieNaruto/paint-android --private   # 或 --public
     或用网页建空库：https://github.com/new（不要初始化 README/.gitignore，保持空库）。
  2. 把该空库 clone 到本地并进入该目录：
       git clone git@github.com:KryieNaruto/paint-android.git
       cd paint-android
  3. 在该目录里运行本脚本（用绝对/相对路径指向 paintDemo 仓库里的脚本）：
       /path/to/paintDemo/scripts/bootstrap-consumer.sh --tag v0.1.0
  4. 提交 .gitmodules 与 submodule 指针：
       git add .gitmodules sdk && git commit -m "chore: submodule paintDemo SDK 到 sdk/"

选项:
  --url <URL>      SDK 仓库 URL（默认 https://github.com/KryieNaruto/paintDemo.git）
  --tag <TAG>      把 SDK 钉在指定 tag（推荐；不跟踪 main）
  --commit <SHA>   把 SDK 钉在指定 commit（与 --tag 二选一）
  -h, --help       显示本帮助

约定（详见 docs/git/README.md）:
  - submodule 路径固定 sdk/，指向 https://github.com/KryieNaruto/paintDemo.git
  - 钉 commit 或 tag，禁止长期漂浮跟踪 main
  - 消费者只 #include "dgc_paint_c_api.h"，禁止 include core/
  - clone 消费者仓库时用 git clone --recurse-submodules
EOF
}

# --- 参数解析 -------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --url)
      [[ $# -ge 2 ]] || { echo "错误: --url 需要参数" >&2; exit 1; }
      SDK_URL="$2"; shift 2 ;;
    --tag)
      [[ $# -ge 2 ]] || { echo "错误: --tag 需要参数" >&2; exit 1; }
      TAG="$2"; shift 2 ;;
    --commit)
      [[ $# -ge 2 ]] || { echo "错误: --commit 需要参数" >&2; exit 1; }
      COMMIT="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "未知参数: $1" >&2; echo >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -n "$TAG" && -n "$COMMIT" ]]; then
  echo "错误: --tag 与 --commit 只能二选一" >&2
  exit 1
fi

# --- 前置检查：必须在消费者仓库目录里运行 ---------------------------------
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "错误: 当前目录不是 git 仓库。" >&2
  echo "请先在【已存在的空消费者仓库】目录里运行本脚本（见 --help）。" >&2
  exit 1
fi

if [[ -e "$SUBMODULE_PATH" ]]; then
  echo "错误: 路径 '$SUBMODULE_PATH' 已存在，拒绝覆盖。" >&2
  exit 1
fi

# 防止重复添加同路径 submodule
if git config --file .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | grep -qx "submodule\..*\.path $SUBMODULE_PATH"; then
  echo "错误: .gitmodules 里已存在路径为 '$SUBMODULE_PATH' 的 submodule。" >&2
  exit 1
fi

# --- 核心动作 -------------------------------------------------------------
echo ">> git submodule add $SDK_URL $SUBMODULE_PATH"
git submodule add "$SDK_URL" "$SUBMODULE_PATH"

if [[ -n "$TAG" ]]; then
  echo ">> 钉 tag: git -C $SUBMODULE_PATH checkout $TAG"
  git -C "$SUBMODULE_PATH" checkout "$TAG"
elif [[ -n "$COMMIT" ]]; then
  echo ">> 钉 commit: git -C $SUBMODULE_PATH checkout $COMMIT"
  git -C "$SUBMODULE_PATH" checkout "$COMMIT"
else
  cat <<'EOF'

未指定 --tag / --commit。已按 submodule 默认记录当前 SDK commit。
建议立即钉 tag（不要长期漂浮跟踪 main）：
  git -C sdk checkout <tag>
EOF
fi

cat <<'EOF'

下一步（在消费者仓库内）:
  git add .gitmodules sdk
  git commit -m "chore: submodule paintDemo SDK 到 sdk/"

后续 clone 请用:
  git clone --recurse-submodules <消费者仓库 URL>
EOF
