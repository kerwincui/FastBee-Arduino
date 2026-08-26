# 四设备 Web 快速探测：/api/health + 登录 + /api/system/info
$ips = @{
    'esp32' = '192.168.5.40'
    'c3'    = '192.168.5.12'
    'c6'    = '192.168.5.11'
    's3'    = '192.168.5.55'
}
$wc = New-Object System.Net.WebClient
foreach ($k in $ips.Keys) {
    $ip = $ips[$k]
    Write-Host "===== $k ($ip) ====="
    try {
        $health = $wc.DownloadString("http://$ip/api/health")
        Write-Host "  health: $($health.Substring(0, [Math]::Min(200, $health.Length)))"
    } catch { Write-Host "  health: FAIL $($_.Exception.Message)" }
    try {
        $resp = Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST -Body @{username='admin';password='admin123'} -UseBasicParsing -TimeoutSec 10
        Write-Host "  login: $($resp.StatusCode)"
        $tok = ($resp.Content | ConvertFrom-Json)
        $token = $tok.token; if (-not $token) { $token = $tok.data.token }
        if ($token) {
            try {
                $info = Invoke-WebRequest -Uri "http://$ip/api/system/info" -Headers @{Authorization="Bearer $token"} -UseBasicParsing -TimeoutSec 10
                $j = $info.Content | ConvertFrom-Json
                if ($j.data) { $j = $j.data }
                Write-Host "  chip=$($j.chipModel) fw=$($j.firmwareVersion) heap=$($j.freeHeap)"
            } catch { Write-Host "  system/info: FAIL $($_.Exception.Message)" }
        } else { Write-Host "  login resp keys: $($tok.PSObject.Properties.Name -join ',')" }
    } catch { Write-Host "  login: FAIL $($_.Exception.Message)" }
}
Write-Host "PROBE-DONE"
