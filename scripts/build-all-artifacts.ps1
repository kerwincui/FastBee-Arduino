[CmdletBinding()]
param(
    [string[]]$Environments = @(
        "esp32",
        "esp32c3",
        "esp32s3",
        "esp32c6",
        "esp32s3-full"
    ),
    [string]$OutputDir = "dist\firmware\all-latest",
    [switch]$SkipBuild,
    [switch]$CleanOutput
)

$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

# ─── 预构建：清理可能锁住 .bin 文件的残留进程 ───────────────────────────────
# PlatformIO 串口监视器退出后可能残留 esptool.exe / python.exe 进程，
# 导致后续编译报错："另一个程序正在使用此文件，进程无法访问"
function Kill-StaleProcesses {
    $killed = 0
    # 清理 esptool
    Get-Process -Name esptool -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  Killing stale esptool.exe (PID $($_.Id))" -ForegroundColor Yellow
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        $killed++
    }
    # 清理 PlatformIO 相关 python 进程（仅限 .platformio 路径下的）
    $pyProcs = Get-Process -Name python -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like "*\.platformio\*" }
    if ($pyProcs) {
        $pyProcs | ForEach-Object {
            Write-Host "  Killing stale python.exe (PID $($_.Id))" -ForegroundColor Yellow
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            $killed++
        }
    }
    if ($killed -gt 0) {
        Write-Host "  Cleaned $killed stale process(es)" -ForegroundColor Yellow
        Start-Sleep -Milliseconds 500  # 等待文件锁释放
    }
}

Write-Host "[Pre-Build] Checking for stale processes..." -ForegroundColor Cyan
Kill-StaleProcesses
$ProfileByEnv = @{
    "esp32c3"     = "lite"
    "esp32c6"     = "lite"
    "esp32"       = "standard"
    "esp32s3"     = "standard"
    "esp32s3-full" = "full"
}
# Release naming: fastbee-{chip}n{flash}r{psram}-{edition}.bin
# Examples: fastbee-esp32n4r0-std.bin, fastbee-esp32s3n16r8-full.bin
$OutputNameByEnv = @{
    "esp32"        = "fastbee-esp32n4r0-std.bin"
    "esp32c3"      = "fastbee-esp32c3n4r0-lite.bin"
    "esp32s3"      = "fastbee-esp32s3n8r0-std.bin"
    "esp32c6"      = "fastbee-esp32c6n8r0-lite.bin"
    "esp32s3-full" = "fastbee-esp32s3n16r8-full.bin"
}
$HardwareByEnv = @{
    "esp32"        = "ESP32 4MB Flash"
    "esp32c3"      = "ESP32-C3 4MB Flash"
    "esp32s3"      = "ESP32-S3 8MB Flash"
    "esp32c6"      = "ESP32-C6 8MB Flash"
    "esp32s3-full" = "ESP32-S3 16MB Flash + 8MB PSRAM"
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
    if (-not $ProfileByEnv.ContainsKey($EnvName)) {
        throw "Unknown build environment: $EnvName"
    }

    $Profile = $ProfileByEnv[$EnvName]
    $ReleaseName = [string]$OutputNameByEnv[$EnvName]
    if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        throw "Missing release file name mapping for $EnvName"
    }

    Write-Host ""
    Write-Host "=== FastBee $EnvName ($Profile) ===" -ForegroundColor Green

    if (-not $SkipBuild) {
        Invoke-Pio -Arguments @("run", "-e", $EnvName)
        Invoke-Pio -Arguments @("run", "-e", $EnvName, "--target", "buildfs")
    }

    $EnvDist = Join-Path $ProjectDir "dist\firmware\$EnvName"
    if (-not (Test-Path -LiteralPath $EnvDist -PathType Container)) {
        throw "Missing artifact directory: $EnvDist"
    }

    $Entry = [ordered]@{
        environment = $EnvName
        filesystemProfile = $Profile
        hardware = $HardwareByEnv[$EnvName]
        releaseFile = $ReleaseName
        deployCommand = "powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env $EnvName -Port COMx"
        sourceDir = Get-ProjectRelativePath $EnvDist
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

Write-Host ""
Write-Host "All artifacts are ready:" -ForegroundColor Green
Write-Host $OutputPath
