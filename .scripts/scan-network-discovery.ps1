# Network Scan to Discover All FastBee Devices
param(
    [int]$StartIP = 1,
    [int]$EndIP = 254,
    [int]$TimeoutSec = 2
)

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "FastBee Fleet Network Discovery Scanner" -ForegroundColor Cyan
Write-Host "Range: 192.168.5.$StartIP - 192.168.5.$EndIP" -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan

$foundDevices = @()
$startScan = Get-Date

for ($i = $StartIP; $i -le $EndIP; $i++) {
    $ip = "192.168.5.$i"
    
    try {
        $response = Invoke-RestMethod -Uri "http://$ip/api/system/status" -TimeoutSec $TimeoutSec -UseBasicParsing
        
        # Extract device info
        $uptime = if($response.uptime_seconds -gt 0) { "$($response.uptime_seconds)s" } else { "<1s" }
        $wifiMode = if($response.wifi.mode) { $response.wifi.mode } else { "None" }
        $wifiIP = if($response.wifi.ip_address) { $response.wifi.ip_address } else { "N/A" }
        
        # Try to get more details
        $macAddress = "Unknown"
        try {
            $devInfo = Invoke-RestMethod -Uri "http://$ip/api/device/info" -UseBasicParsing
            if ($devInfo.mac_address) { $macAddress = $devInfo.mac_address }
        } catch {}
        
        $hostname = "Unknown"
        try {
            if ($response.mdns.hostname) { $hostname = $response.mdns.hostname }
        } catch {}
        
        $freeDRAM = if($response.memory_info.free_dram -gt 0) { "$([math]::Round($response.memory_info.free_dram/1024, 1))KB" } else { "N/A" }
        
        # Determine device type based on characteristics
        $deviceType = "Unknown"
        if ($freeDRAM -match "^\d+") {
            $dramVal = [int]$freeDRAM -replace "KB",""
            if ($dramVal -gt 200) { $deviceType = "S3 (with PSRAM)" }
            elseif ($dramVal -gt 100) { $deviceType = "C6/C3/F4R0 (DRAM only)" }
        }
        
        Write-Host "[✓] $ip | Uptime: $($uptime.PadLeft(6)) | WiFi: $($wifiMode.PadLeft(5)) | DRAM: $($freeDRAM.PadLeft(8)) | MAC: $macAddress" -ForegroundColor Green
        
        $foundDevices += @{
            IP = $ip
            Uptime = $response.uptime_seconds
            WiFiMode = $wifiMode
            WiFiIP = $wifiIP
            MAC = $macAddress
            Hostname = $hostname
            FreeDRAM = $freeDRAM
            Type = $deviceType
        }
        
    } catch {}
}

$totalTime = ([DateTime]::Now - $startScan).TotalSeconds

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "Discovery Complete" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Found $($foundDevices.Count) devices in $([math]::Round($totalTime, 1)) seconds" -ForegroundColor $(if($foundDevices.Count -eq 4){"Green"}else{"Yellow"})

if ($foundDevices.Count -gt 0) {
    Write-Host "`nDevice Summary:" -ForegroundColor Yellow
    $foundDevices | Format-Table -AutoSize -Property @(@{Label="IP";Expression={$_.IP}}, 
                                                      @{Label="Uptime";Expression={$_.Uptime}s}, 
                                                      @{Label="WiFi";Expression={$_.WiFiMode}}, 
                                                      @{Label="Type";Expression={$_.Type}})
}

exit 0
