[CmdletBinding()]
param(
    [ValidateSet("esp32-F4R0", "esp32-F8R4", "esp32c3-F4R0", "esp32c6-F4R0", "esp32s3-F8R0", "esp32s3-F8R4", "esp32s3-F16R8")]
    [string]$Env = "esp32-F4R0",

    [Parameter(Mandatory = $true)]
    [string]$Port,

    [string]$ImageDir = "dist\firmware\all-latest",
    [int]$Baud = 921600,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

# 平台检测：$IsWindows 在 PowerShell 7+ 内置，Windows PowerShell 5.1 中不存在（默认 true）
$IsWin = if (Test-Path Variable:IsWindows) { $IsWindows } else { $true }

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir

# 非 Windows 平台规范化路径分隔符
if (-not $IsWin) {
    $ImageDir = $ImageDir -replace '\\', '/'
}

function Get-ChipName {
    param([string]$Environment)

    if ($Environment -match '^(esp32[a-z0-9]*)-') {
        return $Matches[1]
    }
    return "esp32"
}

function Get-PythonForEsptool {
    # PlatformIO Python 路径因平台而异
    if ($IsWin) {
        $pioPython = Join-Path (Join-Path (Join-Path $env:USERPROFILE ".platformio") "penv") "Scripts\python.exe"
    } else {
        $pioPython = Join-Path (Join-Path (Join-Path $HOME ".platformio") "penv") "bin/python"
    }
    if (Test-Path -LiteralPath $pioPython -PathType Leaf) {
        return $pioPython
    }

    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) {
        $cmd = Get-Command python3 -ErrorAction SilentlyContinue
    }
    if ($cmd) {
        return $cmd.Source
    }

    throw "Python not found. Install PlatformIO first so esptool is available."
}

$resolvedImageDir = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $ImageDir))
$manifestPath = Join-Path $resolvedImageDir "manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Release manifest not found: $manifestPath. Run $(Join-Path scripts build-all-artifacts.ps1) first."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$entry = @($manifest.environments | Where-Object { $_.environment -eq $Env }) | Select-Object -First 1
if (-not $entry) {
    throw "Environment '$Env' not found in $manifestPath."
}

$releaseFile = [string]$entry.releaseFile
if ([string]::IsNullOrWhiteSpace($releaseFile)) {
    throw "Manifest entry for '$Env' does not contain releaseFile."
}

$imagePath = Join-Path $resolvedImageDir $releaseFile
if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
    throw "Release image not found: $imagePath"
}

$chip = Get-ChipName -Environment $Env
$python = Get-PythonForEsptool

Write-Host "FastBee release flash" -ForegroundColor Green
Write-Host "  Environment : $Env"
Write-Host "  Chip        : $chip"
Write-Host "  Port        : $Port"
Write-Host "  Baud        : $Baud"
Write-Host "  Image       : $imagePath"

$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

$args = @(
    "-m", "esptool",
    "--chip", $chip,
    "--port", $Port,
    "--baud", [string]$Baud,
    "--before", "default-reset",
    "--after", "hard-reset",
    "write-flash",
    "-z",
    "0x0",
    $imagePath
)

Write-Host "$python $($args -join ' ')" -ForegroundColor DarkCyan
if ($DryRun) {
    Write-Host "Dry run only; no flash was written." -ForegroundColor Yellow
    return
}

& $python @args
if ($LASTEXITCODE -ne 0) {
    throw "esptool failed with exit code $LASTEXITCODE"
}

Write-Host "Release image flashed successfully." -ForegroundColor Green
