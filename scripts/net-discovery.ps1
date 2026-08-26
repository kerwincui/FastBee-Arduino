# Comprehensive Network Scanner for FastBee Devices

$ErrorActionPreference = "Continue"

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Output "  FastBee Full Network Discovery Scan" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

$foundDevices = @()
$startIP = 1
$endIP = 60

for ($i = $startIP; $i -le $endIP; $i++) {
    $ip = "192.168.5.$i"
    
    # First try ping (fast check)
    $ping = Test-Connection -ComputerName $ip -Count 1 -Quiet -ErrorAction SilentlyContinue
    
    if ($ping) {
        Write-Host "[$i] Device responding at $ip..." -ForegroundColor Gray
        
        # Try API endpoint
        try {
            $api = curl.exe --noproxy "*" --max-time 2 http://$ip/api/system/status 2>&1
            
            if ($api.Length -gt 300) {
                try {
                    $json = ConvertFrom-Json $api
                    $type = $json.deviceType
                    $mac = $json.macAddress
                    $mode = $json.mode
                    
                    # Color code by device type
                    $color = switch ($true) {
                        {$type -like "*C3*"} {"Green"}
                        {$type -like "*S3*"} {"Yellow"}
                        {$type -like "*C6*"} {"Magenta"}
                        default {"White"}
                    }
                    
                    Write-Output "  ✓ Found: $ip -> [$type] MAC:$mac Mode:$mode" -ForegroundColor $color
                    
                    $foundDevices += [PSCustomObject]@{
                        IP = $ip
                        Type = $type
                        MAC = $mac
                        Mode = $mode
                    }
                    
                } catch {
                    Write-Output "  ? Web page but no JSON: $ip" -ForegroundColor Yellow
                }
            } else {
                # Try web page only
                $html = curl.exe --noproxy "*" --max-time 2 http://$ip/ 2>&1
                if ($html -and $html.Length -gt 1000) {
                    Write-Output "  ! Web page available ($($html.Length) bytes), API not ready: $ip" -ForegroundColor Cyan
                }
            }
            
        } catch {
            Write-Output "  ! Request failed: $ip" -ForegroundColor DarkGray
        }
    }
    
    # Progress indicator every 10 IPs
    if (($i % 10) -eq 0) {
        Write-Host "  Scanned up to .$i... ($($endIP-$i) remaining)" -ForegroundColor DarkGray
    }
}

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Output "  Scan Results Summary" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

if ($foundDevices.Count -gt 0) {
    Write-Host "`n✓ Successfully found $($foundDevices.Count) FastBee device(s):`n" -ForegroundColor Green
    $foundDevices | Format-Table -AutoSize
    
    Write-Host "`n=========================================" -ForegroundColor Cyan
    Write-Output "  Next Steps:" -ForegroundColor Cyan
    Write-Host "=========================================" -ForegroundColor Cyan
    Write-Output "`nTo run Playwright E2E tests on these devices:" -ForegroundColor Yellow
    $devicesStr = $foundDevices.IP -join ','
    Write-Output "$env:DEVICES=`"$devicesStr`"" -ForegroundColor White
    Write-Output "npm run test:e2e" -ForegroundColor White
    
    Write-Output "`nOr use the quick command:" -ForegroundColor Yellow
    Write-Output '$env:DEVICES = "$devicesStr"; npm run test:e2e' -ForegroundColor Cyan
    
} else {
    Write-Host "`n✗ No FastBee devices found online." -ForegroundColor Red
    Write-Host "`nPossible reasons:" -ForegroundColor Yellow
    Write-Host "  - Devices haven't finished booting yet (wait 2-3 minutes after flash)"
    Write-Host "  - WiFi connection failed (SSID 'fastbee' may not exist)"
    Write-Host "  - Devices are in AP mode (try http://192.168.4.1)"
    Write-Host "  - Check router DHCP list for connected devices"
}

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "Scan Complete!" -ForegroundColor Green
Write-Host "=========================================\n" -ForegroundColor Cyan
