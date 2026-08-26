$s3 = '192.168.5.12'; $c6 = '192.168.5.13'
function Test-DeviceHealth($ip) {
    try {
        $h = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 6 -UseBasicParsing
        $j = $h.Content | ConvertFrom-Json
        Write-Host "$ip: $($h.StatusCode) up=$($j.timestamp) heap=$($j.data.freeHeap)"
        return $true
    } catch {
        Write-Host "$ip: FAIL $($_.Exception.Message.Substring(0, [Math]::Min(60, $_.Exception.Message.Length)))"
        return $false
    }
}
Write-Host "--- ESP32-S3 ---"
Test-DeviceHealth $s3
Write-Host "--- ESP32-C6 ---"
Test-DeviceHealth $c6
