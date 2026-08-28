# FastBee ESP32-C6 Device Diagnosis & Recovery Script
# Target: ESP32-C6 (COM11, Hostname: fastbee.local)

$ErrorActionPreference = "Continue"

Write-Output "`n=========================================" -ForegroundColor Cyan
Write-Output "  FastBee C6 Device Full Recovery" -ForegroundColor Cyan
Write-Host "=========================================\n" -ForegroundColor Cyan

# Step 1: Quick API Health Check
Write-Output "=== Step 1: API Health Verification ===" -ForegroundColor Yellow
try {
    $apiStatus = curl.exe --noproxy "*" --max-time 10 http://fastbee.local/api/system/status 2>&1
    if ($apiStatus.Length -gt 100) {
        Write-Output "✓ API responding at fastbee.local" -ForegroundColor Green
        try {
            $json = ConvertFrom-Json $apiStatus
            Write-Output "Device Type: $($json.deviceType)" -ForegroundColor White
            Write-Output "MAC Address: $($json.macAddress)" -ForegroundColor White  
            Write-Output "IP Address: $($json.ipAddress)" -ForegroundColor White
            Write-Output "WiFi Mode: $($json.mode)" -ForegroundColor White
            Write-Output "System Uptime: $($json.uptimeSec)s" -ForegroundColor White
            $deviceOnline = $true
        } catch {
            Write-Output "✗ Invalid JSON response" -ForegroundColor Red
            $deviceOnline = $false
        }
    } else {
        Write-Output "✗ Short API response" -ForegroundColor Red
        $deviceOnline = $false
    }
} catch {
    Write-Output "✗ Cannot reach device at fastbee.local: $_" -ForegroundColor Red
    $deviceOnline = $false
}

Start-Sleep -Seconds 2

# Step 2: Web Page Validation
Write-Output "`n=== Step 2: Web Page Validation ===" -ForegroundColor Yellow
try {
    $html = curl.exe --noproxy "*" --max-time 10 http://fastbee.local/ 2>&1
    if ($html.Length -gt 1000) {
        Write-Output "✓ Web page loaded: $($html.Length) bytes" -ForegroundColor Green
        if ($html -match "login-form|username|password") {
            Write-Output "✓ Login form present - Web service healthy" -ForegroundColor Green
        } else {
            Write-Output "⚠ Page exists but login form missing" -ForegroundColor Yellow
        }
    } else {
        Write-Output "✗ Page too short or empty" -ForegroundColor Red
    }
} catch {
    Write-Output "✗ Web page unreachable: $_" -ForegroundColor Red
}

Start-Sleep -Seconds 2

# Step 3: MQTT Status Check
Write-Output "`n=== Step 3: MQTT Connection Status ===" -ForegroundColor Yellow
try {
    $mqttStatus = curl.exe --noproxy "*" --max-time 10 http://fastbee.local/api/mqtt/status 2>&1
    if ($mqttStatus.Length -gt 50) {
        Write-Output "✓ MQTT status accessible" -ForegroundColor Green
        Write-Output "$mqttStatus" -ForegroundColor Gray
    } else {
        Write-Output "⚠ MQTT status endpoint returns minimal data" -ForegroundColor Yellow
    }
} catch {
    Write-Output "✗ MQTT status unavailable: $_" -ForegroundColor Red
}

Start-Sleep -Seconds 2

# Step 4: Network Configuration Check
Write-Output "`n=== Step 4: Network Configuration Analysis ===" -ForegroundColor Yellow
try {
    $netStatus = curl.exe --noproxy "*" --max-time 10 http://fastbee.local/api/network/status 2>&1
    if ($netStatus.Length -gt 100) {
        Write-Output "✓ Network status retrieved" -ForegroundColor Green
        $netJson = ConvertFrom-Json $netStatus
        Write-Output "Mode: $($netJson.mode)" -ForegroundColor White
        Write-Output "SSID: $($netJson.wifi.ssid)" -ForegroundColor White
        Write-Output "IP: $($netJson.ipAddress)" -ForegroundColor White
        Write-Output "Gateway: $($netJson.gateway)" -ForegroundColor White
    } else {
        Write-Output "⚠ Network status incomplete" -ForegroundColor Yellow
    }
} catch {
    Write-Output "✗ Network status unreachable: $_" -ForegroundColor Red
}

Start-Sleep -Seconds 2

# Step 5: Determine if Recovery Needed
Write-Output "`n=== Step 5: Recovery Decision ===" -ForegroundColor Yellow

$isRecoveredNeeded = $false
$reasons = @()

if (-not $deviceOnline) {
    $isRecoveredNeeded = $true
    $reasons += "Device not online via network"
}

if ($json.mode -like "*AP*" -and $json.mode -notlike "*STA*") {
    $isRecoveredNeeded = $true
    $reasons += "Device in AP mode only (no STA connection)"
}

if ($mqttStatus -notlike "*connected*" -and $mqttStatus -notlike "*status*") {
    $reasons += "MQTT status unclear or disconnected"
}

if ($reasons.Count -gt 0) {
    Write-Output "⚠ Recovery recommended:" -ForegroundColor Yellow
    foreach ($reason in $reasons) {
        Write-Output "  - $reason" -ForegroundColor Gray
    }
} else {
    Write-Output "✓ Device appears healthy - No recovery needed" -ForegroundColor Green
}

# Step 6: Execute Recovery if Needed
if ($isRecoveredNeeded -or $args.Contains("--force-recover")) {
    Write-Output "`n=========================================" -ForegroundColor Red
    Write-Output "  INITIATING RECOVERY PROCEDURE" -ForegroundColor Red
    Write-Host "=========================================\n" -ForegroundColor Red
    
    # Generate full filesystem image (Unified Architecture v2.0)
    Write-Output "Step 6.1: Generating clean full filesystem (Unified)..." -ForegroundColor Yellow
    Write-Output "Command: node scripts/gzip-www.js --no-upload --no-monitor" -ForegroundColor Gray
    $gzipResult = node scripts/gzip-www.js --no-upload --no-monitor 2>&1
    if ($gzipResult -match "Staging package ready") {
        Write-Output "✓ Filesystem generation complete" -ForegroundColor Green
        $stagingDir = Select-String -Input $gzipResult -Pattern "\.pio/fs-staging/[^\s]+" | ForEach-Object { $_.Matches.Value } | Select-Object -Last 1
        if ($stagingDir) {
            Write-Output "Staging directory: $stagingDir" -ForegroundColor White
            $env:PLATFORMIO_DATA_DIR = Join-Path (Get-Location) $stagingDir
            Write-Output "Set PLATFORMIO_DATA_DIR=$($env:PLATFORMIO_DATA_DIR)" -ForegroundColor White
        }
    } else {
        Write-Output "✗ Filesystem generation failed" -ForegroundColor Red
        Write-Output "$gzipResult" -ForegroundColor Gray
    }
    
    Start-Sleep -Seconds 3
    
    # Upload LittleFS to device
    Write-Output "`nStep 6.2: Uploading LittleFS to ESP32-C6..." -ForegroundColor Yellow
    Write-Output "Command: pio run -e esp32c6-F4R0 --target uploadfs" -ForegroundColor Gray
    $uploadfsResult = pio run -e esp32c6-F4R0 --target uploadfs 2>&1
    if ($uploadfsResult -match "Successfully|OK|success") {
        Write-Output "✓ LittleFS upload successful" -ForegroundColor Green
    } else {
        Write-Output "✗ LittleFS upload failed" -ForegroundColor Red
        Write-Output "$uploadfsResult" -ForegroundColor Gray
    }
    
    Start-Sleep -Seconds 3
    
    # Compile and flash firmware
    Write-Output "`nStep 6.3: Compiling and flashing firmware..." -ForegroundColor Yellow
    Write-Output "Command: pio run -e esp32c6-F4R0 --target upload" -ForegroundColor Gray
    $flashResult = pio run -e esp32c6-F4R0 --target upload 2>&1
    if ($flashResult -match "Successfully|OK|success|flashed") {
        Write-Output "✓ Firmware flash successful" -ForegroundColor Green
    } else {
        Write-Output "✗ Firmware flash failed" -ForegroundColor Red
        Write-Output "$flashResult" -ForegroundColor Gray
    }
    
    Start-Sleep -Seconds 5
    
    # Wait for device reboot
    Write-Output "`nWaiting for device to reboot (60 seconds)..." -ForegroundColor Yellow
    for ($i = 0; $i -lt 60; $i++) {
        Write-Output "`rReboot progress: $($i)/60 seconds" -NoNewline
        Start-Sleep -Seconds 1
    }
    Write-Output "`r                                             " -NoNewline
    
} else {
    Write-Output "`n✓ No recovery required - Device appears healthy" -ForegroundColor Green
}

# Step 7: Post-Recovery Verification
Write-Output "`n=========================================" -ForegroundColor Cyan
Write-Output "  POST-RECOVERY VERIFICATION" -ForegroundColor Cyan
Write-Host "=========================================\n" -ForegroundColor Cyan

Write-Output "Verifying device accessibility after recovery..." -ForegroundColor Yellow
$verified = $false
for ($retry = 0; $retry -lt 10; $retry++) {
    try {
        $checkApi = curl.exe --noproxy "*" --max-time 5 http://fastbee.local/api/system/status 2>&1
        if ($checkApi.Length -gt 100) {
            Write-Output "✓ Device is responsive after recovery!" -ForegroundColor Green
            $verified = $true
            break
        }
    } catch {}
    Write-Output "`rRetry $((`$retry + 1))/10 - Waiting for device..." -NoNewline
    Start-Sleep -Seconds 5
}
Write-Output "`r                                                   " -NoNewline

if ($verified) {
    Write-Output "`n✓ Recovery completed successfully!" -ForegroundColor Green
    Write-Output "Next: Run E2E tests to validate all functionality" -ForegroundColor Cyan
} else {
    Write-Output "⚠ Device still not responsive - Check serial logs" -ForegroundColor Yellow
    Write-Output "Serial port for C6: COM11 at 115200 baud" -ForegroundColor Gray
}

Write-Output "`n=========================================" -ForegroundColor Cyan
Write-Output "  DIAGNOSIS COMPLETE" -ForegroundColor Cyan
Write-Host "=========================================\n" -ForegroundColor Cyan
