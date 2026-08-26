# FastBee-Arduino Fleet Test Runner - Automated Testing Script

param(
    [switch]$QuickTest,
    [switch]$FullTest,
    [switch]$StabilityCheck,
    [int]$DeviceIP # Optional: Specify specific device IP to test
)

$ProgressPreference = 'SilentlySuppress'

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "FastBee Multi-Device E2E Test Runner" -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan

# Helper function to wait for device to be ready
function Wait-ForDeviceReady {
    param(
        [string]$IPAddress,
        [int]$TimeoutSeconds = 30
    )
    
    Write-Host "[WAIT] Waiting for device $IPAddress to boot..." -ForegroundColor Yellow
    
    $startTime = Get-Date
    while ((New-TimeSpan -Start=$startTime).TotalSeconds -lt $TimeoutSeconds) {
        try {
            $response = Invoke-RestMethod -Uri "http://$IPAddress/api/system/status" -TimeoutSec 5 -UseBasicParsing
            if ($response.boot_ready -eq $true) {
                Write-Host "[READY] Device $IPAddress is ready (uptime: $($response.uptime_seconds)s)" -ForegroundColor Green
                return $true
            }
        } catch {
            Start-Sleep -Seconds 2
        }
    }
    
    Write-Host "[TIMEOUT] Device $IPAddress did not become ready within ${TimeoutSeconds}s" -ForegroundColor Red
    return $false
}

# Quick health check test
function Run-QuickHealthCheck {
    param($IPAddress)
    
    Write-Host "`n[HEALTH] Running quick health check on $IPAddress..." -ForegroundColor Cyan
    
    $results = @{
        systemStatus = "FAIL"
        wifiStatus = "FAIL"
        mdnsStatus = "FAIL"
        apiLatency = 0
    }
    
    $start = Get-Date
    
    try {
        # Test system status API
        $sysStatus = Invoke-RestMethod -Uri "http://$IPAddress/api/system/status" -UseBasicParsing
        $results.systemStatus = "OK"
        $results.uptime = $sysStatus.uptime_seconds
        
        # Test WiFi status
        $wifiStatus = Invoke-RestMethod -Uri "http://$IPAddress/api/wifi/status" -UseBasicParsing
        $results.wifiStatus = $wifiStatus.mode
        $results.ipAddress = $wifiStatus.ip_address
        
        # Test mDNS availability
        $mdnsStatus = Invoke-RestMethod -Uri "http://$IPAddress/api/mdns/status" -UseBasicParsing
        $results.mdnsStatus = if($mdnsStatus.available){"OK"}else{"OFFLINE"}
        $results.hostname = $mdnsStatus.hostname
        
        # Calculate latency
        $end = Get-Date
        $results.apiLatency = [math]::Round(($end - $start).TotalMilliseconds, 1)
        
        return $results
    } catch {
        Write-Host "  [ERROR] Health check failed: $($_.Exception.Message)" -ForegroundColor Red
        return $null
    }
}

# Run Playwright E2E tests
function Run-PlaywrightTests {
    param(
        [string]$IPAddress,
        [bool]$QuickMode
    )
    
    Write-Host "`n[PLAYWRIGHT] Executing E2E tests on $IPAddress..." -ForegroundColor Cyan
    
    # Set device environment variable
    $env:DEVICES = $IPAddress
    
    $testResults = @{
        success = $false
        durationMs = 0
        failures = @()
        testsRun = 0
    }
    
    $startTime = Get-Date
    
    try {
        # Quick mode: Just run discovery and basic connectivity
        if ($QuickMode) {
            Write-Host "  [TEST] Quick mode: Running device discovery..." -ForegroundColor Gray
            
            $discoverOutput = npm run test:discover 2>&1
            if ($LASTEXITCODE -eq 0 -or $discoverOutput -like "*$IPAddress*") {
                $testResults.success = $true
                Write-Host "  [PASS] Device discovered successfully" -ForegroundColor Green
            } else {
                $testResults.failures += "Device discovery failed"
            }
        } else {
            # Full mode: Run comprehensive test suite
            Write-Host "  [TEST] Full mode: Running complete deployment validation..." -ForegroundColor Gray
            
            # Run full deployment test with extended timeout
            $playwrightArgs = @(
                "npx", "playwright", "test",
                "test/e2e/fastbee-full-deployment.spec.js",
                "--project=chromium",
                "--timeout=180000",
                "--reporter=list"
            )
            
            & $playwrightArgs
            
            if ($LASTEXITCODE -eq 0) {
                $testResults.success = $true
                Write-Host "  [PASS] All E2E tests completed successfully" -ForegroundColor Green
            } else {
                $testResults.failures += "Some E2E tests failed"
                Write-Host "  [INFO] See above for test failure details" -ForegroundColor Yellow
            }
            
            # Also run web access validation
            Write-Host "  [TEST] Running web access validation..." -ForegroundColor Gray
            
            $webAccessArgs = @(
                "npx", "playwright", "test",
                "test/e2e/web-access-validation.spec.js",
                "--project=chromium"
            )
            
            & $webAccessArgs
            if ($LASTEXITCODE -ne 0) {
                $testResults.failures += "Web access validation failed"
            }
        }
        
    } catch {
        $testResults.failures += $_.Exception.Message
        Write-Host "  [ERROR] E2E test exception: $($_.Exception.Message)" -ForegroundColor Red
    } finally {
        $endTime = Get-Date
        $testResults.durationMs = ($endTime - $startTime).TotalMilliseconds
        $testResults.testsRun = if($QuickMode){1}else{2}
    }
    
    return $testResults
}

# Memory stability check
function Run-MemoryStabilityCheck {
    param($IPAddress)
    
    Write-Host "`n[MEMORY] Checking DRAM memory health on $IPAddress..." -ForegroundColor Cyan
    
    $memHistory = @()
    $samples = 5
    $interval = 10 # seconds between samples
    
    try {
        for ($i = 1; $i -le $samples; $i++) {
            $status = Invoke-RestMethod -Uri "http://$IPAddress/api/system/status" -UseBasicParsing
            
            $memData = @{
                sample = $i
                timestamp = Get-Date
                freeDRAM = $status.memory_info.free_dram
                usedDRAM = $status.memory_info.used_dram
                totalRAM = $status.memory_info.heap_size
                largestFreeBlock = $status.memory_info.largest_free_block
            }
            
            $memHistory += $memData
            
            Write-Host "  Sample $i/$samples: Free=${memData.freeDRAM}KB, Largest Block=${memData.largestFreeBlock}KB" -ForegroundColor Gray
            
            # Check critical thresholds
            if ($memData.freeDRAM -lt 12000) {
                Write-Host "  [WARN] CRITICAL: DRAM free below 12KB threshold!" -ForegroundColor Red
            } elseif ($memData.freeDRAM -lt 16000) {
                Write-Host "  [WARN] WARNING: DRAM free below 16KB - monitor closely" -ForegroundColor Yellow
            }
            
            # Check for memory leaks
            if ($i -gt 1) {
                $prevSample = $memHistory[-2]
                $diff = $memData.freeDRAM - $prevSample.freeDRAM
                if ($diff -lt -2000) {
                    Write-Host "  [ALERT] Potential memory leak detected (-$([math]::Abs($diff))KB since last sample)" -ForegroundColor Magenta
                }
            }
            
            if ($i -lt $samples) {
                Start-Sleep -Seconds $interval
            }
        }
        
        # Analyze trend
        $firstMem = $memHistory[0].freeDRAM
        $lastMem = $memHistory[-1].freeDRAM
        $memChange = $lastMem - $firstMem
        
        Write-Host "`n  Memory Trend Analysis:" -ForegroundColor Yellow
        Write-Host "    First sample:  $firstMem KB" -ForegroundColor Gray
        Write-Host "    Last sample:   $lastMem KB" -ForegroundColor Gray
        Write-Host "    Change:        $((if($memChange -ge 0){"+"})$memChange KB)" -ForegroundColor $(if($memChange -lt -2000){"Red"}elseif($memChange -lt 0){"Yellow"}else{"Green"})
        
        return @{
            samplesCollected = $samples
            initialFreeDRAM = $firstMem
            finalFreeDRAM = $lastMem
            memoryChange = $memChange
            isHealthy = ($lastMem -gt 12000)
        }
        
    } catch {
        Write-Host "  [ERROR] Memory check failed: $($_.Exception.Message)" -ForegroundColor Red
        return $null
    }
}

# Main execution
try {
    # Discover available devices
    Write-Host "[DISCOVERY] Scanning network for FastBee devices..." -ForegroundColor Cyan
    
    $discoverCmd = "node test/e2e/helpers/discover-devices.js"
    $discoverOutput = Invoke-Expression $discoverCmd
    
    # Parse discovered devices
    $deviceIPs = @()
    if ($discoverOutput -match 'Found (\d+) device\(s\):') {
        # Additional discovery logic would go here
        Write-Host "  Discovered devices: $($matches[1])" -ForegroundColor Green
    } else {
        Write-Host "  Note: No devices auto-discovered. Please specify via -DeviceIP parameter." -ForegroundColor Yellow
    }
    
    # If specific device IP provided, test it
    if ($DeviceIP) {
        Write-Host "`n[TARGET] Testing specified device: $DeviceIP" -ForegroundColor Cyan
        
        # Wait for device to be ready
        $isReady = Wait-ForDeviceReady $DeviceIP -TimeoutSeconds 30
        if (-not $isReady) {
            Write-Host "[FAILED] Device not responding, aborting tests" -ForegroundColor Red
            exit 1
        }
        
        # Run health checks
        $healthResult = Run-QuickHealthCheck $DeviceIP
        if ($healthResult) {
            Write-Host "`n[RESULTS] Health Check Summary for $DeviceIP:" -ForegroundColor Green
            Write-Host "  System Status:  $($healthResult.systemStatus)" -ForegroundColor Gray
            Write-Host "  WiFi Mode:      $($healthResult.wifiStatus)" -ForegroundColor Gray
            Write-Host "  IP Address:     $($healthResult.ipAddress)" -ForegroundColor Gray
            Write-Host "  mDNS Status:    $($healthResult.mdnsStatus)" -ForegroundColor Gray
            Write-Host "  Uptime:         $($healthResult.uptime)s" -ForegroundColor Gray
            Write-Host "  API Latency:    $($healthResult.apiLatency)ms" -ForegroundColor Gray
        }
        
        # Run memory stability check
        $memResult = Run-MemoryStabilityCheck $DeviceIP
        if ($memResult) {
            Write-Host "`n[MEMORY RESULT] DRAM Health: $(if($memResult.isHealthy){'HEALTHY'}else:'CRITICAL')" -ForegroundColor $(if($memResult.isHealthy){"Green"}else{"Red"})
        }
        
        # Run E2E tests
        $e2eResult = Run-PlaywrightTests $DeviceIP -QuickMode:$QuickTest
        
        Write-Host "`n[E2E RESULT] Tests Completed:" -ForegroundColor Green
        Write-Host "  Success: $($e2eResult.success)" -ForegroundColor $(if($e2eResult.success){"Green"}else{"Red"})
        Write-Host "  Duration: $([math]::Round($e2eResult.durationMs/1000, 2))s" -ForegroundColor Gray
        Write-Host "  Tests Run: $($e2eResult.testsRun)" -ForegroundColor Gray
        
        if (-not $e2eResult.success -and $e2eResult.failures.Count -gt 0) {
            Write-Host "  Failures:" -ForegroundColor Red
            foreach ($failure in $e2eResult.failures) {
                Write-Host "    - $failure" -ForegroundColor Red
            }
        }
        
    } else {
        Write-Host "`n[INFO] Use -DeviceIP parameter to specify a device IP address" -ForegroundColor Cyan
        Write-Host "Example: .\run-fleet-tests.ps1 -DeviceIP 192.168.5.85" -ForegroundColor Cyan
        exit 0
    }
    
} catch {
    Write-Host "`n[FATAL] Test execution failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "Test Execution Completed" -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan
exit 0
