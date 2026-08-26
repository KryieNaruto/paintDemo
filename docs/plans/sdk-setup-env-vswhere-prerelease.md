# 修复计划：SDK setup-env.ps1 / setup-env-win.sh vswhere 缺 -prerelease

## 0. Bug 报告

- **现象**：与 paint-pc 同根因 —— `scripts/setup-env.ps1:81` 与 `scripts/setup-env-win.sh:115` 的 vswhere 查询缺 `-prerelease`，Windows 下会过滤掉 VS 2026 Insiders（prerelease），误选 release 旧版（如 VS 2022 BuildTools）。
- **来源**：paint-pc 修复 vswhere-prerelease-fix 时审阅确认 SDK submodule 存在同类缺口，记录「另立任务」，现执行。
- **报告者**：主会话延续（paint-pc 修复的审阅发现）。

## 1. 问题查找（①，已完成）

### 复现（无头 mock vswhere）
与 paint-pc 相同机制，已在 paint-pc 修复中实证：mock vswhere 缺 `-prerelease` 返回 release 2022，加 `-prerelease` 返回 2026 Insiders。SDK 两处是同一查询形态。

### 根因（实证）
vswhere **默认排除 prerelease**（Insiders/预览版），必须显式加 `-prerelease`。
- `scripts/setup-env.ps1:81`：`& $vswhere -latest -products * -requires ... -property installationPath` — 缺 `-prerelease`。
- `scripts/setup-env-win.sh:115`：`"$vswhere" -latest -products '*' ...` — 缺 `-prerelease`。

### 影响面
- `scripts/setup-env.ps1:81`（Windows PowerShell 探测）
- `scripts/setup-env-win.sh:115`（Windows bash 探测 + MSVC 环境注入）
- 两处均为独立函数调用（`Find-VsInstallation` / MSVC 注入），无共享复用链。
- `setup-env.sh`（Linux）不查 vswhere，不受影响。

## 2. 修复方案（②）

给两处 vswhere 查询加 `-prerelease`：

### `scripts/setup-env.ps1:81`
```powershell
$vs = & $vswhere -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
```
同步在 `setup-env.ps1:76` 注释上方加一行：`# 必须带 -prerelease：vswhere 默认排除 Insiders/预览版（VS 2026 Insiders 属 prerelease），缺了会误选 release 旧版。`

### `scripts/setup-env-win.sh:115-117`
```bash
vsdir="$("$vswhere" -prerelease -latest -products '*' \
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath 2>/dev/null | tail -1)"
```
同步在 `setup-env-win.sh:97` 注释区补一句：`# vswhere 必须带 -prerelease，否则 2026 Insiders 被默认过滤。`

### 设计要点
- `-prerelease -latest`：`-prerelease` 纳入 Insiders 通道，`-latest` 按版本号最高选（2026=18.x > 2022=17.x）。
- 对 release-only 用户无副作用。
- 不回退原则：不做降级/掩盖。
- 注释固化根因：防未来回归（审阅 95 分建议补）。

## 3. 回归用例设计（先红后绿）

**新增** `tests/test_setup_env_vswhere_prerelease.sh`（无头，Linux 可跑，mock vswhere + grep 查询行断言）：

```bash
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
```

**先红后绿**：未修复源码 grep 无 `-prerelease` → 红；修复后 → 绿。

## 4. 影响面核对
- 仅改 `scripts/setup-env.ps1:81`、`scripts/setup-env-win.sh:115-117`（各一行）+ 新增回归测试。
- Linux 的 setup-env.sh 不动；fetch-deps.sh 不动；bootstrap-consumer.sh 不动。
- paint-pc 的 setup.sh/setup.ps1 已在上一个修复处理，本次不重复。

## 5. 验证方式
- **无头回归**：`bash tests/test_setup_env_vswhere_prerelease.sh`。
- **语法**：`bash -n scripts/setup-env-win.sh`（ps1 无语法检查工具，人工核对一行）。
- **冒烟**：`bash scripts/setup-env.sh --check`（Linux 探测不回归）。
- **诚实标注**：真机终验需 Windows 跑 setup-env.ps1 / setup-env-win.sh 确认定位 2026。不涉渲染，离屏验证不适用。

## 6. 风险与健壮性
- `-prerelease` 对 release-only 无副作用。
- 单行改动，无新依赖，无回退路径。
