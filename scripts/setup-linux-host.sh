#!/usr/bin/env bash
# 在 Linux / WSL Ubuntu 上安装编译 libdgc_paint 所需的 host 四包。
# 需要 sudo（root 下可省略）。不要在 Windows / Git Bash 里执行。
# 不安装 libglfw3-dev（GLFW 属于 paint-pc 消费者）。

set -euo pipefail

usage() {
  cat <<EOF
用法: setup-linux-host.sh [-h|--help]
  在 Linux 或 WSL Ubuntu 上安装 SDK host 工具链（任务书四包）：
    build-essential cmake ninja-build libvulkan-dev

  需要 sudo（当前已是 root 时直接 apt）。非 Linux 会拒绝执行。

  不安装 libglfw3-dev / GLFW / JDK / Android Studio。
  无 GPU 运行时不视为失败（只装头文件与开发包）。

  装完调用同仓库 scripts/check-env.sh，把探测结果打到 stdout。
  版本下限见 docs/env/toolchain.md；安装步骤见 docs/env/linux-host.md。
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "setup-linux-host.sh: 请到 Linux 或 WSL 执行（当前不是 Linux，拒绝调用 apt）" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ "$(id -u)" -eq 0 ]]; then
  APT=(apt)
else
  if ! command -v sudo >/dev/null 2>&1; then
    echo "setup-linux-host.sh: 需要 sudo（或用 root 执行）" >&2
    exit 1
  fi
  APT=(sudo apt)
fi

echo "SDK root: ${SDK_ROOT}"
echo "安装: build-essential cmake ninja-build libvulkan-dev"
"${APT[@]}" update
"${APT[@]}" install --no-install-recommends -y \
  build-essential \
  cmake \
  ninja-build \
  libvulkan-dev

echo "--- check-env.sh ---"
bash "${SDK_ROOT}/scripts/check-env.sh"
