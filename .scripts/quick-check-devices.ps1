# Quick Fleet Device Check Script
param(
    [string[]]$DeviceIPs = @("192.168.5.34", "192.168.5.66", "192.168.5.31", "192.168.5.116"),
    [int]$RetryCount = 3,
    [int]$RetryDelay = 5
)

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "Quick Fleet Device Health Check" -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan

$onlineDevices = 0
$totalDevices = $DeviceIPs.Count

foreach ($ip in $DeviceIPs) {
    Write-Host "Checking $ip..." -ForegroundColor Yellow
    
    for ($retry = 1; $retry -le $RetryCount; $retry++) {
        try {
            $response = Invoke-RestMethod -Uri "http://$ip/api/system/status" -TimeoutSec 5 -UseBasicParsing
            
            # Got response
            $uptime = if($response.uptime_seconds -gt 0) { "$($response.uptime_seconds)s" } else { "<1s" }
            $wifiMode = if($response.wifi.mode) { "$($response.wifi.mode)" } else { "None" }
            $wifiIP = if($response.wifi.ip_address) { "$($response.wifi.ip_address)" } else { "N/A" }
            $freeDRAM = if($response.memory_info.free_dram -gt 0) { "$([math]::Round($response.memory_info.free_dram/1024, 1))KB" } else { "N/A" }
            
            Write-Host "  ✓ Online (Uptime: $uptime, WiFi: $wifiMode, IP: $wifiIP, Free DRAM: $freeDRAM)" -ForegroundColor Green
            $onlineDevices++
            break
            
        } catch {
            if ($retry -lt $RetryCount) {
                Write-Host "  Retry $retry/$RetryCount... waiting ${RetryDelay}s" -ForegroundColor Gray
                Start-Sleep -Seconds $RetryDelay
            } else {
                Write-Host "  ✗ Not responding" -ForegroundColor Red
            }
        }
    }
}

# Summary
Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "Summary: $onlineDevices/$totalDevices devices online" -ForegroundColor $(if($onlineDevices -eq $totalDevices){"Green"}else{"Yellow"})
Write-Host "============================================================`n" -ForegroundColor Cyan

if ($onlineDevices -gt 0) {
    Write-Host "Next Steps:" -ForegroundColor Cyan
    Write-Host "1. Run detailed health check: .\.scripts\run-fleet-tests.ps1 -DeviceIP <IP>" -ForegroundColor White
    Write-Host "2. Run E2E tests: npm run test:e2e" -ForegroundColor White
    Write-Host "3. Start stability monitoring: .\.scripts\fastbee-fleet-deploy.ps1 -StabilityTest" -ForegroundColor White
}

exit 0
