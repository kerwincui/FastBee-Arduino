<#
.SYNOPSIS
    FastBee one-click full deployment: build once, auto-detect serial ports,
    flash multiple devices in parallel.

.DESCRIPTION
    Pipeline:
      1. Build firmware + filesystem for the selected environment
      2. Merge into a single factory-with-fs.bin (bootloader + partitions +
         firmware + LittleFS), flashable at offset 0x0
      3. Auto-detect available serial ports (CP210x / CH340 / FTDI / ESP CDC)
      4. Flash all detected (or user-specified) devices in parallel with esptool

    Safe defaults: no config erase, no flash size change, factory image only.
    High-risk operations (full chip erase etc.) are intentionally NOT included.

.EXAMPLE
    # Build esp32-F4R0 and flash every connected device in parallel
    .\scripts\deploy-all.ps1

.EXAMPLE
    # Specific ports only
    .\scripts\deploy-all.ps1 -Env esp32c3-F4R0 -Ports COM5,COM7

.EXAMPLE
    # Detect ports without flashing
    .\scripts\deploy-all.ps1 -ListPortsOnly

.EXAMPLE
    # Reuse an existing build (skip compile)
    .\scripts\deploy-all.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [ValidateSet("esp32-F4R0", "esp32-F8R0", "esp32-F8R8", "esp32c3-F4R0", "esp32c6-F4R0",
                 "esp32c6-F8R0", "esp32s3-F8R0", "esp32s3-F8R8", "esp32s3-F16R8")]
    [string]$Env = "esp32-F4R0",

    # Explicit port list; empty = auto-detect
    [string[]]$Ports = @(),

    # Max parallel flash jobs (esptool instances). Keep modest: USB-UART
    # adapters share hubs and too much parallelism causes brownouts.
    [int]$MaxParallel = 2,

    [int]$Baud = 921600,

    [switch]$SkipBuild,
    [switch]$ListPortsOnly,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$IsWin = if (Test-Path Variable:IsWindows) { $IsWindows } else { $true }
$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir
. (Join-Path $PSScriptRoot "firmware-build-lock.ps1")

$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Green
}

function Get-ChipName([string]$Environment) {
    if ($Environment -match '^(esp32[a-z0-9]*)-') { return $Matches[1] }
    return "esp32"
}

function Get-PioPython {
    if ($IsWin) {
        $p = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
    } else {
        $p = Join-Path $HOME ".platformio/penv/bin/python"
    }
    if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command python3 -ErrorAction SilentlyContinue }
    if ($cmd) { return $cmd.Source }
    throw "Python not found. Install PlatformIO first so esptool is available."
}

function Get-EsptoolPorts {
    # Parse `pio device list --json-output`; return { port, description } objects
    $raw = & pio device list --json-output 2>$null
    $json = ($raw | Out-String) | ConvertFrom-Json
    $result = @()
    foreach ($item in @($json)) {
        foreach ($p in @($item.ports)) {
            $desc = [string]$p.description
            $hwid = [string]$p.hwid
            # Keep only likely ESP32 dev boards / USB-UART bridges
            if ($desc -match 'CP210|CH340|CH910|FT23|FTDI|ESP32|Espressif|USB-SERIAL|UART|CDC' -or
                $hwid -match '10C4:EA60|1A86:7523|1A86:55D4|0403:6001|303A:|10C4:EA70') {
                $result += [pscustomobject]@{
                    Port = [string]$p.port
                    Description = $desc
                }
            }
        }
    }
    return $result
}

$firmwareLock = Enter-FastBeeFirmwareBuildLock `
    -ProjectRoot $ProjectDir `
    -Label "deploy-all" `
    -Environment $Env `
    -Port ($Ports -join ",")
try {
# ---------------------------------------------------------------- step 1: build
$factoryImage = Join-Path $ProjectDir ".pio\build\$Env\factory-with-fs.bin"
if (-not $IsWin) { $factoryImage = $factoryImage -replace '\\', '/' }

if ($SkipBuild) {
    Write-Step "SkipBuild requested, reusing existing image"
} else {
    Write-Step "Building environment '$Env' (firmware + filesystem)"
    & pio run -e $Env
    if ($LASTEXITCODE -ne 0) { throw "Build failed for environment '$Env' (exit $LASTEXITCODE)" }
}

if (-not (Test-Path -LiteralPath $factoryImage -PathType Leaf)) {
    # Fallback: plain factory.bin (no FS merged) still flashes at 0x0
    $alt = Join-Path $ProjectDir ".pio\build\$Env\factory.bin"
    if (-not $IsWin) { $alt = $alt -replace '\\', '/' }
    if (Test-Path -LiteralPath $alt -PathType Leaf) {
        Write-Warning "factory-with-fs.bin not found; using factory.bin (filesystem NOT included)"
        $factoryImage = $alt
    } else {
        throw "No factory image found under .pio\build\$Env. Check extra_scripts merge step."
    }
}
$imageSizeKB = [math]::Round((Get-Item -LiteralPath $factoryImage).Length / 1KB)
Write-Host "Image: $factoryImage ($imageSizeKB KB)" -ForegroundColor Cyan

# ---------------------------------------------------------------- step 2: ports
Write-Step "Detecting serial ports"
$detected = Get-EsptoolPorts
if ($detected.Count -eq 0) {
    Write-Warning "No ESP32-compatible serial ports detected."
} else {
    foreach ($d in $detected) {
        Write-Host ("  {0,-12} {1}" -f $d.Port, $d.Description)
    }
}

if ($ListPortsOnly) { return }

if ($Ports.Count -gt 0) {
    $targets = @($Ports)
} else {
    $targets = @($detected | ForEach-Object { $_.Port })
}
if ($targets.Count -eq 0) {
    throw "No target ports. Connect devices or pass -Ports explicitly."
}
Write-Host "Targets: $($targets -join ', ') (MaxParallel=$MaxParallel)" -ForegroundColor Cyan

# ---------------------------------------------------------------- step 3: flash
$chip = Get-ChipName $Env
$python = Get-PioPython

function Invoke-Flash([string]$TargetPort) {
    $a = @(
        "-m", "esptool",
        "--chip", $chip,
        "--port", $TargetPort,
        "--baud", [string]$Baud,
        "--before", "default-reset",
        "--after", "hard-reset",
        "write-flash", "-z",
        "0x0", $factoryImage
    )
    & $python @a 2>&1 | ForEach-Object { "[$TargetPort] $_" }
    return $LASTEXITCODE
}

if ($DryRun) {
    Write-Step "DRY RUN: the following commands would execute"
    foreach ($t in $targets) {
        Write-Host "$python -m esptool --chip $chip --port $t --baud $Baud write-flash -z 0x0 $factoryImage" -ForegroundColor DarkCyan
    }
    return
}

Write-Step "Flashing $($targets.Count) device(s) in parallel (max $MaxParallel concurrent)"
$jobs = @{}
$done = @()
$failed = @()
$idx = 0

while ($done.Count + $failed.Count -lt $targets.Count -or $jobs.Count -gt 0) {
    # spawn up to MaxParallel
    while ($jobs.Count -lt $MaxParallel -and $idx -lt $targets.Count) {
        $t = $targets[$idx]; $idx++
        $scriptBlock = {
            param($TargetPort, $ChipName, $PythonExe, $Image, $BaudRate)
            $a = @("-m", "esptool", "--chip", $ChipName, "--port", $TargetPort,
                   "--baud", "$BaudRate", "--before", "default-reset",
                   "--after", "hard-reset", "write-flash", "-z", "0x0", $Image)
            $out = & $PythonExe @a 2>&1 | Out-String
            [pscustomobject]@{ Port = $TargetPort; ExitCode = $LASTEXITCODE; Output = $out }
        }
        $jobs[$t] = Start-Job -ScriptBlock $scriptBlock -ArgumentList $t, $chip, $python, $factoryImage, $Baud
        Write-Host "  Started flash job for $t" -ForegroundColor DarkCyan
    }
    Start-Sleep -Milliseconds 500
    foreach ($t in @($jobs.Keys)) {
        $j = $jobs[$t]
        if ($j.State -eq 'Completed' -or $j.State -eq 'Failed') {
            $r = Receive-Job $j -ErrorAction SilentlyContinue
            Remove-Job $j -Force
            $jobs.Remove($t)
            if ($r -and $r.ExitCode -eq 0) {
                Write-Host "  [OK]   $t flashed successfully" -ForegroundColor Green
                $done += $t
            } else {
                Write-Host "  [FAIL] $t (exit=$($r.ExitCode))" -ForegroundColor Red
                if ($r) { Write-Host ($r.Output -split "`n" | Select-Object -Last 5) -ForegroundColor DarkRed }
                $failed += $t
            }
        }
    }
}

Write-Step "Summary"
Write-Host "  Environment : $Env"
Write-Host "  Image       : $factoryImage"
Write-Host "  Succeeded   : $($done -join ', ')" -ForegroundColor Green
if ($failed.Count -gt 0) {
    Write-Host "  Failed      : $($failed -join ', ')" -ForegroundColor Red
    throw "Batch flash finished with $($failed.Count) failure(s)."
}
Write-Host "All devices flashed successfully." -ForegroundColor Green
}
finally {
    Exit-FastBeeFirmwareBuildLock -Handle $firmwareLock
}
