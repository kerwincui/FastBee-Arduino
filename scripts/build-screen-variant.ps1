# 触控屏 RGB 固件变体（esp32s3-F16R8-screen）构建入口。
#
# 该变体不在 platformio.ini 正式矩阵内：custom_sdkconfig（XIP from PSRAM、
# LCD RGB ISR IRAM_SAFE 等）要求独立 SDK 编译，SDK 已重编一次并冻结在仓内
# 核心目录 fastbee-screen-core/（r14 基线，哈希断言见
# artifacts/rgb-xip40-v12-revalidated-r14.json；2026-10-16 起由仓外
# D:/fastbee-screen-core 与 C:/p12 迁入仓根，.gitignore 的 /fastbee-*/
# 已覆盖不提交）。每轮候选派生 artifacts/build-<Candidate>.py（全量源码镜像
# + 冻结校验）与 artifacts/platformio-<Candidate>.ini，本脚本提供唯一正确的
# 运行环境：冻结核心目录的 PLATFORMIO_* 路径、完全离线模式与 penv 守卫。
# 不要使用默认 ~/.platformio 包目录运行该构建（会误触发工具包下载并失败）。
#
# 用法：powershell -File scripts/build-screen-variant.ps1 -Candidate screen-r3
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9]+-r[0-9]+$')]
    [string]$Candidate,
    [int]$LockTimeoutSeconds = 21600
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuilder = "artifacts/build-$Candidate.py"
$taskIni = "artifacts/platformio-$Candidate.ini"
foreach ($taskPath in @($taskBuilder, $taskIni)) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskRoot $taskPath))) {
        throw "Missing derived candidate file: $taskPath"
    }
}
$taskCore = Join-Path $taskRoot 'fastbee-screen-core'
$taskP12 = Join-Path $taskRoot 'fastbee-screen-build'
foreach ($taskSub in @('packages/framework-arduinoespressif32-libs', 'platforms', 'cache')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskCore $taskSub))) {
        throw "Frozen RGB core dir incomplete: $taskCore/$taskSub (see scripts/README.md build-screen-variant.ps1)"
    }
}
if ($env:FASTBEE_FIRMWARE_LOCK_TOKEN) { throw 'Invoke as a fresh outer lock owner.' }
Set-Location $taskRoot
. "$taskRoot/scripts/firmware-build-lock.ps1"
$handle = Enter-FastBeeFirmwareBuildLock -ProjectRoot $taskRoot -Label "$Candidate-incremental" -TimeoutSeconds $LockTimeoutSeconds
try {
    # penv guard: pin PlatformIO penv first on PATH and drop external Python
    # overrides so old Git/mingw64 runtimes cannot hijack pio subprocesses.
    $penvScripts = Join-Path $env:USERPROFILE '.platformio/penv/Scripts'
    if (Test-Path -LiteralPath $penvScripts) {
        $env:PATH = $penvScripts + [System.IO.Path]::PathSeparator + $env:PATH
    }
    Remove-Item Env:PYTHONHOME -ErrorAction SilentlyContinue
    Remove-Item Env:PYTHONPATH -ErrorAction SilentlyContinue
    # Offline mode: the frozen core ships the complete toolchain/SDK cache;
    # dependency re-resolution against GitHub is neither needed nor allowed.
    $env:PLATFORMIO_OFFLINE = '1'
    $env:PLATFORMIO_CORE_DIR = $taskCore
    $env:PLATFORMIO_BUILD_DIR = "$taskP12/build"
    $env:PLATFORMIO_LIBDEPS_DIR = "$taskP12/libdeps"
    $env:PLATFORMIO_CACHE_DIR = "$taskCore/cache"
    $env:PLATFORMIO_PACKAGES_DIR = "$taskCore/packages"
    $env:PLATFORMIO_PLATFORMS_DIR = "$taskCore/platforms"
    $env:IDF_COMPONENT_CHECK_NEW_VERSION = '0'
    & (Join-Path $env:USERPROFILE '.platformio/penv/Scripts/python.exe') (Join-Path $taskRoot $taskBuilder)
    if ($LASTEXITCODE -ne 0) { throw "$Candidate build failed: $LASTEXITCODE" }
}
finally { Exit-FastBeeFirmwareBuildLock -Handle $handle }
