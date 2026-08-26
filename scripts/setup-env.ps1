<#
.SYNOPSIS
    libdgc_paint SDK + paint-pc 消费者 Windows 一键环境搭建脚本（W1）。

.DESCRIPTION
    在 Windows 真机上一条命令把 paint-pc 真实绘制验证环境搭起来：
      1) 探测  VS2026 (MSVC cl.exe) / CMake / Ninja / Vulkan SDK / glslc / git；
      2) 补缺  硬依赖缺失给精确安装指引（VS installer 命令行 / Vulkan SDK 下载器），
              软依赖缺失仅警告；
      3) 拉取  clone paint-pc 消费者仓库 + SDK submodule（钉 9e6eefb）；
      4) 构建  vcvars64 环境内 cmake -B build -G Ninja + cmake --build；
      5) 验证  build\paint_pc.exe --headless out.png + 离屏 PNG 真实笔迹像素断言
              （CLI + 离屏渲染输出图像硬约束）。

    用法（PowerShell 5.1+ / PowerShell Core 7+）:
      .\scripts\setup-env.ps1              默认：探测+补缺指引+拉取+构建+验证
      .\scripts\setup-env.ps1 --check      只探测不安装，输出缺项清单
      .\scripts\setup-env.ps1 -SkipBuild   跳过构建/验证（仅探测+拉取）
      .\scripts\setup-env.ps1 -Repo <dir>  使用/创建指定 paint-pc 目录（默认 ./paint-pc）
      .\scripts\setup-env.ps1 -Help        打印本帮助

    口径来源: docs/env/env-setup.md §3（Windows VS2026）。
    依赖分级: 硬依赖缺失 → 非零退出并给安装指引；软依赖缺失 → 仅警告。
.PARAMETER Check
    只探测不安装，输出缺项清单。硬依赖缺失 → exit 1，仅软依赖缺失 → exit 0。
.PARAMETER SkipBuild
    跳过拉取/构建/验证，仅探测 + 补缺指引。
.PARAMETER Repo
    paint-pc 消费者仓库目录（默认 ./paint-pc，相对脚本所在仓库根）。
.PARAMETER Help
    打印帮助后退出。
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$SkipBuild,
    [string]$Repo = "",
    [switch]$Help
)

# 仅 Windows 平台有意义。
if ($PSVersionTable.PSEdition -eq "Desktop" -and $PSVersionTable.PSVersion.Major -lt 5) {
    Write-Host "ERROR: 需要 PowerShell 5.1+。" -ForegroundColor Red
    exit 2
}
if ($Help) {
    Get-Help $PSCommandPath -Detailed
    exit 0
}

$ErrorActionPreference = "Stop"

# ---------- 输出辅助（对齐 setup-env.sh 的 info/warn/err） ----------
function Info($m)  { Write-Host $m }
function Warn($m)  { Write-Host ("WARN: " + $m) -ForegroundColor Yellow }
function Err($m)   { Write-Host ("ERROR: " + $m) -ForegroundColor Red }
function Ok($m)    { Write-Host ("[OK]   " + $m) -ForegroundColor Green }

# ---------- 探测结果存储 ----------
$script:ChkNames  = [System.Collections.Generic.List[string]]::new()
$script:ChkLevels = [System.Collections.Generic.List[string]]::new()
$script:ChkStatus = [System.Collections.Generic.List[string]]::new()
$script:ChkDetail = [System.Collections.Generic.List[string]]::new()
$script:HardMiss  = 0
$script:SoftMiss  = 0

function Record($name, $level, $status, $detail) {
    $script:ChkNames.Add($name)
    $script:ChkLevels.Add($level)
    $script:ChkStatus.Add($status)
    $script:ChkDetail.Add($detail)
}

# ---------- 探测实现 ----------

# 必须带 -prerelease：vswhere 默认排除 Insiders/预览版（VS 2026 Insiders 属 prerelease），缺了会误选 release 旧版。
# 找 Visual Studio 安装目录（vswhere 是 VS 自带官方定位器）。
function Find-VsInstallation {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { $vswhere = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe" }
    if (-not (Test-Path $vswhere)) { return "" }
    $vs = & $vswhere -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return ($vs | Select-Object -First 1).Trim()
}

function Probe-VisualStudio {
    $vs = Find-VsInstallation
    if (-not $vs) {
        Record "Visual Studio (MSVC)" "硬" "MISS(硬)" "未找到含 VC 工具集的 VS（cl.exe 编译必败）"
        $script:HardMiss++
        return
    }
    # 在开发者环境外定位 cl.exe（VC\Tools\MSVC\<ver>\bin\Hostx64\x64\cl.exe）
    $cl = Get-ChildItem -Path (Join-Path $vs "VC\Tools\MSVC") -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "Hostx64\\x64\\cl\.exe$" } | Select-Object -First 1
    if (-not $cl) {
        Record "Visual Studio (MSVC)" "硬" "MISS(硬)" "找到 VS 但缺 cl.exe（VC 工具集未装）"
        $script:HardMiss++
        return
    }
    $ver = & $cl.FullName 2>&1 | Select-Object -First 1
    Record "Visual Studio (MSVC)" "硬" "OK" "VS @ $vs ($(Split-Path (Split-Path (Split-Path $cl.DirectoryName))))"
}

function Probe-CMake {
    $cm = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cm) {
        Record "CMake" "硬" "MISS(硬)" "未安装（configure 必败）"
        $script:HardMiss++
        return
    }
    $v = (& cmake --version 2>$null | Select-Object -First 1)
    # 版本下限 3.22（host 建议 3.31+，只校验 3.22）
    if ($v -match "cmake version ([0-9]+\.)+[0-9]+") {
        $ver = $Matches[0] -replace "cmake version ", ""
        $major = [int]($ver.Split('.')[0]); $minor = [int]($ver.Split('.')[1])
        if ($major -gt 3 -or ($major -eq 3 -and $minor -ge 22)) {
            Record "CMake" "硬" "OK" $v
        } else {
            Record "CMake" "硬" "MISS(硬)" "$v 过旧（需 ≥ 3.22，建议 3.31+）"
            $script:HardMiss++
        }
    } else {
        Record "CMake" "硬" "OK" "已安装但版本无法确认（请手动确认 ≥ 3.22）"
    }
}

function Probe-Ninja {
    $nj = Get-Command ninja -ErrorAction SilentlyContinue
    if (-not $nj) {
        # VS 自带 ninja（随 "C++ CMake tools for Windows" 工作负载），路径形如
        # VS\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
        $vs = Find-VsInstallation
        if ($vs) {
            $ninjaInVs = Get-ChildItem -Path (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake") -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($ninjaInVs) {
                Record "Ninja" "硬" "OK" "VS 自带 $($ninjaInVs.FullName)"
                return
            }
        }
        Record "Ninja" "硬" "MISS(硬)" "未安装（可用 VS 'C++ CMake tools' 工作负载提供）"
        $script:HardMiss++
        return
    }
    $v = (& ninja --version 2>$null | Select-Object -First 1)
    Record "Ninja" "硬" "OK" "ninja $v"
}

function Probe-VulkanSdk {
    $vsdk = $env:VULKAN_SDK
    if (-not $vsdk -or -not (Test-Path $vsdk)) {
        Record "Vulkan SDK" "硬" "MISS(硬)" "\$env:VULKAN_SDK 未设置或目录不存在（paint-pc 离屏渲染走真实 VkBackend，链接 vulkan-1.lib 必败）"
        $script:HardMiss++
        return
    }
    $lib = Test-Path (Join-Path $vsdk "Lib\vulkan-1.lib")
    if (-not $lib) {
        Record "Vulkan SDK" "硬" "MISS(硬)" "VULKAN_SDK 存在但缺 Lib\vulkan-1.lib（SDK 安装不完整）"
        $script:HardMiss++
        return
    }
    Record "Vulkan SDK" "硬" "OK" "$vsdk"
}

function Probe-Glslc {
    # glslc 由 LunarG Vulkan SDK 提供（Windows 版），host 编译走 shaderc 运行时
    # 编译，不直接调 glslc；这里仅作软依赖提示。
    $g = Get-Command glslc -ErrorAction SilentlyContinue
    if (-not $g) {
        Record "glslc" "软" "WARN(软)" "未在 PATH（LunarG Vulkan SDK 提供；shaderc 走库调用，此仅为提示）"
        $script:SoftMiss++
        return
    }
    $v = (& glslc --version 2>$null | Select-Object -First 1)
    Record "glslc" "软" "OK" $v
}

function Probe-Git {
    $g = Get-Command git -ErrorAction SilentlyContinue
    if (-not $g) {
        Record "git" "硬" "MISS(硬)" "未安装（clone paint-pc + submodule 必败）"
        $script:HardMiss++
        return
    }
    $v = (& git --version 2>$null | Select-Object -First 1)
    Record "git" "硬" "OK" $v
}

function Probe-All {
    $script:ChkNames.Clear(); $script:ChkLevels.Clear(); $script:ChkStatus.Clear(); $script:ChkDetail.Clear()
    $script:HardMiss = 0; $script:SoftMiss = 0
    Probe-VisualStudio
    Probe-CMake
    Probe-Ninja
    Probe-Git
    Probe-VulkanSdk
    Probe-Glslc
}

# ---------- 输出 ----------
function Print-Check {
    Info "=== 环境探测结果（口径见 docs/env/env-setup.md §3）==="
    for ($i = 0; $i -lt $script:ChkNames.Count; $i++) {
        Write-Host ("[{0}] {1}: {2}" -f $script:ChkStatus[$i], $script:ChkNames[$i], $script:ChkDetail[$i])
    }
    if ($script:HardMiss -gt 0) {
        Err "硬依赖缺失 $($script:HardMiss) 项，paint-pc 构建/验证将失败。"
    } else {
        Info "硬依赖齐全；软依赖缺失不阻断构建。"
    }
}

function Print-Guidance {
    Info "请按 docs/env/env-setup.md §3 手动补缺："
    Info "  - Visual Studio: 安装 VS2026 并勾选「使用 C++ 的桌面开发」+「C++ CMake tools for Windows」。"
    Info "    命令行自动补工作负载（管理员 PowerShell）："
    Info "      & '<VS Installer>\vs_installer.exe' modify --installPath '<VS路径>' --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.CMake.Project --quiet --norestart"
    Info "  - Vulkan SDK（paint-pc 离屏渲染硬依赖）：下载 LunarG Vulkan SDK Windows 版"
    Info "      https://vulkan.lunarg.com/sdk/home  → 安装后设 \$env:VULKAN_SDK 指向其根目录。"
    Info "  - git: 安装 https://git-scm.com/download/win"
    Info "  - glslc 为软依赖（LunarG SDK 提供），缺失仅警告。"
}

function Print-Help {
    Get-Help $PSCommandPath -Detailed
}

# ---------- 拉取 paint-pc ----------
function Get-PaintPc {
    param([string]$RepoDir, [string]$ScriptRoot)
    if (-not $RepoDir) { $RepoDir = Join-Path $ScriptRoot "paint-pc" }
    if (-not (Test-Path (Join-Path $RepoDir ".git"))) {
        Info "clone paint-pc → $RepoDir"
        & git clone --recurse-submodules https://github.com/KryieNaruto/paint-pc.git $RepoDir
        if ($LASTEXITCODE -ne 0) { Err "clone 失败"; exit 1 }
    } else {
        Info "paint-pc 已存在 @ $RepoDir"
        Push-Location $RepoDir
        & git fetch origin
        & git pull --ff-only origin main
        Pop-Location
    }
    Push-Location $RepoDir
    # 消费者 submodule 由仓库 .gitmodules 指向 paintDemo SDK，钉 9e6eefb。
    & git submodule update --init --recursive
    Pop-Location
    if ($LASTEXITCODE -ne 0) { Err "submodule 同步失败"; exit 1 }
    return $RepoDir
}

# ---------- 构建 + 验证 ----------
function Build-And-Verify {
    param([string]$RepoDir, [string]$ScriptRoot)
    Push-Location $RepoDir

    # 定位 VS 安装以进入 vcvars64 开发者环境。
    $vs = Find-VsInstallation
    if (-not $vs) { Err "未找到 VS（探测阶段应已拦截）"; Pop-Location; exit 1 }
    $vcvars = Get-ChildItem -Path (Join-Path $vs "VC\Auxiliary\Build") -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vcvars) { Err "未找到 vcvars64.bat（VC 工具集未装完整）"; Pop-Location; exit 1 }

    # 注意：Vulkan SDK 的 vulkan-1.lib 需在链接期可见。CMake 的 render/vulkan
    # find_library 用系统默认路径（Windows 下含 $env:VULKAN_SDK/Lib），SDK installer
    # 会设 VULKAN_SDK 环境变量。这里显式带上，避免依赖用户 shell 环境。
    $env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"

    Info "=== 构建 paint-pc（Ninja + vcvars64）==="
    # 在 vcvars64 环境内跑 cmake/ninja（cl.exe / nmake / ninja 由 VS 提供）。
    # 与 paint-pc 现有口径一致：-DDGCPAIN_BUILD_TESTS=OFF（消费者根下 SDK tests 因
    # CMAKE_SOURCE_DIR 错位会失效）、-DDGCPAIN_BUILD_CLI=OFF（host 不编 SDK CLI）。
    $buildDir = Join-Path $RepoDir "build"
    & cmd /c "`"$($vcvars.FullName)`" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF && cmake --build build"
    if ($LASTEXITCODE -ne 0) { Err "构建失败"; Pop-Location; exit 1 }

    $exe = Join-Path $buildDir "paint_pc.exe"
    if (-not (Test-Path $exe)) { Err "产物缺失: $exe"; Pop-Location; exit 1 }
    Ok "构建产物: $exe"

    Info "=== 离屏验证（headless PNG 真实笔迹断言）==="
    $outPng = Join-Path $buildDir "headless.png"
    & cmd /c "`"$($vcvars.FullName)`" && `"$exe`" --headless `"$outPng`""
    if ($LASTEXITCODE -ne 0) { Err "headless 运行失败"; Pop-Location; exit 1 }
    if (-not (Test-Path $outPng)) { Err "PNG 未生成"; Pop-Location; exit 1 }

    # 离屏 PNG 真实笔迹断言：B3-1 真实内核 + VkBackend 下，固定笔迹（seed=42 黑色斜线）
    # 必须产生与背景不同的暗像素。背景为纯白 (255,255,255)（SDK dgcClear 恒清白）。
    # 用 System.Drawing（Windows 原生）读像素，免 Python 依赖。
    $dark = Test-StrokePixels $outPng
    if ($dark -gt 50) {
        Ok "离屏 PNG 含真实笔迹（stroke pixels=$dark）: $outPng"
    } else {
        Err "离屏 PNG 无真实笔迹（dark=$dark）—— B3-1 真实内核未生效或渲染链路异常"
        Pop-Location
        exit 1
    }
    Pop-Location
}

# ---------- 离屏 PNG 真实笔迹断言（Windows 原生 System.Drawing） ----------
function Test-StrokePixels {
    param([string]$PngPath)
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::new($PngPath)
    try {
        # 动态导出背景：取四角 8×8 区域众数（笔迹为斜线不经过四角）。
        $bgCounts = @{}
        foreach ($corner in @(@(0,0), @($bmp.Width-8,0), @(0,$bmp.Height-8), @($bmp.Width-8,$bmp.Height-8))) {
            for ($y = $corner[1]; $y -lt $corner[1]+8; $y++) {
                for ($x = $corner[0]; $x -lt $corner[0]+8; $x++) {
                    $c = $bmp.GetPixel($x, $y)
                    $key = "{0},{1},{2}" -f $c.R, $c.G, $c.B
                    if ($bgCounts.ContainsKey($key)) { $bgCounts[$key]++ } else { $bgCounts[$key] = 1 }
                }
            }
        }
        $bgKey = ($bgCounts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
        $bgParts = $bgKey.Split(",")
        $bgSum = [int]$bgParts[0] + [int]$bgParts[1] + [int]$bgParts[2]
        Info "背景(四角众数)=($bgKey) sum=$bgSum"

        # 隔 3 抽样足够（笔迹对角线贯穿画面）；暗像素 = 明显低于背景亮度。
        $dark = 0
        for ($y = 0; $y -lt $bmp.Height; $y += 3) {
            for ($x = 0; $x -lt $bmp.Width; $x += 3) {
                $c = $bmp.GetPixel($x, $y)
                $sum = [int]$c.R + [int]$c.G + [int]$c.B
                if ($c.A -gt 0 -and $sum -lt ($bgSum - 60)) { $dark++ }
            }
        }
        return $dark
    } finally {
        $bmp.Dispose()
    }
}

# ---------- 主流程 ----------
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path   # scripts/
$ScriptRoot = Split-Path -Parent $ScriptRoot                     # 仓库根

Probe-All
Print-Check

if ($Check) {
    if ($script:HardMiss -gt 0) { Print-Guidance; exit 1 }
    exit 0
}

if ($script:HardMiss -gt 0) {
    Print-Guidance
    Err "硬依赖缺失 $($script:HardMiss) 项，未自动安装（Windows 下 VS/Vulkan SDK 需交互安装）。"
    exit 1
}

if ($SkipBuild) {
    Info "探测通过（--SkipBuild 跳过构建/验证）。"
    exit 0
}

$RepoDir = Get-PaintPc -RepoDir $Repo -ScriptRoot $ScriptRoot
Build-And-Verify -RepoDir $RepoDir -ScriptRoot $ScriptRoot

Info "Windows 环境就绪：paint-pc 已构建，headless 离屏 PNG 含真实笔迹。"
exit 0
