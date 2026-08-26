# Test network connectivity for FastBee devices

$ErrorActionPreference = "Continue"
Write-Host "`n=== Network Check for FastBee Devices ===" -ForegroundColor Cyan

$ips = @("192.168.5.17","192.168.5.24","192.168.5.25","192.168.5.26","192.168.5.27","192.168.5.30")
foreach ($ip in $ips) {
    try {
        $html = curl.exe --noproxy "*" --max-time 3 http://$ip/ 2>$null
        if ($html -and $html.Length -gt 1000) {
            Write-Host "$ip: Web available ($($html.Length) bytes)" -ForegroundColor Green
            $api = curl.exe --noproxy "*" --max-time 2 http://$ip/api/system/status 2>$null
            if ($api -and $api.Length -gt 500) {
                Write-Host "  API OK: $($api.Length) bytes" -ForegroundColor Yellow
                $json = ConvertFrom-Json $api 2>$null
                if ($json) {
                    Write-Host "  Type: $($json.deviceType), MAC: $($json.macAddress)" -ForegroundColor White
                }
            }
        } else {
            Write-Host "$ip: Not ready" -ForegroundColor Gray
        }
    } catch {
        Write-Host "$ip: Error - $_" -ForegroundColor Red
    }
    Start-Sleep -Milliseconds 300
}

Write-Host "`nScanning full range..." -ForegroundColor Yellow
for ($i = 2; $i -le 50; $i++) {
    $ip = "192.168.5.$i"
    try {
        $response = curl.exe --noproxy "*" --max-time 1 http://$ip/api/system/status 2>&1
        if ($response.Length -gt 500) {
            Write-Host "Found: $ip" -ForegroundColor Green
        }
    } catch {}
}
