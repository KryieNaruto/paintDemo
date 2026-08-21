#!/usr/bin/env bash
# 在「已经存在的消费者仓库」根目录，把 paintDemo SDK 加为 sdk/ 子模块。
# 不创建 GitHub 仓库（本环境 gh 只读；请先自行 gh repo create / 网页建空库）。
set -euo pipefail

SDK_URL="${DGCPAIN_SDK_URL:-https://github.com/KryieNaruto/paintDemo.git}"
SDK_PATH="sdk"

usage() {
  cat <<'EOF'
Usage: bootstrap-consumer.sh [-h]
  Run from the root of an existing consumer git repo (paint-android / paint-pc).

  Adds submodule:
    sdk/  ->  https://github.com/KryieNaruto/paintDemo.git

  Environment:
    DGCPAIN_SDK_URL   Override SDK remote (default: GitHub paintDemo HTTPS)

  You must create the consumer GitHub repo yourself first, for example:
    gh repo create KryieNaruto/paint-android --public
    gh repo create KryieNaruto/paint-pc --public
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ ! -d .git ]]; then
  echo "bootstrap-consumer.sh: run from a consumer repository root (.git missing)" >&2
  exit 1
fi

if [[ -e "$SDK_PATH" ]]; then
  echo "bootstrap-consumer.sh: $SDK_PATH already exists" >&2
  exit 1
fi

git submodule add "$SDK_URL" "$SDK_PATH"
echo "Added $SDK_URL at $SDK_PATH"
echo "Pin a commit or tag before pushing, e.g.: git -C $SDK_PATH checkout <tag>"
echo "Then: git add .gitmodules $SDK_PATH && git commit -m 'Add paintDemo SDK as submodule at sdk/'"
