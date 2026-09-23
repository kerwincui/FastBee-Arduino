<#
.SYNOPSIS
    FastBee smart environment recommender: prints the recommended PlatformIO
    environment, flash partition table and peripheral driver combination for a
    given ESP32 chip variant, based on verified flash/RAM budgets.

.DESCRIPTION
    Canonical memory-qualified environments:
      - ESP32 classic 4MB/0  -> esp32-F4R0
      - ESP32 classic 8MB/0  -> esp32-F8R0
      - ESP32 classic 8MB/8  -> esp32-F8R8 (WROVER)
      - ESP32-C3 4MB/0       -> esp32c3-F4R0
      - ESP32-C6 4MB/0       -> esp32c6-F4R0
      - ESP32-C6 8MB/0       -> esp32c6-F8R0
      - ESP32-S3 8MB/0       -> esp32s3-F8R0
      - ESP32-S3 8MB/8       -> esp32s3-F8R8
      - ESP32-S3 16MB/8      -> esp32s3-F16R8
    All environments ship every supported feature compiled in; features are
    enabled/disabled at runtime from the web "Feature Config" page.
    Web images are unified: assets follow each target's compiled capabilities.

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

# Recommendation table: chip + flash + PSRAM -> env, module, partition, notes
# 2026-08 Web 资源统一化：所有环境使用统一入口，运行时按能力门控。
$Recommendations = @(
    [pscustomobject]@{ Chip="esp32";   FlashMB=4;  PsramMB=0; Env="esp32-F4R0";   Module="ESP32-WROOM-32E-N4";   Partition="partitions/4MB.csv";  Web="capability-driven"; Tier="P0"; Notes="Minimum compatibility target; single app slot, OTA unavailable." }
    [pscustomobject]@{ Chip="esp32";   FlashMB=8;  PsramMB=0; Env="esp32-F8R0";   Module="ESP32-WROOM-32E-N8";   Partition="partitions/8MB.csv";  Web="capability-driven"; Tier="P0"; Notes="Classic ESP32 no-PSRAM OTA target." }
    [pscustomobject]@{ Chip="esp32";   FlashMB=8;  PsramMB=8; Env="esp32-F8R8";   Module="ESP32-WROVER-B-N8R8";  Partition="partitions/8MB.csv";  Web="capability-driven"; Tier="P1"; Notes="Classic ESP32 PSRAM compatibility target." }
    [pscustomobject]@{ Chip="esp32c3"; FlashMB=4;  PsramMB=0; Env="esp32c3-F4R0"; Module="ESP32-C3-WROOM-N4";   Partition="partitions/4MB.csv";  Web="capability-driven"; Tier="P0"; Notes="Low-resource target; single app slot, OTA unavailable." }
    [pscustomobject]@{ Chip="esp32c6"; FlashMB=4;  PsramMB=0; Env="esp32c6-F4R0"; Module="ESP32-C6-WROOM-N4";   Partition="partitions/4MB.csv";  Web="capability-driven"; Tier="P0"; Notes="Constrained C6 target; single app slot, OTA unavailable." }
    [pscustomobject]@{ Chip="esp32c6"; FlashMB=8;  PsramMB=0; Env="esp32c6-F8R0"; Module="ESP32-C6-WROOM-N8";   Partition="partitions/8MB.csv";  Web="capability-driven"; Tier="P0"; Notes="Official C6 DevKitC-1 class target with A/B OTA." }
    [pscustomobject]@{ Chip="esp32s3"; FlashMB=8;  PsramMB=0; Env="esp32s3-F8R0"; Module="ESP32-S3-WROOM-1-N8";  Partition="partitions/8MB.csv";  Web="capability-driven"; Tier="P0"; Notes="S3 no-PSRAM OTA and memory-constraint target." }
    [pscustomobject]@{ Chip="esp32s3"; FlashMB=8;  PsramMB=8; Env="esp32s3-F8R8"; Module="ESP32-S3-WROOM-1-N8R8"; Partition="partitions/8MB.csv"; Web="capability-driven"; Tier="P0"; Notes="Recommended S3 daily development target." }
    [pscustomobject]@{ Chip="esp32s3"; FlashMB=16; PsramMB=8; Env="esp32s3-F16R8";Module="ESP32-S3-WROOM-1-N16R8";Partition="partitions/16MB.csv"; Web="capability-driven"; Tier="P0"; Notes="S3 PSRAM development and release target." }
)

function Show-Table([object[]]$rows) {
    $rows | Format-Table Chip, FlashMB, PsramMB, Env, Module, Partition, Tier -AutoSize
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
