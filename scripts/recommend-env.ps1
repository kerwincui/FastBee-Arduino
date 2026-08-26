<#
.SYNOPSIS
    FastBee smart environment recommender: prints the recommended PlatformIO
    environment, flash partition table and peripheral driver combination for a
    given ESP32 chip variant, based on verified flash/RAM budgets.

.DESCRIPTION
    One production environment per chip spec (simplified 2026-08):
      - ESP32 classic 4MB   -> esp32-F4R0    (all features in, default disabled)
      - ESP32-C3   4MB      -> esp32c3-F4R0  (no Serial2)
      - ESP32-C6   4MB      -> esp32c6-F4R0  (3 UART)
      - ESP32-S3   8MB      -> esp32s3-F8R4  (PSRAM auto-budget on non-PSRAM boards)
      - ESP32-S3   16MB     -> esp32s3-F16R8 (flagship, all features)
    All environments ship every supported feature compiled in; features are
    enabled/disabled at runtime from the web "Feature Config" page.
    Web images are unified: every environment uses the full profile (2026-08).

    The -Chip value can also be auto-detected from a connected device with
    -DetectPort COMx (reads chip model via esptool).

.EXAMPLE
    .\scripts\recommend-env.ps1 -Chip esp32c3
    .\scripts\recommend-env.ps1 -DetectPort COM5
    .\scripts\recommend-env.ps1 -All
#>
[CmdletBinding()]
param(
    [ValidateSet("esp32", "esp32c3", "esp32c6", "esp32s3", "")]
    [string]$Chip = "",

    [string]$DetectPort = "",

    [switch]$All,

    # Flash size hint when chip alone is ambiguous (4 / 8 / 16)
    [ValidateSet(0, 4, 8, 16)]
    [int]$FlashMB = 0
)

$ErrorActionPreference = "Stop"

# Recommendation table: chip + flash -> env, partition csv, web profile, notes
# 2026-08 Web 镜像统一化：所有环境一律 full 档（运行时功能门控）
$Recommendations = @(
    [pscustomobject]@{ Chip="esp32";  FlashMB=4;  Env="esp32-F4R0";           Partition="partitions/fastbee.csv";      Web="full"; Notes="Default production target. 4MB flash cap: high-RAM features auto-rejected by boot budget." }
    [pscustomobject]@{ Chip="esp32c3";FlashMB=4;  Env="esp32c3-F4R0";         Partition="partitions/fastbee.csv";      Web="full"; Notes="2 UART only: cellular uses Serial1. 4MB flash cap applies." }
    [pscustomobject]@{ Chip="esp32c6";FlashMB=4;  Env="esp32c6-F4R0";         Partition="partitions/fastbee.csv";      Web="full"; Notes="3 UART: cellular may use Serial2. 4MB flash cap applies." }
    [pscustomobject]@{ Chip="esp32s3";FlashMB=8;  Env="esp32s3-F8R4";         Partition="partitions/fastbee-8MB.csv";  Web="full"; Notes="Non-PSRAM S3 boards degrade gracefully via boot budget." }
    [pscustomobject]@{ Chip="esp32s3";FlashMB=16; Env="esp32s3-F16R8";        Partition="partitions/fastbee-16MB.csv"; Web="full"; Notes="Flagship: all features incl. heavy peripherals." }
)

function Show-Table([object[]]$rows) {
    $rows | Format-Table Chip, FlashMB, Env, Partition, Web -AutoSize
    foreach ($r in $rows) {
        Write-Host ("  {0}: {1}" -f $r.Env, $r.Notes) -ForegroundColor DarkGray
    }
}

if ($All) {
    Write-Host "FastBee environment recommendation table" -ForegroundColor Green
    Show-Table $Recommendations
    Write-Host "`nAll environments compile every supported feature in; enable/disable" -ForegroundColor Green
    Write-Host "features at runtime from the web 'Feature Config' page."
    Write-Host "  NOTE: 4MB devices keep ~89% flash usage - no room for extra features." -ForegroundColor Yellow
    return
}

if ($DetectPort) {
    $IsWin = if (Test-Path Variable:IsWindows) { $IsWindows } else { $true }
    if ($IsWin) { $py = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe" }
    else        { $py = Join-Path $HOME ".platformio/penv/bin/python" }
    if (-not (Test-Path -LiteralPath $py)) { $py = (Get-Command python).Source }
    Write-Host "Reading chip model from $DetectPort ..." -ForegroundColor Cyan
    $info = & $py -m esptool --port $DetectPort chip_id 2>&1 | Out-String
    $flashInfo = & $py -m esptool --port $DetectPort flash_id 2>&1 | Out-String
    if ($info -match 'Chip is (ESP32-[CS]\d[^\s(]*)') {
        $model = $Matches[1]
        Write-Host "Detected: $model" -ForegroundColor Green
        if ($model -match 'C3') { $Chip = "esp32c3" }
        elseif ($model -match 'C6') { $Chip = "esp32c6" }
        elseif ($model -match 'S3') { $Chip = "esp32s3" }
        else { $Chip = "esp32" }
    } else {
        throw "Cannot detect chip on $DetectPort. Is the device connected and not held by a monitor?"
    }
    if ($flashInfo -match 'Detected flash size: (\d+)MB') {
        $FlashMB = [int]$Matches[1]
        Write-Host "Detected flash: $FlashMB MB" -ForegroundColor Green
    }
}

if (-not $Chip) {
    throw "Specify -Chip (esp32|esp32c3|esp32c6|esp32s3), -DetectPort COMx, or -All."
}

$rows = @($Recommendations | Where-Object { $_.Chip -eq $Chip })
if ($FlashMB -gt 0) {
    $exact = @($rows | Where-Object { $_.FlashMB -eq $FlashMB })
    if ($exact.Count -gt 0) { $rows = $exact }
}

Write-Host "Recommendation for chip '$Chip'$(if($FlashMB){' (' + $FlashMB + 'MB)'})" -ForegroundColor Green
Show-Table $rows

$pick = $rows | Select-Object -First 1
Write-Host "Deploy command:" -ForegroundColor Cyan
Write-Host "  .\scripts\deploy-all.ps1 -Env $($pick.Env)"
