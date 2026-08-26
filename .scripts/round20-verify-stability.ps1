# FastBee Round 20 - Device Verification Script
# This script continuously monitors device for DANGEROUSLY LONG errors and memory stability

$scriptStartTime = Get-Date
$logPath = ".pio\puzhong\long-COM16.log"
$reportInterval = 300  # 5 minutes

Write-Host "=== Round 20: Device Stability Verification ===" -ForegroundColor Cyan
Write-Host "Started at: $($scriptStartTime.ToString('yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Yellow
Write-Host "Device IP: http://fastbee.local (mDNS resolved) - Verifying HTTP API..." -ForegroundColor Gray

# Wait for stable HTTP response
for ($i = 1; $i -le 20; $i++) {
    $httpResult = curl.exe --noproxy "*" http://fastbee.local/api/system/status -s -MaxTime 10
    if ($httpResult) {
        Write-Host "[✓] Device fully online via HTTP!" -ForegroundColor Green
        break
    } else {
        Write-Host "[?] HTTP not ready yet... attempt $i/20" -ForegroundColor Yellow
        Start-Sleep -Seconds 15
    }
}

if (-not $httpResult) {
    Write-Host "[!] HTTP still unavailable after timeout, continuing with serial monitoring only" -ForegroundColor Red
}

Write-Host "`n[Starting] Continuous DANGEROUSLY LONG monitoring (will run for at least 20 minutes)..." -ForegroundColor Cyan

# Main monitoring loop
while ((Get-Date).Subtract($scriptStartTime).TotalMinutes -lt 25) {
    if (Test-Path $logPath) {
        # Check for new log entries
        $recentLogs = Get-Content $logPath -Tail 100
        
        # Count DANGEROUSLY LONG errors since last check
        $dangerousLogs = $recentLogs | Where-Object { $_ -match "\[DANGEROUSLY LONG\]" }
        
        if ($dangerousLogs.Count -gt 0) {
            Write-Host "[⚠️] DANGEROUSLY LONG error detected!`n$($dangerousLogs | Select-Object -Last 3)" -ForegroundColor Red
        } else {
            Write-Host "[✓] No DANGEROUSLY LONG errors (monitoring so far: $(Get-Date).Subtract($scriptStartTime).Minutes.Minutes)`n" -ForegroundColor Green
        }
        
        # Check heap levels
        $heapValues = $recentLogs | Select-String "LOOP-ALIVE.*heap=" | ForEach-Object { 
            [int]($_ -replace ".*heap=", "") 
        } | Select-Object -Last 5
        
        if ($heapValues.Count -eq 5) {
            $avgHeap = ($heapValues | Measure-Object -Average).Average
            $minHeap = $heapValues | Measure-Object -Minimum).Minimum
            
            if ($minHeap -lt 29000) {
                Write-Host "[⚠️] Low heap warning! Min: $minHeap bytes, Avg: $([int]$avgHeap) bytes" -ForegroundColor Yellow
            } elseif ($minHeap -ge 29000) {
                Write-Host "[✓] Heap stable - Min: $minHeap bytes, Avg: $([int]$avgHeap) bytes" -ForegroundColor Green
            }
        }
        
        # Check for MemGuard events
        $memguardEvents = $recentLogs | Where-Object { $_ -match "MEMGUARD|MEMRECOVER" }
        if ($memguardEvents.Count -gt 0) {
            Write-Host "[INFO] Recent MemGuard events:`n$($memguardEvents | Select-Object -Last 2)" -ForegroundColor Cyan
        }
    }
    
    # Sleep before next check
    Start-Sleep -Seconds 60
    
    # Report progress every 5 minutes
    $elapsedMinutes = [math]::Round((Get-Date).Subtract($scriptStartTime).TotalMinutes)
    if ($elapsedMinutes % 5 -eq 0) {
        Write-Host "`n=== Progress Report ($(Get-Date).ToString('HH:mm:ss')), Elapsed: $elapsedMinutes min ===" -ForegroundColor Magenta
    }
}

Write-Host "`n=== Verification Complete ===" -ForegroundColor Cyan
Write-Host "Total monitoring time: $($scriptStartTime).Subtract((Get-Date)).TotalMinutes Minutes" -ForegroundColor Yellow
Write-Host "Final status: Please review the complete log file: $logPath" -ForegroundColor White
