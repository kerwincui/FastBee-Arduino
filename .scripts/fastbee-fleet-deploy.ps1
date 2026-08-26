# FastBee-Arduino Multi-Device Fleet Deployment and Stability Testing Script
# This script deploys firmware to multiple ESP32 devices and runs comprehensive tests

param(
    [switch]$DeployOnly,
    [switch]$StabilityTest,
    [int]$StabilityDurationHours = 24,
    [switch]$RunE2ETest,
    [switch]$ReportOnly
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlySuppress"

# Configuration
$Devices = @(
    @{Name="ESP32-S3-F16R8"; Env="esp32s3-F16R8"; COMPort="COM15"; MAC="14:c1:9f:c1:52:d8"},
    @{Name="ESP32-C3-F4R0"; Env="esp32c3-F4R0"; COMPort="COM4"; MAC="14:63:93:90:2f:64"},
    @{Name="ESP32-C6-F4R0"; Env="esp32c6-F4R0"; COMPort="COM11"; MAC="98:a3:16:ff:fe:9b:59:dc"},
    @{Name="ESP32-F4R0"; Env="esp32-F4R0"; COMPort="COM16"; MAC=$null} # To be discovered
)

$TestResults = @{}
$StartTime = Get-Date

Write-Host "=" * 80 -ForegroundColor Cyan
Write-Host "FastBee-Arduino Multi-Device Fleet Deployment & Stability Test" -ForegroundColor Cyan
Write-Host "=" * 80 -ForegroundColor Cyan
Write-Host "Started at: $($StartTime.ToString('yyyy-MM-dd HH:mm:ss'))" -ForegroundColor White
Write-Host ""

# Function to deploy firmware to a single device
function Deploy-Device {
    param($Device)
    
    Write-Host "[DEPLOY] Deploying $($Device.Name) on $($Device.COMPort)..." -ForegroundColor Yellow
    
    # Step 1: Generate web assets for this environment
    Write-Host "  [INFO] Generating web assets..." -ForegroundColor Gray
    $env:PYTHONIOENCODING = 'utf-8'
    
    # Step 2: Upload LittleFS filesystem
    Write-Host "  [INFO] Uploading LittleFS filesystem..." -ForegroundColor Gray
    $fsResult = pio run -e $Device.Env --target uploadfs -v 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [ERROR] Filesystem upload failed!" -ForegroundColor Red
        return $false
    }
    
    # Step 3: Upload firmware
    Write-Host "  [INFO] Uploading firmware..." -ForegroundColor Gray
    $fwResult = pio run -e $Device.Env --target upload -v 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [ERROR] Firmware upload failed!" -ForegroundColor Red
        return $false
    }
    
    # Step 4: Wait for device to boot
    Write-Host "  [INFO] Waiting for device to boot (15 seconds)..." -ForegroundColor Gray
    Start-Sleep -Seconds 15
    
    Write-Host "  [SUCCESS] Device deployed successfully!" -ForegroundColor Green
    return $true
}

# Function to verify device is accessible
function Test-DeviceAccess {
    param($IPAddress)
    
    try {
        $response = Invoke-WebRequest -Uri "http://$IPAddress/api/system/status" -TimeoutSec 10 -UseBasicParsing
        return $response.StatusCode -eq 200
    } catch {
        return $false
    }
}

# Function to collect device metrics
function Collect-DeviceMetrics {
    param($IPAddress)
    
    $metrics = @{
        timestamp = Get-Date
        ip = $IPAddress
    }
    
    try {
        $status = Invoke-RestMethod -Uri "http://$IPAddress/api/system/status" -UseBasicParsing
        $metrics.memoryFree = $status.memory_info.free_dram
        $metrics.memoryUsed = $status.memory_info.used_dram
        $metrics.heapSize = $status.memory_info.heap_size
        $metrics.uptime = $status.uptime_seconds
        $metrics.wifiMode = $status.wifi.mode
        $metrics.wifiIP = $status.wifi.ip_address
        $metrics.mdnsAvailable = $status.mdns.available
        $metrics.mdnsHostname = $status.mdns.hostname
        
        # Check MQTT status if available
        if ($status.mqtt) {
            $metrics.mqttConnected = $status.mqtt.connected
            $metrics.mqttProtocol = $status.mqtt.protocol
        }
        
        return $metrics
    } catch {
        Write-Host "  [WARN] Failed to collect metrics from $IPAddress" -ForegroundColor Yellow
        return $null
    }
}

# Function to run Playwright E2E test on a single device
function Run-E2ETest {
    param($IPAddress, $DeviceName)
    
    Write-Host "[E2E] Running Playwright tests on $DeviceName ($IPAddress)..." -ForegroundColor Cyan
    
    $testResult = @{
        passed = $false
        durationMs = 0
        failures = @()
    }
    
    $startTime = Get-Date
    
    try {
        # Set device IP for Playwright
        $env:DEVICES = $IPAddress
        
        # Run discovery first
        Write-Host "  [INFO] Discovering device..." -ForegroundColor Gray
        npm run test:discover 2>&1 | Out-Null
        
        # Run full deployment test
        Write-Host "  [INFO] Executing full deployment validation..." -ForegroundColor Gray
        $playwrightResult = npx playwright test test/e2e/fastbee-full-deployment.spec.js `
            --project=chromium `
            --timeout=120000 `
            2>&1
        
        $endTime = Get-Date
        $duration = ($endTime - $startTime).TotalMilliseconds
        $testResult.durationMs = $duration
        
        if ($LASTEXITCODE -eq 0) {
            $testResult.passed = $true
            Write-Host "  [SUCCESS] E2E tests completed in $([math]::Round($duration/1000, 2))s" -ForegroundColor Green
        } else {
            $testResult.failures += "Playwright execution failed"
            Write-Host "  [FAILED] E2E tests failed" -ForegroundColor Red
        }
    } catch {
        $testResult.failures += $_.Exception.Message
        Write-Host "  [ERROR] E2E test exception: $($_.Exception.Message)" -ForegroundColor Red
    }
    
    return $testResult
}

# Main deployment process
try {
    foreach ($Device in $Devices) {
        Write-Host ""
        Write-Host "========================================" -ForegroundColor Cyan
        Write-Host "Processing: $($Device.Name)" -ForegroundColor Cyan
        Write-Host "Environment: $($Device.Env)" -ForegroundColor Cyan
        Write-Host "COM Port: $($Device.COMPort)" -ForegroundColor Cyan
        Write-Host "========================================" -ForegroundColor Cyan
        
        # Deploy device
        $deploySuccess = Deploy-Device $Device
        $Device.DeploySuccess = $deploySuccess
        $TestResults[$Device.Name] = @{
            deploySuccess = $deploySuccess
            ipAddress = $null
            metricsHistory = @()
            e2eTests = @()
        }
        
        if ($deploySuccess) {
            # Try to discover device IP via mDNS or network scan
            Write-Host ""
            Write-Host "[DISCOVERY] Attempting to discover device IP..." -ForegroundColor Yellow
            
            # Simple network scan approach (assuming 192.168.5.x subnet)
            $deviceIP = $null
            for ($i = 1; $i -le 100; $i++) {
                $testIP = "192.168.5.$i"
                if (Test-DeviceAccess $testIP) {
                    $deviceIP = $testIP
                    Write-Host "[FOUND] Device $Device.Name detected at $testIP" -ForegroundColor Green
                    break
                }
            }
            
            if (-not $deviceIP) {
                Write-Host "[WARN] Could not auto-discover device IP" -ForegroundColor Yellow
            }
            
            $TestResults[$Device.Name].ipAddress = $deviceIP
            
            # Run E2E tests if requested
            if ($RunE2ETest -and $deviceIP) {
                Write-Host ""
                Write-Host "[TEST] Running E2E test suite..." -ForegroundColor Cyan
                $e2eResult = Run-E2ETest $deviceIP $Device.Name
                $TestResults[$Device.Name].e2eTests += $e2eResult
                
                if ($e2eResult.passed) {
                    Write-Host "[SUCCESS] E2E Tests Passed" -ForegroundColor Green
                } else {
                    Write-Host "[FAILURE] E2E Tests Failed:" -ForegroundColor Red
                    foreach ($failure in $e2eResult.failures) {
                        Write-Host "  - $failure" -ForegroundColor Red
                    }
                }
            }
        }
    }
    
    # Collect initial metrics
    Write-Host ""
    Write-Host "[METRICS] Collecting initial device metrics..." -ForegroundColor Cyan
    
    foreach ($Device in $Devices) {
        if ($TestResults[$Device.Name].deploySuccess -and $TestResults[$Device.Name].ipAddress) {
            $metrics = Collect-DeviceMetrics $TestResults[$Device.Name].ipAddress
            if ($metrics) {
                $TestResults[$Device.Name].metricsHistory += $metrics
                Write-Host "  $($Device.Name): MEM_FREE=$($metrics.memoryFree)KB, UPTIME=$($metrics.uptime)s" -ForegroundColor Gray
            }
        }
    }
    
    # Stability monitoring (if requested)
    if ($StabilityTest -and -not $DeployOnly) {
        Write-Host ""
        Write-Host "[STABILITY] Starting $StabilityDurationHours-hour stability monitoring..." -ForegroundColor Cyan
        
        $monitoringIntervalMinutes = 5
        $totalIterations = $StabilityDurationHours * 60 / $monitoringIntervalMinutes
        $currentIteration = 0
        
        while ($currentIteration -lt $totalIterations) {
            $currentIteration++
            $elapsedTime = ([DateTime]::Now - $StartTime).TotalHours
            
            Write-Host ""
            Write-Host "[$([math]::Round($elapsedTime, 1))h/$StabilityDurationHoursh] Iteration $currentIteration/$totalIterations - Collecting metrics..." -ForegroundColor Yellow
            
            foreach ($Device in $Devices) {
                if ($TestResults[$Device.Name].deploySuccess -and $TestResults[$Device.Name].ipAddress) {
                    $metrics = Collect-DeviceMetrics $TestResults[$Device.Name].ipAddress
                    if ($metrics) {
                        $TestResults[$Device.Name].metricsHistory += $metrics
                        
                        # Monitor memory health
                        if ($metrics.memoryFree -lt 12000) {
                            Write-Host "  [WARN] $($Device.Name) DRAM free block critically low: $($metrics.memoryFree)KB" -ForegroundColor Red
                        }
                        
                        # Monitor WiFi connectivity
                        if (-not $metrics.wifiIP) {
                            Write-Host "  [WARN] $($Device.Name) lost WiFi connection" -ForegroundColor Red
                        }
                    }
                }
            }
            
            # Log summary every hour
            if ($currentIteration % (60 / $monitoringIntervalMinutes) -eq 0) {
                Write-Host ""
                Write-Host "=== Hourly Summary ===" -ForegroundColor Cyan
                foreach ($Device in $Devices) {
                    if ($TestResults[$Device.Name].deploySuccess) {
                        $history = $TestResults[$Device.Name].metricsHistory
                        if ($history.Count -gt 0) {
                            $latest = $history[-1]
                            $avgFree = ($history | Measure-Object memoryFree -Average).Average
                            Write-Host "  $($Device.Name): Avg MEM_FREE=$([math]::Round($avgFree, 0))KB, Samples=$($history.Count)" -ForegroundColor Gray
                        }
                    }
                }
            }
            
            Start-Sleep -Minutes $monitoringIntervalMinutes
        }
    }
    
    # Generate final report
    Write-Host ""
    Write-Host "=" * 80 -ForegroundColor Cyan
    Write-Host "FINAL DEPLOYMENT REPORT" -ForegroundColor Cyan
    Write-Host "=" * 80 -ForegroundColor Cyan
    
    foreach ($Device in $Devices) {
        Write-Host ""
        Write-Host "--- $($Device.Name) ---" -ForegroundColor Yellow
        
        if ($TestResults[$Device.Name].deploySuccess) {
            Write-Host "  Deployment Status: SUCCESS" -ForegroundColor Green
            Write-Host "  IP Address: $($TestResults[$Device.Name].ipAddress)" -ForegroundColor Gray
            
            if ($TestResults[$Device.Name].e2eTests.Count -gt 0) {
                $e2e = $TestResults[$Device.Name].e2eTests[0]
                Write-Host "  E2E Tests: $(if($e2e.passed){'PASSED'}else{'FAILED'})" -ForegroundColor $(if($e2e.passed){"Green"}else{"Red"})
                Write-Host "  Duration: $([math]::Round($e2e.durationMs/1000, 2))s" -ForegroundColor Gray
            }
            
            if ($TestResults[$Device.Name].metricsHistory.Count -gt 0) {
                $history = $TestResults[$Device.Name].metricsHistory
                $latest = $history[-1]
                $maxUptime = ($history | Measure-Object uptime -Maximum).Maximum
                $minMemFree = ($history | Measure-Object memoryFree -Minimum).Minimum
                
                Write-Host "  Metrics Collected: $($history.Count)" -ForegroundColor Gray
                Write-Host "  Max Uptime: $([math]::Round($maxUptime/3600, 2)) hours" -ForegroundColor Gray
                Write-Host "  Min DRAM Free: $($minMemFree)KB" -ForegroundColor $(if($minMemFree -lt 12000){"Red"}else{"Green"})
                
                # Memory trend analysis
                if ($history.Count -ge 2) {
                    $first = $history[0].memoryFree
                    $last = $history[-1].memoryFree
                    $memChange = $last - $first
                    
                    if ($memChange -lt -1000) {
                        Write-Host "  Memory Trend: DETERIORATING (-$([math]::Abs($memChange))KB)" -ForegroundColor Red
                    } elseif ($memChange -gt 1000) {
                        Write-Host "  Memory Trend: IMPROVING (+$memChange KB)" -ForegroundColor Green
                    } else {
                        Write-Host "  Memory Trend: STABLE" -ForegroundColor Gray
                    }
                }
            }
        } else {
            Write-Host "  Deployment Status: FAILED" -ForegroundColor Red
        }
    }
    
    $endTime = Get-Date
    $totalDuration = ([DateTime]::Now - $StartTime).TotalSeconds
    
    Write-Host ""
    Write-Host "Deployment Summary" -ForegroundColor Cyan
    Write-Host "  Total Duration: $([math]::Round($totalDuration/60, 2)) minutes" -ForegroundColor Gray
    Write-Host "  Devices Deployed: $($Devices | Where-Object {$TestResults[$_.Name].deploySuccess}).Count / $($Devices.Count)" -ForegroundColor Gray
    
    # Save report to file
    $reportFile = ".pio/fleet-deployment-report-$(Get-Date -Format 'yyyyMMdd-HHmmss').json"
    $reportData = @{
        timestamp = $StartTime.ToString('ISO8601')
        durationSeconds = $totalDuration
        devices = $TestResults
    }
    
    $reportData | ConvertTo-Json -Depth 10 | Out-File $reportFile -Encoding utf8
    Write-Host "Report saved to: $reportFile" -ForegroundColor Cyan
    
} catch {
    Write-Host ""
    Write-Host "[FATAL] Deployment failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Deployment completed!" -ForegroundColor Green
exit 0
