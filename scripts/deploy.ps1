[CmdletBinding()]
param(
    [ValidateSet("esp32", "esp32c3", "esp32c6", "esp32s3", "esp32s3-full")]
    [string]$Env = "esp32",

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
    Get-Process -Name esptool -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  Killing stale esptool.exe (PID $($_.Id))" -ForegroundColor Yellow
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        $killed++
    }
    Get-Process -Name python -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path -like "*\.platformio\*"
    } | ForEach-Object {
        Write-Host "  Killing stale python.exe (PID $($_.Id))" -ForegroundColor Yellow
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        $killed++
    }
    if ($killed -gt 0) {
        Write-Host "  Cleaned $killed stale process(es)" -ForegroundColor Yellow
        Start-Sleep -Milliseconds 500
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

if (-not $SkipFs) {
    if ($BuildOnly) {
        Invoke-FastBeePio -Arguments @("run", "-e", $Env, "--target", "buildfs")
    }
    else {
        Invoke-FastBeePio -Arguments (Add-UploadPort @("run", "-e", $Env, "--target", "uploadfs"))
    }
}

if (-not $SkipFirmware) {
    if ($BuildOnly) {
        Invoke-FastBeePio -Arguments @("run", "-e", $Env)
    }
    else {
        Invoke-FastBeePio -Arguments (Add-UploadPort @("run", "-e", $Env, "--target", "upload"))
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
