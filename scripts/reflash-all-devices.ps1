# Complete Fleet Deployment Script - Factory.bin Flash for All Devices

$ErrorActionPreference = "Continue"
$logDir = ".pio\puzhong"
$startTime = Get-Date

Write-Host "`n=============================================" -ForegroundColor Cyan
Write-Output "  COMPLETE FLASHING OF ALL 4 DEVICES" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan

# Device configuration
$devices = @(
    @{Name='C3'; Port='COM13'; Env='esp32c3-F4R0'; FWPath='dist\firmware\esp32c3-F4R0\factory.bin'; ChipParam='--flash-mode dio --flash-size 4MB --flash-freq 40m'},
    @{Name='Classic'; Port='COM16'; Env='esp32-F4R0'; FWPath='dist\firmware\esp32-F4R0\factory.bin'; ChipParam=''},
    @{Name='S3'; Port='COM15'; Env='esp32s3-F16R8'; FWPath='dist\firmware\esp32s3-F16R8\factory.bin'; ChipParam=''},
    @{Name='C6'; Port='COM11'; Env='esp32c6-F4R0'; FWPath='dist\firmware\esp32c6-F4R0\factory.bin'; ChipParam=''}
)

$successCount = 0
$failedDevices = @()

foreach ($dev in $devices) {
    Write-Host "`n[*] Flashing $($dev.Name) device (Port: $($dev.Port))..." -ForegroundColor Yellow
    
    # Check if firmware file exists
    if (-not (Test-Path $dev.FWPath)) {
        Write-Host "  ✗ Firmware NOT found at $($dev.FWPath)" -ForegroundColor Red
        $failedDevices += $dev.Name
        continue
    }
    
    Write-Host "  ✓ Firmware file: $($dev.FWPath)" -ForegroundColor Green
    Write-Host "  Size: $([math]::Round((Get-Item $dev.FWPath).Length/1MB,2)) MB" -ForegroundColor Gray
    
    try {
        # Erase flash first
        Write-Host "  Erasing flash..." -ForegroundColor Gray
        & python esptool.py --port $dev.Port erase_flash 2>$null | Out-Null
        
        # Build flash command based on chip type
        if ($dev.Name -eq 'C3') {
            # C3 needs special parameters
            Write-Host "  Flashing with C3-specific params..." -ForegroundColor Gray
            & python esptool.py --port $dev.Port write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m 0x0 $dev.FWPath 2>&1 | Tee-Object "$logDir\$($dev.Name)-COMPLETE-FLASH.log" | ForEach-Object { if($_) { Write-Host "  $_" } }
        } else {
            # Standard parameters for other devices
            Write-Host "  Flashing standard factory.bin..." -ForegroundColor Gray
            & python esptool.py --port $dev.Port write-flash 0x0 $dev.FWPath 2>&1 | Tee-Object "$logDir\$($dev.Name)-COMPLETE-FLASH.log" | ForEach-Object { if($_) { Write-Host "  $_" } }
        }
        
        Write-Host "  ✓ $($dev.Name) flash completed successfully!" -ForegroundColor Green
        $successCount++
        
    } catch {
        Write-Host "  ✗ Failed to flash $($dev.Name): $_" -ForegroundColor Red
        $failedDevices += $dev.Name
    }
    
    Start-Sleep -Seconds 3
}

$endTime = Get-Date
$totalDuration = New-TimeSpan -Start $startTime -End $endTime

Write-Host "`n=============================================" -ForegroundColor Cyan
Write-Host "  FLASHING SUMMARY" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "Total devices: $($devices.Count)" -ForegroundColor White
Write-Host "Successful: $successCount" -ForegroundColor Green
if ($failedDevices.Count -gt 0) {
    Write-Host "Failed: $($failedDevices.Count) - $($failedDevices -join ', ')' -ForegroundColor Red
}
Write-Host "Duration: $($totalDuration.Minutes)m $($totalDuration.Seconds)s" -ForegroundColor Yellow

# Wait for all devices to boot
Write-Host "`nWaiting 90 seconds for all devices to boot and connect to WiFi..." -ForegroundColor Cyan
Start-Sleep -Seconds 90

# Network discovery
Write-Host "`n=== Network Discovery ===" -ForegroundColor Cyan
$onlineDevices = @()
for ($i = 2; $i -le 60; $i++) {
    $ip = "192.168.5.$i"
    try {
        $api = curl.exe --noproxy "*" --max-time 1 http://$ip/api/system/status 2>$null
        if ($api -and $api.Length -gt 200) {
            $json = ConvertFrom-Json $api 2>$null
            if ($json) {
                $type = $json.deviceType
                $mac = $json.macAddress
                $color = "White"
                if ($type -like "*C3*") { $color = "Green" }
                elseif ($type -like "*C6*") { $color = "Magenta" }
                elseif ($type -like "*S3*") { $color = "Yellow" }
                
                Write-Host "Found: $ip -> [$type] MAC: $mac" -ForegroundColor $color
                $onlineDevices += [PSCustomObject]@{IP=$ip;Type=$type;MAC=$mac}
            }
        }
    } catch {}
    Start-Sleep-Milliseconds 100
}

Write-Host "`n=== Final Summary ===" -ForegroundColor Cyan
Write-Host "Devices flashed successfully: $successCount/$($devices.Count)" -ForegroundColor White
Write-Host "Devices found online: $($onlineDevices.Count)" -ForegroundColor $(if($onlineDevices.Count-gt0){"Green"}else{"Yellow"})

if ($onlineDevices.Count -gt 0) {
    Write-Host "`n✓ Partial or full success! Some devices are online." -ForegroundColor Green
    Write-Host "Proceeding to Playwright E2E tests..." -ForegroundColor Yellow
} else {
    Write-Host "`n✗ No devices found online after flashing." -ForegroundColor Red
    Write-Host "Possible reasons:" -ForegroundColor Gray
    Write-Host "  - WiFi connection failed (SSID/password mismatch)"
    Write-Host "  - HTTP service not started properly"
    Write-Host "  - Serial console needed to check boot logs"
    Write-Host "`nRecommendation: Check router DHCP list for connected devices"
}
