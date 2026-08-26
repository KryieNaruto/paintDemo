#!/usr/bin/env bash
# 回归：SDK setup-env.ps1 / setup-env-win.sh 的 vswhere 查询必须带 -prerelease，
# 否则 Insiders/预览版（VS 2026）被默认过滤，误选 release 旧版。
set -euo pipefail
cd "$(dirname "$0")/.."

# mock vswhere：缺 -prerelease → 返回 2022（release）；加 → 返回 2026（prerelease）。
mock_dir="$(mktemp -d)"
cat > "$mock_dir/vswhere.exe" <<'MOCK'
#!/usr/bin/env bash
RELEASE="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
PRERELEASE="/c/Program Files/Microsoft Visual Studio/18/Insiders"
HAS_PRERELEASE=""
for a in "$@"; do [ "$a" = "-prerelease" ] && HAS_PRERELEASE=1; done
if [ -n "$HAS_PRERELEASE" ]; then printf '%s\n%s\n' "$PRERELEASE" "$RELEASE"; else printf '%s\n' "$RELEASE"; fi
MOCK
chmod +x "$mock_dir/vswhere.exe"

# 对照组（断言）：缺 -prerelease 返回 2022 → 即故障形态。
rel="$(bash "$mock_dir/vswhere.exe" -latest -products '*' -property installationPath | head -n1)"
case "$rel" in
  *2022/BuildTools) echo "对照确认：缺 -prerelease 返回 2022 BuildTools（故障形态）" ;;
  *) echo "FAIL: 对照组失真（应返回 2022）"; rm -rf "$mock_dir"; exit 1 ;;
esac

# 断言 1：setup-env.ps1 的 vswhere 查询行含 -prerelease。
if grep -- '-prerelease' scripts/setup-env.ps1 | grep -q -- 'vswhere'; then
  echo "断言1 OK：setup-env.ps1 查询行含 -prerelease"
else
  echo "FAIL: setup-env.ps1 查询行缺 -prerelease"; rm -rf "$mock_dir"; exit 1
fi
# 断言 2：setup-env-win.sh 的 vswhere 查询行含 -prerelease。
if grep -- '-prerelease' scripts/setup-env-win.sh | grep -q -- 'vswhere'; then
  echo "断言2 OK：setup-env-win.sh 查询行含 -prerelease"
else
  echo "FAIL: setup-env-win.sh 查询行缺 -prerelease"; rm -rf "$mock_dir"; exit 1
fi
# 断言 3：带 -prerelease 选中 2026 Insiders。
pre="$(bash "$mock_dir/vswhere.exe" -prerelease -latest -products '*' -property installationPath | head -n1)"
case "$pre" in
  *18/Insiders) echo "断言3 OK：加 -prerelease 选中 2026 Insiders" ;;
  *) echo "FAIL: 加 -prerelease 未选中 2026"; rm -rf "$mock_dir"; exit 1 ;;
esac

rm -rf "$mock_dir"
echo "PASS: SDK setup-env vswhere 带 -prerelease"
