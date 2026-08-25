#!/usr/bin/env bash
# B5-3 golden 一键再生成：换机/换驱动后按 §4.0.3「跨 GPU 允许 ±1 LSB 容差」重建基准。
# 用法（在 worktree 根执行，需先 cmake --preset host-linux && cmake --build --preset host-linux）：
#   tests/fixtures/regenerate_golden.sh [dgc_cli 路径]
#
# 默认定位 build/host-linux/cli/dgc_cli；也可显式传入 dgc_cli 二进制路径。
# lavapipe 未默认可见时，请设置 VK_ICD_FILENAMES 指向 lvp_icd.json。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
CLI="${1:-${ROOT}/build/host-linux/cli/dgc_cli}"

SCRIPT="${HERE}/determinism_script.json"
OUT="${HERE}/golden_determinism.png"

if [[ ! -x "${CLI}" ]]; then
    echo "dgc_cli 未找到: ${CLI}（先 cmake --preset host-linux && cmake --build --preset host-linux，或传入路径）" >&2
    exit 1
fi

"${CLI}" run "${SCRIPT}" --out "${OUT}"
echo "golden 已重建: ${OUT}"
