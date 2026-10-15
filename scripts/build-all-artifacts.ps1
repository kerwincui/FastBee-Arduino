# FastBee-Arduino: Build All Artifacts Script
# ==========================================
# Unified Architecture v2.0 - Single Firmware Variant
#
# BUILD MATRIX (One firmware architecture across hardware targets):
#   • esp32-F4R0       → ESP32 Classic, 4MB Flash, no PSRAM
#   • esp32-F8R0       → ESP32 Classic, 8MB Flash, no PSRAM
#   • esp32-F8R8       → ESP32 Classic/WROVER, 8MB Flash + 8MB PSRAM
#   • esp32c3-F4R0     → ESP32-C3, 4MB Flash, no PSRAM
#   • esp32c6-F4R0     → ESP32-C6, 4MB Flash, no PSRAM
#   • esp32c6-F8R0     → ESP32-C6, 8MB Flash, no PSRAM
#   • esp32s3-F8R0     → ESP32-S3, 8MB Flash, no PSRAM
#   • esp32s3-F8R8     → ESP32-S3, 8MB Flash + 8MB PSRAM
#   • esp32s3-F16R8    → ESP32-S3, 16MB Flash + 8MB PSRAM
#
# DEPRECATED: Product editions were removed; use the unified capability model.
#              All features compile into single firmware; runtime budget control via FeatureBudget
#
# USAGE:
#   powershell scripts\build-all-artifacts.ps1                    # Build all 9 variants
#   powershell scripts\build-all-artifacts.ps1 -Env s3,F4R0       # Build specific env(s)
#
# OUTPUT:
#   dist\firmware\all-latest\manifest.json
#   dist\firmware\all-latest\fastbee-{chip}-{config}.bin
#
# ARTIFACT NAMING SCHEME:
#   fastbee-esp32-F4R0.bin
#   fastbee-esp32c3-F4R0.bin
#   fastbee-esp32c6-F4R0.bin
#   fastbee-esp32-F8R0.bin
#   fastbee-esp32-F8R8.bin
#   fastbee-esp32c6-F8R0.bin
#   fastbee-esp32s3-F8R0.bin
#   fastbee-esp32s3-F8R8.bin
#   fastbee-esp32s3-F16R8.bin
#

[CmdletBinding()]
param(
    [string[]]$Environments = @(
        "esp32-F4R0",
        "esp32-F8R0",
        "esp32-F8R8",
        "esp32c3-F4R0",
        "esp32c6-F4R0",
        "esp32c6-F8R0",
        "esp32s3-F8R0",
        "esp32s3-F8R8",
        "esp32s3-F16R8"
    ),
    [string]$OutputDir = "dist\firmware\all-latest",
    [switch]$SkipBuild,
    [switch]$CleanOutput
)

$ErrorActionPreference = "Stop"

# All supported targets use the same unified runtime/web profile. Keep this
# explicit so manifest commands do not depend on an undefined shell variable.
$Profile = "unified"

function ConvertTo-Hashtable {
    param([Parameter(Mandatory = $true, ValueFromPipeline = $true)][object]$InputObject)

    if ($null -eq $InputObject) { return $null }

    if ($InputObject -is [System.Collections.IDictionary]) {
        $table = @{}
        foreach ($key in $InputObject.Keys) {
            $table[$key] = ConvertTo-Hashtable $InputObject[$key]
        }
        return $table
    }

    if ($InputObject -is [System.Collections.IEnumerable] -and
        $InputObject -isnot [string]) {
        return @($InputObject | ForEach-Object { ConvertTo-Hashtable $_ })
    }

    if ($InputObject -is [pscustomobject]) {
        $table = @{}
        foreach ($property in $InputObject.PSObject.Properties) {
            $table[$property.Name] = ConvertTo-Hashtable $property.Value
        }
        return $table
    }

    return $InputObject
}

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
. (Join-Path $PSScriptRoot "firmware-build-lock.ps1")

# ─── 预构建：清理可能锁住 .bin 文件的残留进程 ───────────────────────────────
# PlatformIO 串口监视器退出后可能残留 esptool.exe / python.exe 进程，
# 导致后续编译报错："另一个程序正在使用此文件，进程无法访问"
function Kill-StaleProcesses {
    $killed = 0
    $procNames = @('esptool', 'python', 'xtensa-esp-elf-gcc', 'xtensa-esp-elf-g++', 'xtensa-esp-elf-ar', 'xtensa-esp-elf-ld')
    foreach ($name in $procNames) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
            if ($name -eq 'python') {
                if ($_.Path -notlike '*.platformio\*') { return }
                # 与 deploy.ps1 同一口径：penv python 也可能在跑本仓长期任务
                # （monitor-device-candidate.py 等），仅清理命令行确属构建活动的进程。
                $cmdLine = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction SilentlyContinue).CommandLine
                if (-not $cmdLine -or $cmdLine -notmatch '-m platformio|platformio\.py|scons|[\\/]\.pio[\\/]') { return }
            }
            Write-Host "  Killing stale $name (PID $($_.Id))" -ForegroundColor Yellow
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            $killed++
        }
    }
    if ($killed -gt 0) {
        Write-Host "  Cleaned $killed stale process(es), waiting for file locks to release..." -ForegroundColor Yellow
        Start-Sleep -Seconds 1
    }
}

# Release naming: fastbee-{chip}-F{flash}R{psram}.bin
# Examples: fastbee-esp32-F4R0.bin, fastbee-esp32s3-F16R8.bin
$OutputNameByEnv = @{
    "esp32-F4R0"     = "fastbee-esp32-F4R0.bin"
    "esp32-F8R0"     = "fastbee-esp32-F8R0.bin"
    "esp32-F8R8"     = "fastbee-esp32-F8R8.bin"
    "esp32c3-F4R0"   = "fastbee-esp32c3-F4R0.bin"
    "esp32c6-F4R0"   = "fastbee-esp32c6-F4R0.bin"
    "esp32c6-F8R0"   = "fastbee-esp32c6-F8R0.bin"
    "esp32s3-F8R0"   = "fastbee-esp32s3-F8R0.bin"
    "esp32s3-F8R8"   = "fastbee-esp32s3-F8R8.bin"
    "esp32s3-F16R8"  = "fastbee-esp32s3-F16R8.bin"
}
$HardwareByEnv = @{
    "esp32-F4R0"     = "ESP32 4MB Flash"
    "esp32-F8R0"     = "ESP32 8MB Flash"
    "esp32-F8R8"     = "ESP32 8MB Flash + 8MB PSRAM"
    "esp32c3-F4R0"   = "ESP32-C3 4MB Flash"
    "esp32c6-F4R0"   = "ESP32-C6 4MB Flash"
    "esp32c6-F8R0"   = "ESP32-C6 8MB Flash"
    "esp32s3-F8R0"   = "ESP32-S3 8MB Flash"
    "esp32s3-F8R8"   = "ESP32-S3 8MB Flash + 8MB PSRAM"
    "esp32s3-F16R8"  = "ESP32-S3 16MB Flash + 8MB PSRAM"
}
# 只收集完整固件文件(包含 bootloader + partitions + firmware + filesystem)
$FilesToCollect = @(
    "factory-with-fs.bin"
)
$RequiredFiles = @(
    "factory-with-fs.bin"
)

function Resolve-ProjectPath {
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        $FullPath = [System.IO.Path]::GetFullPath($Path)
    }
    else {
        $FullPath = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $Path))
    }

    $DistRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir "dist"))
    $DistRootWithSlash = $DistRoot.TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
    if (($FullPath -eq $DistRoot) -or (-not $FullPath.StartsWith($DistRootWithSlash, [System.StringComparison]::OrdinalIgnoreCase))) {
        throw "Output path must be a subdirectory under dist: $FullPath"
    }
    return $FullPath
}

function Get-ProjectRelativePath {
    param([string]$Path)

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $ProjectRoot = [System.IO.Path]::GetFullPath($ProjectDir).TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
    if ($FullPath.StartsWith($ProjectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $FullPath.Substring($ProjectRoot.Length).Replace("\", "/")
    }
    return $FullPath.Replace("\", "/")
}

function Invoke-Pio {
    param([string[]]$Arguments)

    Write-Host ""
    Write-Host "pio $($Arguments -join ' ')" -ForegroundColor Cyan
    & pio @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO command failed: pio $($Arguments -join ' ')"
    }
}

function Test-BuildCacheIntegrity {
    param([string]$BuildEnv)

    $buildDir = Join-Path $ProjectDir ".pio\build\$BuildEnv"
    if (-not (Test-Path -LiteralPath $buildDir -PathType Container)) { return }

    $libFile = Join-Path $buildDir "libFrameworkArduino.a"
    $hasArchive = Test-Path -LiteralPath $libFile -PathType Leaf
    $hasObjects = @(
        Get-ChildItem -LiteralPath $buildDir -Filter "*.o" -Recurse -ErrorAction SilentlyContinue
    ).Count -gt 0

    if ($hasObjects -and -not $hasArchive) {
        Write-Host "[Integrity] Build cache corrupted for $BuildEnv (archive missing). Cleaning..." -ForegroundColor Yellow
        & pio run -e $BuildEnv -t clean 2>$null | Out-Null
        return
    }

    $bootloader = Join-Path $buildDir "bootloader.bin"
    if (Test-Path -LiteralPath $bootloader -PathType Leaf) {
        try {
            $handle = [System.IO.File]::Open($bootloader, 'Open', 'ReadWrite', 'None')
            $handle.Close()
        }
        catch {
            Write-Host "[Integrity] bootloader.bin locked for $BuildEnv. Cleaning..." -ForegroundColor Yellow
            & pio run -e $BuildEnv -t clean 2>$null | Out-Null
        }
    }
}

function Invoke-PioWithRetry {
    param([string[]]$Arguments)

    Write-Host ""
    Write-Host "pio $($Arguments -join ' ')" -ForegroundColor Cyan
    & pio @Arguments
    if ($LASTEXITCODE -eq 0) { return }

    # 构建失败 → 清理缓存后重试一次
    Write-Host ""
    Write-Host "[Retry] Build failed, cleaning cache and rebuilding..." -ForegroundColor Yellow
    $envArg = $null
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        if ($Arguments[$i] -eq '-e' -and ($i + 1) -lt $Arguments.Count) {
            $envArg = $Arguments[$i + 1]
            break
        }
    }
    if ($envArg) {
        & pio run -e $envArg -t clean 2>$null | Out-Null
    }
    Kill-StaleProcesses

    Write-Host ""
    Write-Host "[Retry] pio $($Arguments -join ' ')" -ForegroundColor Cyan
    & pio @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed after retry: pio $($Arguments -join ' ')"
    }
    Write-Host "[Retry] Build succeeded after clean rebuild." -ForegroundColor Green
}

function Get-GitCommitSha {
    try {
        $sha = (& git rev-parse --short=12 HEAD 2>$null) -as [string]
        return $sha.Trim()
    }
    catch {
        return ""
    }
}

function Get-FileSha256 {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-EnvironmentMetadata {
    param([string]$Environment)

    $match = [regex]::Match($Environment, '^(esp32|esp32c3|esp32c6|esp32s3)-F(4|8|16)R(0|8)$')
    if (-not $match.Success) {
        throw "Unsupported release environment metadata: $Environment"
    }
    $flashMB = [int]$match.Groups[2].Value
    $psramMB = [int]$match.Groups[3].Value
    return [ordered]@{
        chip = $match.Groups[1].Value
        flashMB = $flashMB
        psramMB = $psramMB
        partition = "partitions/$($flashMB)MB.csv"
        otaCapable = $flashMB -ge 8
        otaSlots = if ($flashMB -ge 8) { 2 } else { 1 }
        signature = [ordered]@{
            signed = $false
            algorithm = "none"
            reason = "Secure Boot signing key is not configured for this open build"
        }
    }
}

function Copy-Artifact {
    param(
        [string]$SourceDir,
        [string]$SourceName,
        [string]$DestDir,
        [string]$DestName
    )

    if ([string]::IsNullOrWhiteSpace($DestName)) {
        throw "Destination artifact name is empty for $SourceName"
    }

    $SourcePath = Join-Path $SourceDir $SourceName
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        return $null
    }

    $DestPath = Join-Path $DestDir $DestName
    Copy-Item -LiteralPath $SourcePath -Destination $DestPath -Force
    return $DestName
}

function Invoke-WebArchiveVerifier {
    param(
        [string]$Environment,
        [string]$WebDirectory
    )

    $Verifier = Join-Path $ProjectDir "scripts\verify-web-release-assets.js"
    $Json = & node $Verifier "--env=$Environment" "--www-dir=$WebDirectory"
    if ($LASTEXITCODE -ne 0) {
        throw "Target Web archive verification failed for $Environment"
    }
    return ($Json | ConvertFrom-Json)
}

function Copy-WebArtifactDirectory {
    param(
        [string]$SourceDirectory,
        [string]$DestinationDirectory
    )

    $DestinationParent = Split-Path -Parent $DestinationDirectory
    New-Item -ItemType Directory -Path $DestinationParent -Force | Out-Null
    if (Test-Path -LiteralPath $DestinationDirectory) {
        Remove-Item -LiteralPath $DestinationDirectory -Recurse -Force
    }
    Copy-Item -LiteralPath $SourceDirectory -Destination $DestinationDirectory -Recurse
}

function Clear-OutputDirectory {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return
    }

    Get-ChildItem -LiteralPath $Path -Force -Recurse | ForEach-Object {
        try {
            $_.Attributes = [System.IO.FileAttributes]::Normal
        }
        catch {
            Write-Warning "Unable to reset attributes: $($_.FullName)"
        }
    }
    Get-ChildItem -LiteralPath $Path -Force | ForEach-Object {
        $ItemPath = $_.FullName
        try {
            Remove-Item -LiteralPath $ItemPath -Recurse -Force -ErrorAction Stop
        }
        catch {
            Write-Warning "Unable to remove old artifact, will overwrite if possible: $ItemPath"
        }
    }
}

$firmwareLock = Enter-FastBeeFirmwareBuildLock `
    -ProjectRoot $ProjectDir `
    -Label "build-all-artifacts"
$buildBlocked = $false
try {
Write-Host "[Pre-Build] Checking for stale processes..." -ForegroundColor Cyan
Kill-StaleProcesses

$OutputPath = Resolve-ProjectPath $OutputDir
Set-Location $ProjectDir

if (Test-Path -LiteralPath $OutputPath -PathType Leaf) {
    throw "Output path points to a file, not a directory: $OutputPath"
}
if ((Test-Path -LiteralPath $OutputPath) -and $CleanOutput) {
    Clear-OutputDirectory $OutputPath
}
New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null

$ReleaseManifest = [ordered]@{
    generatedAt = [System.DateTime]::UtcNow.ToString("o")
    gitSha = Get-GitCommitSha
    outputDir = Get-ProjectRelativePath $OutputPath
    environments = @()
}

foreach ($EnvName in $Environments) {
    # 所有硬件目标使用统一固件架构。
    $ReleaseName = [string]$OutputNameByEnv[$EnvName]
    if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        throw "Missing release file name mapping for $EnvName"
    }

    Write-Host ""
    Write-Host "=== FastBee $EnvName ===" -ForegroundColor Green
    $Metadata = Get-EnvironmentMetadata -Environment $EnvName

    if (-not $SkipBuild) {
        Test-BuildCacheIntegrity -BuildEnv $EnvName
        Kill-StaleProcesses
        Invoke-PioWithRetry -Arguments @("run", "-e", $EnvName)
        Invoke-PioWithRetry -Arguments @("run", "-e", $EnvName, "--target", "buildfs")
    }

    $EnvDist = Join-Path $ProjectDir "dist\firmware\$EnvName"
    if (-not (Test-Path -LiteralPath $EnvDist -PathType Container)) {
        throw "Missing artifact directory: $EnvDist"
    }
    $EnvWebDir = Join-Path $EnvDist "www"
    $SourceWebValidation = Invoke-WebArchiveVerifier -Environment $EnvName -WebDirectory $EnvWebDir
    $ReleaseWebDir = Join-Path (Join-Path $OutputPath $EnvName) "www"
    Copy-WebArtifactDirectory -SourceDirectory $EnvWebDir -DestinationDirectory $ReleaseWebDir
    $ReleaseWebValidation = Invoke-WebArchiveVerifier -Environment $EnvName -WebDirectory $ReleaseWebDir
    if ($SourceWebValidation.treeSha256 -ne $ReleaseWebValidation.treeSha256) {
        throw "Archived Web tree differs from the buildfs source for $EnvName"
    }

    $Entry = [ordered]@{
        environment = $EnvName
        filesystemProfile = $Profile
        hardware = $HardwareByEnv[$EnvName]
        chip = $Metadata.chip
        flashMB = $Metadata.flashMB
        psramMB = $Metadata.psramMB
        partition = $Metadata.partition
        otaCapable = $Metadata.otaCapable
        otaSlots = $Metadata.otaSlots
        signature = $Metadata.signature
        releaseFile = $ReleaseName
        deployCommand = "powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env $EnvName -Port COMx"
        sourceDir = Get-ProjectRelativePath $EnvDist
        webAssets = [ordered]@{
            directory = Get-ProjectRelativePath $ReleaseWebDir
            fileCount = $ReleaseWebValidation.fileCount
            treeSha256 = $ReleaseWebValidation.treeSha256
            requestGovernor = $ReleaseWebValidation.requestGovernor
        }
        smokeTest = [ordered]@{
            required = $true
            command = "powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip> -Profile $Profile"
            status = "not-run"
        }
        files = [ordered]@{}
        sizes = [ordered]@{}
        sha256 = [ordered]@{}
    }

    foreach ($SourceName in $FilesToCollect) {
        $DestName = $ReleaseName
        $CopiedName = Copy-Artifact -SourceDir $EnvDist -SourceName $SourceName -DestDir $OutputPath -DestName $DestName
        if ($CopiedName) {
            $CopiedPath = Join-Path $OutputPath $CopiedName
            $Entry.files[$SourceName] = $CopiedName
            $Entry.sizes[$CopiedName] = (Get-Item -LiteralPath $CopiedPath).Length
            $Entry.sha256[$CopiedName] = Get-FileSha256 -Path $CopiedPath
        }
    }

    $MissingFiles = @()
    foreach ($RequiredFile in $RequiredFiles) {
        if (-not $Entry.files.Contains($RequiredFile)) {
            $MissingFiles += $RequiredFile
        }
    }
    if ($MissingFiles.Count -gt 0) {
        throw "Missing required artifacts for ${EnvName}: $($MissingFiles -join ', ')"
    }

    $ReleaseManifest.environments += $Entry
}

$ManifestPath = Join-Path $OutputPath "manifest.json"
$ManifestJson = $ReleaseManifest | ConvertTo-Json -Depth 8
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson, $Utf8NoBom)

# ─── WP-71: 构建体积 delta 摘要和红线门禁 ──────────────────────────────
$ReportPath = Join-Path $ProjectDir "artifacts\build-size-report.json"
$BaselinePath = Join-Path $ProjectDir "scripts\.pio\build-baseline.json"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReportPath) | Out-Null

function Get-FirmwareSizeInfo {
    param([string]$Environment)

    # PlatformIO normally emits firmware.bin. Some post-processing flows only
    # keep factory.bin, so accept it as a fallback; never silently turn a
    # missing artifact into a zero-byte baseline.
    $candidates = @(
        (Join-Path $ProjectDir ".pio\build\$Environment\firmware.bin"),
        (Join-Path $ProjectDir ".pio\build\$Environment\factory.bin"),
        (Join-Path $ProjectDir "dist\firmware\$Environment\firmware.bin"),
        (Join-Path $ProjectDir "dist\firmware\$Environment\factory.bin")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $item = Get-Item -LiteralPath $candidate
            return [pscustomobject]@{
                size = [int64]$item.Length
                path = Get-ProjectRelativePath $candidate
            }
        }
    }
    throw "No firmware.bin or factory.bin found for $Environment; build artifacts are required for size reporting."
}

$currentSizes = @{}
foreach ($env in $Environments) {
    $sizeInfo = Get-FirmwareSizeInfo $env
    $size = $sizeInfo.size
    $currentSizes[$env] = @{
        firmwareBin = $size
        firmwarePath = $sizeInfo.path
        # Compare the application image only. factory-with-fs.bin is a padded
        # full-flash image and must not be added to the application size.
        total = $size
    }
}

# 读取基线对比
$baseline = @{}
if (Test-Path $BaselinePath) {
    $baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json | ConvertTo-Hashtable
}

$deltaReport = [ordered]@{
    timestamp = Get-Date -Format "yyyy-MM-ddTHH:mm:ssZ"
    platformioIniHash = if (Test-Path "platformio.ini") { 
        (Get-FileHash "platformio.ini" -Algorithm SHA256).Hash 
    } else { "unknown" }
}

$totalDeltaKB = 0
$thresholdFlashKB = 2048  # 2KB 红线
$thresholdWarningKB = 512 # 500B 警告
$appCapacityByFlashMB = @{
    4 = 0x330000  # partitions/4MB.csv app0 (3.1875MB; single app)
    8 = 0x380000  # partitions/8MB.csv app0/app1
    16 = 0x700000 # partitions/16MB.csv app0/app1
}
$capacityWarningPercent = 90
$capacityBlockPercent = 98

function Get-FlashSizeMBFromEnvironment {
    param([string]$Environment)
    $match = [regex]::Match($Environment, '-F(4|8|16)R(?:0|4|8)$')
    if ($match.Success) { return [int]$match.Groups[1].Value }
    return 0
}

foreach ($env in $currentSizes.Keys) {
    $flashMB = Get-FlashSizeMBFromEnvironment $env
    $appCapacity = $appCapacityByFlashMB[$flashMB]
    $usagePercent = if ($appCapacity) {
        [math]::Round(($currentSizes[$env].total / $appCapacity) * 100, 2)
    } else { 0 }
    $capacityGate = "pass"
    if ($usagePercent -ge $capacityBlockPercent) {
        $capacityGate = "block"
    } elseif ($usagePercent -ge $capacityWarningPercent) {
        $capacityGate = "warn"
    }

    $baselineEntry = $baseline[$env]
    $baselineTotal = if ($baselineEntry) { [int64]$baselineEntry.total } else { 0 }
    if (-not $baselineEntry -or $baselineTotal -le 0) {
        $deltaReport[$env] = @{
            status = "baseline-reset"
            flashKB = [math]::Round($currentSizes[$env].total / 1024, 2)
            deltaKB = "N/A (baseline missing)"
            gate = "pass"
            artifact = $currentSizes[$env].firmwarePath
            appCapacityKB = [math]::Round($appCapacity / 1024, 2)
            usagePercent = $usagePercent
            capacityGate = $capacityGate
        }
    } else {
        $prev = $baselineTotal
        $curr = $currentSizes[$env].total
        $delta = $curr - $prev
        $deltaKB = [math]::Round($delta / 1024, 2)
        $totalDeltaKB += $deltaKB
        
        $gate = "pass"
        if ($delta -gt ($thresholdFlashKB * 1024)) {
            $gate = "block"
        } elseif ($delta -gt ($thresholdWarningKB * 1024)) {
            $gate = "warn"
        }
        
        $deltaReport[$env] = @{
            status = "updated"
            flashKB = [math]::Round($curr / 1024, 2)
            prevKB = [math]::Round($prev / 1024, 2)
            deltaKB = $deltaKB
            gate = if ($capacityGate -eq "block" -or $gate -eq "block") { "block" } elseif ($capacityGate -eq "warn" -or $gate -eq "warn") { "warn" } else { "pass" }
            artifact = $currentSizes[$env].firmwarePath
            appCapacityKB = [math]::Round($appCapacity / 1024, 2)
            usagePercent = $usagePercent
            capacityGate = $capacityGate
            explanation = $null  # 超过 500B 需要人工补充解释字段
        }
    }
}

$deltaReport.totalDeltaAcrossAllEnvsKB = [math]::Round($totalDeltaKB, 2)
$hasCapacityBlock = @($deltaReport.Keys | Where-Object { $_ -match '^esp32' -and $deltaReport[$_].capacityGate -eq 'block' }).Count -gt 0
$hasCapacityWarning = @($deltaReport.Keys | Where-Object { $_ -match '^esp32' -and $deltaReport[$_].capacityGate -eq 'warn' }).Count -gt 0
$deltaReport.gateSummary = if ($hasCapacityBlock -or $totalDeltaKB -gt $thresholdFlashKB) { "BLOCKED" } elseif ($hasCapacityWarning -or $totalDeltaKB -gt $thresholdWarningKB) { "WARNING" } else { "PASSED" }

# 保存报告
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($ReportPath, ($deltaReport | ConvertTo-Json -Depth 5), $Utf8NoBom)

# 保存/更新基线
$currentBaseline = @{}
foreach ($env in $currentSizes.Keys) {
    $currentBaseline[$env] = @{
        total = $currentSizes[$env].total
        firmwareBin = $currentSizes[$env].firmwareBin
        firmwarePath = $currentSizes[$env].firmwarePath
        recordedAt = Get-Date -Format "yyyy-MM-ddTHH:mm:ssZ"
    }
}
if (-not (Test-Path (Split-Path $BaselinePath -Parent))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $BaselinePath -Parent) | Out-Null
}
[System.IO.File]::WriteAllText($BaselinePath, ($currentBaseline | ConvertTo-Json -Depth 3), $Utf8NoBom)

if ($deltaReport.gateSummary -eq "BLOCKED") {
    Write-Host "[WP-71 GATE] BUILD BLOCKED: size delta or app partition capacity threshold exceeded!" -ForegroundColor Red
    Write-Host "Details: $ReportPath" -ForegroundColor Red
    $buildBlocked = $true
} elseif ($deltaReport.gateSummary -eq "WARNING") {
    Write-Host "[WP-71 GATE] WARNING: size delta or app partition usage requires review!" -ForegroundColor Yellow
    Write-Host "Details: $ReportPath" -ForegroundColor Yellow
} else {
    Write-Host "[WP-71] Build size delta within limits: $($deltaReport.totalDeltaAcrossAllEnvsKB) KB" -ForegroundColor Green
    Write-Host "Report saved to: $ReportPath" -ForegroundColor Gray
}
# ────────────────────────────────────────────────────────────────────────────
}
finally {
    Exit-FastBeeFirmwareBuildLock -Handle $firmwareLock
}

if ($buildBlocked) { exit 1 }

Write-Host ""
Write-Host "All artifacts are ready:" -ForegroundColor Green
Write-Host $OutputPath
