<#
.SYNOPSIS
    把 ohscrcpy PC 客户端打包成单文件 exe（PyInstaller --onefile，默认）。

.DESCRIPTION
    默认产物是一个自包含的单文件 exe：目标机器无需安装 Python / numpy /
    pygame，也无需自己装 ffmpeg —— 运行库、ffmpeg.exe、设备端 ohscrcpy
    二进制全部内嵌，启动时解压、退出后自动清理。

    要运行时零解压、零临时文件时用 -OneDir：产物是目录版（exe + _internal\），
    直接从本目录运行，但分发时要带整个目录。

# 内嵌的两个"外部程序"：
      · ffmpeg.exe      客户端用它解码 H.264（无 ffmpeg 时退化为 PATH 查找）
      · ohscrcpy        设备端可执行文件，客户端会自动 hdc file send 到设备

    解压位置说明（onefile）：配置了 --runtime-tmpdir .，_MEI<pid>xxxx 解压目录
    建在【启动时的工作目录】——双击运行、或在 exe 所在目录运行时即 exe 当前
    目录。正常退出自动删除；异常退出的残留留在原地，随手可删。代价：不再
    依赖 %TEMP%，但要求启动目录可写（Program Files 等只读位置会起不来）。

.PARAMETER FfmpegPath
    指定要内嵌的 ffmpeg.exe。缺省时从 PATH 里找。

.PARAMETER NoFfmpeg
    不内嵌 ffmpeg（目标机器需自己把 ffmpeg 放进 PATH）。

.PARAMETER OneDir
    打成目录版（--onedir）：运行时零临时文件，但分发要带整个目录。

.PARAMETER KeepBuild
    保留 PyInstaller 的 build 中间目录（排查打包问题时用）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File client\build_exe.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File client\build_exe.ps1 -FfmpegPath C:\tools\ffmpeg.exe
#>
[CmdletBinding()]
param(
    [string]$FfmpegPath,
    [switch]$NoFfmpeg,
    [switch]$OneDir,
    [switch]$KeepBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ── 路径约定 ─────────────────────────────────────────────────────────────────
$ClientDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ClientDir
$Entry     = Join-Path $ClientDir 'ohscrcpy_client.py'
$DevBin    = Join-Path $ClientDir 'ohscrcpy'
$DistDir   = Join-Path $RepoRoot  'dist'
$BuildDir  = Join-Path $RepoRoot  'build\pyinstaller'
$ExeName   = 'ohscrcpy_client'

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Warn2($msg){ Write-Host "    $msg" -ForegroundColor Yellow }

# ── 1. 前置检查 ──────────────────────────────────────────────────────────────
Write-Step '检查构建环境'

$Python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $Python) { throw '找不到 python，请先安装 Python 3.10+ 并加入 PATH' }
$PyVer = (& $Python -c "import sys;print('%d.%d.%d'%sys.version_info[:3])").Trim()
if ([version]$PyVer -lt [version]'3.10') {
    throw "Python 版本过低（$PyVer），PyInstaller 6.x 需要 3.10+"
}
Write-Ok "python  $PyVer  ($Python)"

if (-not (Test-Path -LiteralPath $Entry)) { throw "找不到客户端脚本: $Entry" }

# 依赖包
$missing = @()
foreach ($mod in 'PyInstaller', 'numpy', 'pygame') {
    & $Python -c "import $mod" 2>$null
    if ($LASTEXITCODE -ne 0) { $missing += $mod }
}
if ($missing.Count -gt 0) {
    throw ("缺少依赖: {0}`n  请先执行:  pip install {1}" -f ($missing -join ', '), ($missing -join ' '))
}
Write-Ok '依赖齐全  pyinstaller / numpy / pygame'

# 设备端二进制（必须内嵌，否则 exe 运行时推不了服务端）
if (-not (Test-Path -LiteralPath $DevBin)) {
    throw "找不到设备端二进制: $DevBin`n  请先把编译产物 ohscrcpy 放到 client/ 目录下"
}
$DevBinSize = [math]::Round((Get-Item -LiteralPath $DevBin).Length / 1MB, 1)
Write-Ok "设备端二进制  $DevBin  ($DevBinSize MB)"

# ── 2. 定位 ffmpeg ───────────────────────────────────────────────────────────
Write-Step '定位 ffmpeg'
$Ffmpeg = $null
if ($NoFfmpeg) {
    Write-Warn2 '按参数要求跳过内嵌，目标机器需自行在 PATH 中提供 ffmpeg'
}
elseif ($FfmpegPath) {
    if (-not (Test-Path -LiteralPath $FfmpegPath)) {
        throw "指定的 ffmpeg 不存在: $FfmpegPath"
    }
    $Ffmpeg = (Resolve-Path -LiteralPath $FfmpegPath).Path
}
else {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { $Ffmpeg = $cmd.Source }
}

if ($Ffmpeg) {
    $FfmpegSize = [math]::Round((Get-Item -LiteralPath $Ffmpeg).Length / 1MB, 1)
    Write-Ok "内嵌 ffmpeg  $Ffmpeg  ($FfmpegSize MB)"
}

# ── 3. 组装 PyInstaller 参数 ─────────────────────────────────────────────────
# 默认单文件：一个 exe 拷走即用，启动时解压到 %TEMP%（退出自动清理）；
# -OneDir 目录版：运行时零临时文件，但要把整个目录一起分发。
$BundleMode = if ($OneDir) { '--onedir' } else { '--onefile' }
Write-Step "打包（PyInstaller $BundleMode，耗时数分钟）"

$pyiArgs = @(
    '--noconfirm'
    '--clean'
    $BundleMode
    '--console'                       # 保留控制台：需要看 [hdc]/[video]/[stat] 日志
    '--name', $ExeName
    '--distpath', $DistDir
    '--workpath', $BuildDir
    '--specpath', $BuildDir
    '--paths', $ClientDir
    # 死代码/体积：脚本只用 numpy + pygame，cv2 等重包不要误带进来
    '--exclude-module', 'cv2'
    '--exclude-module', 'torch'
    '--exclude-module', 'tkinter'
    '--exclude-module', 'matplotlib'
    '--exclude-module', 'PIL'
    '--exclude-module', 'scipy'
    '--exclude-module', 'pandas'
)

    # 解压目录改为启动时的工作目录（--runtime-tmpdir .，双击运行或在 exe
    # 所在目录运行时即 exe 当前目录）：_MEI<pid>xxxx 建在身边，正常退出自动
    # 清理，异常残留一眼可见、随手可删，也不再依赖 %TEMP% 可写。
    # 代价：启动目录必须可写（Program Files 等只读位置会起不来）。
    if (-not $OneDir) { $pyiArgs += @('--runtime-tmpdir', '.') }
    # 内嵌资源：都放到包根目录，客户端用 find_resource() 按名字找
    $pyiArgs += '--add-binary', "$DevBin;."
if ($Ffmpeg) { $pyiArgs += '--add-binary', "$Ffmpeg;." }

$pyiArgs += $Entry

Write-Host ("    python -m PyInstaller " + ($pyiArgs -join ' ')) -ForegroundColor DarkGray
& $Python -m PyInstaller @pyiArgs
if ($LASTEXITCODE -ne 0) { throw "PyInstaller 打包失败（exit=$LASTEXITCODE）" }

# ── 4. 收尾 ─────────────────────────────────────────────────────────────────
if ($OneDir) {
    $OutDir  = Join-Path $DistDir $ExeName
    $OutExe  = Join-Path $OutDir "$ExeName.exe"
    $RunPath = "dist\$ExeName\$ExeName.exe"
    if (-not (Test-Path -LiteralPath $OutExe)) { throw "打包命令成功但没找到产物: $OutExe" }
    # 内嵌资源必须真的进包了（onedir 下 --add-binary 落在 _internal\）
    $BundledDev = Join-Path $OutDir (Join-Path '_internal' (Split-Path -Leaf $DevBin))
    if (-not (Test-Path -LiteralPath $BundledDev)) { throw "内嵌资源没进包: $BundledDev" }
    if ($Ffmpeg) {
        $BundledFfmpeg = Join-Path $OutDir (Join-Path '_internal' 'ffmpeg.exe')
        if (-not (Test-Path -LiteralPath $BundledFfmpeg)) { throw "内嵌资源没进包: $BundledFfmpeg" }
    }
    $OutSize = [math]::Round((Get-ChildItem -LiteralPath $OutDir -Recurse -File |
        Measure-Object -Property Length -Sum).Sum / 1MB, 1)
}
else {
    $OutExe  = Join-Path $DistDir "$ExeName.exe"
    $RunPath = "dist\$ExeName.exe"
    if (-not (Test-Path -LiteralPath $OutExe)) { throw "打包命令成功但没找到产物: $OutExe" }
    $OutSize = [math]::Round((Get-Item -LiteralPath $OutExe).Length / 1MB, 1)
    # exe 若继承了 Low 完整性标签（目录带标签导致），低完整性进程在普通目录
    # 下创建 _MEI 解压目录会被拒，启动直接失败 —— 构建期就查出来并给修复命令
    if ((icacls $OutExe) -match 'Low Mandatory') {
        Write-Warn2 "警告: 产物带 Low 完整性标签，运行时无法创建解压目录，启动会失败！"
        Write-Warn2 "修复: icacls '$DistDir' /setintegritylevel M  （父目录带标签时也要一并改）"
    }
}
if (-not $KeepBuild) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
    # 顺手删掉空的 build/ 父目录（里面还有别的东西时保留）
    $BuildRoot = Split-Path -Parent $BuildDir
    if ((Test-Path -LiteralPath $BuildRoot) -and
        -not (Get-ChildItem -LiteralPath $BuildRoot -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $BuildRoot -Force -ErrorAction SilentlyContinue
    }
}

Write-Step '完成'
Write-Ok "产物: $OutExe  ($OutSize MB)"
Write-Host @"

用法（目标机器只需 hdc 在 PATH 中）：
    $RunPath                # 单设备
    $RunPath -t <deviceId>  # 多设备指定
    $RunPath --help

提示：
  · 解压目录 _MEI<pid>xxxx 建在【启动时的工作目录】（通常即 exe 所在目录），
    正常退出自动清理；要求该目录可写。
  · 要运行时零解压、零临时文件，用 -OneDir 打目录版。
  · 若 exe 同目录存在同名 ohscrcpy / ffmpeg.exe，会优先使用（便于替换调试）。
  · 设置环境变量 OHS_LOGFILE=C:\temp\ohs.log 可把日志同时写文件。
"@ -ForegroundColor Gray
