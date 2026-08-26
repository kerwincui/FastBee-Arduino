# FastBee-Arduino Quick Deployment Verification Script
# This script uploads firmware and performs basic connectivity tests

param(
    [string[]]$Environments = @("esp32s3-F16R8", "esp32c6-F4R0", "esp32-F4R0", "esp32c3-F4R0"),
    [switch]$NoUpload,
    [switch]$VerifyOnly
)

$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlySuppress"

Write-Host "`n`n============================================================" -ForegroundColor Cyan
Write-Host "FastBee Quick Deployment Verification" -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan

$totalDevices = $Environments.Count
$processedDevices = 0
$successfulDeploys = 0

foreach ($env in $Environments) {
    $processedDevices++
    
    Write-Host "[${processedDevices}/${totalDevices}] Processing: $env" -ForegroundColor Yellow
    
    # Get environment details from platformio.ini
    switch ($env) {
        "esp32s3-F16R8" { 
            $comPort = "COM15"
            $mac = "14:c1:9f:c1:52:d8"
            $expectedIP = "192.168.5.34"
        }
        "esp32c6-F4R0" { 
            $comPort = "COM11"
            $mac = "98:a3:16:ff:fe:9b:59:dc"
            $expectedIP = "192.168.5.31"
        }
        "esp32-F4R0" { 
            $comPort = "COM16"
            $mac = $null
            $expectedIP = "192.168.5.116"
        }
        "esp32c3-F4R0" { 
            $comPort = "COM4"
            $mac = "14:63:93:90:2f:64"
            $expectedIP = "192.168.5.66"
        }
    }
    
    # Step 1: Upload LittleFS filesystem if not already done
    if (-not $NoUpload) {
        Write-Host "  [UPD] Uploading LittleFS..." -ForegroundColor Gray
        
        $fsEnv = $env.Replace("esp", "") -replace("-", "_")
        
        try {
            # Set Python encoding to avoid GBK errors
            $env:PYTHONIOENCODING = 'utf-8'
            
            # Check if we have a staging directory ready
            $stagingDirs = Get-ChildItem ".pio/fs-staging" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 3
            
            $targetStaging = $null
            foreach ($dir in $stagingDirs) {
                if ($dir.Name -match $env) {
                    $targetStaging = $dir.FullName
                    break
                }
            }
            
            if (-not $targetStaging) {
                # Use latest available staging
                $targetStaging = $stagingDirs[0].FullName
            }
            
            $env:PLATFORMIO_DATA_DIR = $targetStaging
            
            Write-Host "      Staging dir: $targetStaging" -ForegroundColor Gray
            
            # Upload filesystem
            $uploadFsResult = pio run -e $env --target uploadfs 2>&1
            
            if ($LASTEXITCODE -eq 0) {
                Write-Host "  [✓] LittleFS uploaded successfully" -ForegroundColor Green
            } else {
                Write-Host "  [✗] LittleFS upload failed" -ForegroundColor Red
                continue
            }
        } catch {
            Write-Host "  [ERROR] Filesystem upload error: $($_.Exception.Message)" -ForegroundColor Red
            continue
        }
    }
    
    # Step 2: Upload firmware
    if (-not $NoUpload -and -not $VerifyOnly) {
        Write-Host "  [UPD] Uploading firmware..." -ForegroundColor Gray
        
        try {
            $fwUploadResult = pio run -e $env --target upload 2>&1
            
            if ($LASTEXITCODE -eq 0) {
                Write-Host "  [✓] Firmware uploaded successfully" -ForegroundColor Green
                
                # Wait for device to boot
                Write-Host "  [WAIT] Waiting for device boot (15 seconds)..." -ForegroundColor Gray
                Start-Sleep -Seconds 15
                
                $successfulDeploys++
            } else {
                Write-Host "  [✗] Firmware upload failed" -ForegroundColor Red
            }
        } catch {
            Write-Host "  [ERROR] Firmware upload error: $($_.Exception.Message)" -ForegroundColor Red
        }
    } elseif ($VerifyOnly) {
        Write-Host "  [SKIP] Upload skipped (--VerifyOnly mode)" -ForegroundColor Gray
    }
    
    # Step 3: Verify device is accessible
    Write-Host "  [CHK] Verifying device accessibility at $expectedIP..." -ForegroundColor Gray
    
    try {
        $response = Invoke-WebRequest -Uri "http://$expectedIP/api/system/status" -TimeoutSec 5 -UseBasicParsing -ErrorAction Stop
        
        Write-Host "  [✓] Device responding!" -ForegroundColor Green
        Write-Host "      - Boot Ready: $($response.boot_ready)" -ForegroundColor Gray
        Write-Host "      - Uptime: $($response.uptime_seconds)s" -ForegroundColor Gray
        Write-Host "      - WiFi Mode: $($response.wifi.mode)" -ForegroundColor Gray
        Write-Host "      - WiFi IP: $($response.wifi.ip_address)" -ForegroundColor Gray
        
        if ($response.mdns) {
            Write-Host "      - mDNS Hostname: $($response.mdns.hostname)" -ForegroundColor Gray
            Write-Host "      - mDNS Available: $($response.mdns.available)" -ForegroundColor Gray
        }
        
        # Test mDNS resolution
        if ($response.mdns -and $response.mdns.hostname) {
            Write-Host "  [MDNS] Testing DNS resolution..." -ForegroundColor Gray
            
            try {
                $ipAddress = [System.Net.Dns]::GetHostEntry($response.mdns.hostname + ".local").AddressList[0].ToString()
                Write-Host "  [✓] mDNS '$($response.mdns.hostname).local' → $ipAddress" -ForegroundColor Green
            } catch {
                Write-Host "  [✗] mDNS resolution failed" -ForegroundColor Red
            }
        }
        
    } catch {
        Write-Host "  [✗] Device not accessible at $expectedIP" -ForegroundColor Red
        
        # Try to discover via network scan
        Write-Host "  [DISC] Attempting network discovery..." -ForegroundColor Yellow
        
        for ($i = 1; $i -le 50; $i++) {
            $testIP = "192.168.5.$i"
            try {
                $testResp = Invoke-WebRequest -Uri "http://$testIP/api/system/status" -TimeoutSec 2 -UseBasicParsing -ErrorAction Stop
                Write-Host "  [✓] Found device at $testIP" -ForegroundColor Green
                $expectedIP = $testIP
                break
            } catch {}
        }
    }
    
    Write-Host ""
}

# Final Summary
Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "Deployment Summary" -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan

Write-Host "Total Devices: $totalDevices" -ForegroundColor Gray
Write-Host "Successfully Deployed: $successfulDeploys/$totalDevices" -ForegroundColor $(if($successfulDeploys -eq $totalDevices){"Green"}else{"Yellow"})

if ($successfulDeploys -gt 0) {
    Write-Host "`nNext Steps:" -ForegroundColor Cyan
    Write-Host "  1. Run quick health checks:" -ForegroundColor White
    Write-Host "     .\.scripts\run-fleet-tests.ps1 -DeviceIP 192.168.5.34`n"
    
    Write-Host "  2. Run full E2E test suite:" -ForegroundColor White
    Write-Host "     npm run test:e2e`n"
    
    Write-Host "  3. Start 24-hour stability monitoring:" -ForegroundColor White
    Write-Host "     .\.scripts\fastbee-fleet-deploy.ps1 -StabilityTest -StabilityDurationHours 24`n"
}

exit 0
