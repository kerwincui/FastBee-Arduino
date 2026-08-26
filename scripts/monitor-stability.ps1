# FastBee-ESP32 7x24 小时稳定性监测脚本

$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

# Configuration
$MONITOR_DURATION_HOURS = 24  # Change to run longer tests
$CHECK_INTERVAL_MINUTES = 5   # Polling frequency
$START_TIME = Get-Date

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "FastBee-ESP32 Fleet Stability Monitor (7x24)" -ForegroundColor Cyan  
Write-Host "Duration: $MONITOR_DURATION_HOURS hours" -ForegroundColor Yellow
Write-Host "Check Interval: $CHECK_INTERVAL_MINUTES minutes" -ForegroundColor Yellow
Write-Host "Start Time: $($START_TIME.ToString('yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Yellow
Write-Host "=========================================" -ForegroundColor Cyan

# Initialize results storage
$results = @()

function Test-DeviceHealth {
    param(
        [string]$Name,
        [string]$IP,
        [string]$SerialPort
    )
    
    $status = @{
        Name = $Name
        IP = $IP
        SerialPort = $SerialPort
        Timestamp = Get-Date
    }
    
    # Test 1: Web access latency
    try {
        $webStart = Get-Date
        $webResult = Test-NetConnection -ComputerName $IP -Port 80 -InformationLevel Quiet -ErrorAction SilentlyContinue
        $latency = ((Get-Date) - $webStart).TotalMilliseconds
        
        if ($webResult) {
            $status.WebStatus = "OK"
            $status.WebLatencyMs = [math]::Round($latency, 2)
        } else {
            $status.WebStatus = "FAIL"
            $status.WebLatencyMs = $null
        }
    } catch {
        $status.WebStatus = "ERROR"
    }
    
    # Test 2: MQTT connectivity (check if topic response available)
    try {
        $mqttResponse = Invoke-RestMethod -Uri "http://$IP/api/mqtt/status" -TimeoutSec 3 -ErrorAction SilentlyContinue
        if ($mqttResponse.connected) {
            $status.MQTTStatus = "CONNECTED"
            $status.MQTTLatencyMs = [math]::Round($mqttResponse.latencyMs, 2)
        } else {
            $status.MQTTStatus = "DISCONNECTED"
        }
    } catch {
        $status.MQTTStatus = "ERROR"
    }
    
    # Test 3: FreeHeap monitoring
    try {
        $heapData = Invoke-RestMethod -Uri "http://$IP/api/system/memory" -TimeoutSec 3 -ErrorAction SilentlyContinue
        $status.FreeHeapKB = [math]::Round($heapData.freeHeapKB, 2)
        
        if ($status.FreeHeapKB -gt 100) {
            $status.HeapStatus = "HEALTHY"
        } else {
            $status.HeapStatus = "CRITICAL"
        }
    } catch {
        $status.FreeHeapKB = $null
        $status.HeapStatus = "UNKNOWN"
    }
    
    return $status
}

# Device list to monitor
$devices = @(
    @{Name="COM4-C3"; IP="192.168.1.100"; SerialPort="COM4"},
    @{Name="COM11-C6"; IP="192.168.1.102"; SerialPort="COM11"},
    @{Name="COM15-S3"; IP="192.168.1.200"; SerialPort="COM15"},
    @{Name="COM16-PZ"; IP="192.168.1.101"; SerialPort="COM16"}
)

Write-Host "`nStarting stability monitoring... Press Ctrl+C to stop anytime`n" -ForegroundColor Green

try {
    while ((Get-Date) -lt ($START_TIME + [TimeSpan]::Hours($MONITOR_DURATION_HOURS))) {
        $now = Get-Date
        Write-Host "`n[$($now.ToString('HH:mm:ss'))] Checking all devices..." -ForegroundColor Cyan
        
        foreach ($device in $devices) {
            $result = Test-DeviceHealth -Name $device.Name -IP $device.IP -SerialPort $device.SerialPort
            $results += $result
            
            # Display status
            $statusIcon = switch ($result.WebStatus) {
                "OK" { "✅" }
                "FAIL" { "❌" }
                default { "⚠️" }
            }
            
            Write-Host "  $statusIcon $($result.Name) - Web: $($result.WebLatencyMs)ms | " +
                       "MQTT: $($result.MQTTStatus) | Heap: $($result.FreeHeapKB)KB" -ForegroundColor White
        }
        
        # Log to file every hour
        if ($now.Minute -eq 0) {
            $logFile = ".pio\stability-monitor-$(Get-Date -Format 'yyyyMMdd-HHmm').csv"
            $results | Export-Csv -Path $logFile -NoTypeInformation -Encoding UTF8
            Write-Host "`n[$($now.ToString('HH:mm:ss'))] Data logged to $logFile" -ForegroundColor Yellow
        }
        
        # Sleep until next check
        Start-Sleep -Seconds ($CHECK_INTERVAL_MINUTES * 60)
    }
} catch {
    Write-Host "`nMonitoring interrupted!" -ForegroundColor Red
    
    # Save current results
    $lastLog = ".pio/stability-monitor-last.csv"
    $results | Export-Csv -Path $lastLog -NoTypeInformation -Encoding UTF8
    Write-Host "Final data saved to $lastLog" -ForegroundColor Yellow
} finally {
    Write-Host "`n=========================================" -ForegroundColor Cyan
    Write-Host "Stability Monitoring Complete" -ForegroundColor Cyan
    Write-Host "Total checks performed: $($results.Count)" -ForegroundColor Yellow
    
    # Generate summary statistics
    Write-Host "`n--- Summary Statistics ---" -ForegroundColor Cyan
    
    foreach ($device in $devices) {
        $deviceResults = $results | Where-Object {$_.Name -eq $device.Name}
        if ($deviceResults.Count -gt 0) {
            $avgWebLatency = ($deviceResults | Where-Object {$_.WebLatencyMs -ne $null}).WebLatencyMs | Measure-Object -Average
            $criticalHeapCount = ($deviceResults | Where-Object {$_.HeapStatus -eq "CRITICAL"}).Count
            
            Write-Host "$($device.Name): $([math]::Round($avgWebLatency.Average, 2))ms avg latency, " +
                       "$criticalHeapCount critical heap events" -ForegroundColor White
        }
    }
    
    Write-Host "=========================================" -ForegroundColor Cyan
}
