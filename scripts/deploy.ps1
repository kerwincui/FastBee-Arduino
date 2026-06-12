[CmdletBinding()]
param(
    [ValidateSet("esp32-F4R0", "esp32-F8R4", "esp32c3-F4R0", "esp32c6-F4R0", "esp32s3-F8R0", "esp32s3-F8R4", "esp32s3-F16R8")]
    [string]$Env = "esp32-F4R0",

    [string]$Port = "",

    [switch]$BuildOnly,
    [switch]$SkipFs,
    [switch]$SkipFirmware,
    [switch]$Monitor,
    [switch]$SkipDoctor,

    [string]$DataDir = "",

    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir

# ─── 清理残留进程（避免“另一个程序正在使用此文件”错误）───────────────
function Kill-StaleProcesses {
    $killed = 0
    # 进程名称列表：esptool、PlatformIO python、xtensa 工具链（gcc/ar/ld 等）
    $procNames = @('esptool', 'python', 'xtensa-esp-elf-gcc', 'xtensa-esp-elf-g++', 'xtensa-esp-elf-ar', 'xtensa-esp-elf-ld')
    foreach ($name in $procNames) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
            # python 仅清理 .platformio 路径下的，避免误杀用户进程
            if ($name -eq 'python' -and ($_.Path -notlike '*\.platformio\*')) { return }
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

Kill-StaleProcesses

function Initialize-PlatformIoDataDir {
    if ([string]::IsNullOrWhiteSpace($DataDir)) {
        if (-not [string]::IsNullOrWhiteSpace($env:PLATFORMIO_DATA_DIR)) {
            Write-Host "Clearing inherited PLATFORMIO_DATA_DIR: $env:PLATFORMIO_DATA_DIR" -ForegroundColor Yellow
            Remove-Item Env:\PLATFORMIO_DATA_DIR -ErrorAction SilentlyContinue
        }
        return
    }

    if ([System.IO.Path]::IsPathRooted($DataDir)) {
        $resolvedDataDir = [System.IO.Path]::GetFullPath($DataDir)
    }
    else {
        $resolvedDataDir = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $DataDir))
    }

    New-Item -ItemType Directory -Path $resolvedDataDir -Force | Out-Null
    $env:PLATFORMIO_DATA_DIR = $resolvedDataDir
    Write-Host "Using PLATFORMIO_DATA_DIR: $resolvedDataDir" -ForegroundColor Yellow
}

function Invoke-FastBeePio {
    param([string[]]$Arguments)

    Write-Host ""
    Write-Host "pio $($Arguments -join ' ')" -ForegroundColor Cyan
    & pio @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO command failed: pio $($Arguments -join ' ')"
    }
}

# ─── 构建缓存完整性检查（防止 "libFrameworkArduino.a: No such file" 错误）────
function Test-BuildCacheIntegrity {
    param([string]$BuildEnv)

    $buildDir = Join-Path $ProjectDir ".pio\build\$BuildEnv"
    if (-not (Test-Path -LiteralPath $buildDir -PathType Container)) { return }

    $libFile = Join-Path $buildDir "libFrameworkArduino.a"
    $hasArchive = Test-Path -LiteralPath $libFile -PathType Leaf
    $hasObjects = @(
        Get-ChildItem -LiteralPath $buildDir -Filter "*.o" -Recurse -ErrorAction SilentlyContinue
    ).Count -gt 0

    # 编译产物存在但 .a 归档缺失 → 上次归档步骤被中断（如进程锁定/强杀）
    if ($hasObjects -and -not $hasArchive) {
        Write-Host "[Integrity] Build cache corrupted for $BuildEnv (archive missing, objects present)." -ForegroundColor Yellow
        Write-Host "[Integrity] Running clean build to recover..." -ForegroundColor Yellow
        & pio run -e $BuildEnv -t clean 2>$null | Out-Null
        return
    }

    # bootloader.bin 存在但被锁定 → 残留进程可能还在写
    $bootloader = Join-Path $buildDir "bootloader.bin"
    if (Test-Path -LiteralPath $bootloader -PathType Leaf) {
        try {
            $handle = [System.IO.File]::Open($bootloader, 'Open', 'ReadWrite', 'None')
            $handle.Close()
        }
        catch {
            Write-Host "[Integrity] bootloader.bin is locked by another process, cleaning build dir..." -ForegroundColor Yellow
            & pio run -e $BuildEnv -t clean 2>$null | Out-Null
        }
    }
}

# ─── 带自动重试的 PlatformIO 构建/上传 ──────────────────────────────────
function Invoke-FastBeePioBuild {
    param([string[]]$Arguments)

    # 判断是否为上传目标（upload / uploadfs），上传失败时不清理编译缓存
    $isUpload = $false
    $envArg = $null
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        if ($Arguments[$i] -eq '-e' -and ($i + 1) -lt $Arguments.Count) {
            $envArg = $Arguments[$i + 1]
        }
        if ($Arguments[$i] -eq '--target' -and ($i + 1) -lt $Arguments.Count) {
            $t = $Arguments[$i + 1]
            if ($t -eq 'upload' -or $t -eq 'uploadfs') { $isUpload = $true }
        }
    }

    Write-Host ""
    Write-Host "pio $($Arguments -join ' ')" -ForegroundColor Cyan
    & pio @Arguments
    if ($LASTEXITCODE -eq 0) { return }

    if ($isUpload) {
        # 上传失败 → 等串口稳定后直接重试（不清理编译缓存）
        Write-Host ""
        Write-Host "[Retry] Upload failed, waiting 5s for serial port to stabilize..." -ForegroundColor Yellow
        Start-Sleep -Seconds 5
        Kill-StaleProcesses
        Write-Host "[Retry] pio $($Arguments -join ' ')" -ForegroundColor Cyan
        & pio @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Upload failed after retry: pio $($Arguments -join ' ')"
        }
        Write-Host "[Retry] Upload succeeded on second attempt." -ForegroundColor Green
        return
    }

    # 编译失败 → 清理缓存后重试一次
    Write-Host ""
    Write-Host "[Retry] Build failed, cleaning cache and rebuilding..." -ForegroundColor Yellow
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

function Add-UploadPort {
    param([string[]]$Arguments)

    if ([string]::IsNullOrWhiteSpace($Port)) {
        return $Arguments
    }
    return $Arguments + @("--upload-port", $Port)
}

if ($SkipFs -and $SkipFirmware) {
    throw "Nothing to do: remove -SkipFs or -SkipFirmware."
}

Initialize-PlatformIoDataDir

if (-not $SkipDoctor) {
    $doctorArgs = @("powershell", "-ExecutionPolicy", "Bypass", "-File", "scripts\doctor.ps1")
    if ($Port) {
        $doctorArgs += @("-Port", $Port)
    }
    Invoke-FastBeePio -Arguments @("--version")
    Write-Host ""
    Write-Host "$($doctorArgs -join ' ')" -ForegroundColor Cyan
    $doctorExe = $doctorArgs[0]
    $doctorRest = @()
    if ($doctorArgs.Count -gt 1) {
        $doctorRest = $doctorArgs[1..($doctorArgs.Count - 1)]
    }
    & $doctorExe @doctorRest
    if ($LASTEXITCODE -ne 0) {
        throw "Doctor check failed. Use -SkipDoctor only when the environment was already checked."
    }
}

Write-Host "FastBee deploy" -ForegroundColor Green
Write-Host "  Environment : $Env"
if ($Port) {
    Write-Host "  Port        : $Port"
}
Write-Host "  Mode        : $(if ($BuildOnly) { 'build only' } else { 'build and upload' })"

Test-BuildCacheIntegrity -BuildEnv $Env

if (-not $SkipFs) {
    if ($BuildOnly) {
        Invoke-FastBeePioBuild -Arguments @("run", "-e", $Env, "--target", "buildfs")
    }
    else {
        Invoke-FastBeePioBuild -Arguments (Add-UploadPort @("run", "-e", $Env, "--target", "uploadfs"))
        # uploadfs 完成后板子会重启，等待串口稳定再进行下一步
        Write-Host "Waiting 3s for board to reset after uploadfs..." -ForegroundColor DarkGray
        Start-Sleep -Seconds 3
    }
}

if (-not $SkipFirmware) {
    if ($BuildOnly) {
        Invoke-FastBeePioBuild -Arguments @("run", "-e", $Env)
    }
    else {
        Invoke-FastBeePioBuild -Arguments (Add-UploadPort @("run", "-e", $Env, "--target", "upload"))
    }
}

Write-Host ""
Write-Host "Deploy step completed." -ForegroundColor Green

if ($Monitor -and -not $BuildOnly) {
    $monitorArgs = @("device", "monitor", "-e", $Env, "-b", [string]$Baud)
    if ($Port) {
        $monitorArgs += @("-p", $Port)
    }
    Invoke-FastBeePio -Arguments $monitorArgs
}
