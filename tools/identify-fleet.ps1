# 识别候选 IP 的设备芯片型号（form 编码登录，避免 JSON body 不被支持）
$candidates = @('192.168.5.40','192.168.5.12','192.168.5.55','192.168.5.11','192.168.5.34')
$found = @()
foreach ($ip in $candidates) {
    Write-Host "===== $ip ====="
    try {
        $h = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 6 -UseBasicParsing
        if ($h.Content -notlike '{"success"*') { Write-Host "  not FastBee API (web page)"; continue }
    } catch { Write-Host "  health FAIL: $($_.Exception.Message)"; continue }
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
             -Body "username=admin&password=admin123" -ContentType "application/x-www-form-urlencoded" `
             -UseBasicParsing -TimeoutSec 10
        $tok = ($r.Content | ConvertFrom-Json)
        $token = $tok.token; if (-not $token) { $token = $tok.data.token }
        if (-not $token) { $token = $tok.sessionId }
        if ($token) {
            $info = Invoke-WebRequest -Uri "http://$ip/api/system/info" -Headers @{Authorization="Bearer $token"} -UseBasicParsing -TimeoutSec 10
            $j = $info.Content | ConvertFrom-Json
            $chip = $j.data.chipModel; if (-not $chip) { $chip = $j.chipModel }
            $up = $j.data.uptime; if (-not $up) { $up = $j.uptime }
            Write-Host "  chip=$chip uptime=$up"
            $found += [pscustomobject]@{ IP = $ip; Chip = $chip }
        } else { Write-Host "  no token in login resp" }
    } catch { Write-Host "  login/info FAIL: $($_.Exception.Message)" }
}
$found | ConvertTo-Json | Set-Content "d:\project\gitee\FastBee-Arduino\tools\logs\fleet-chips.json" -Encoding UTF8
Write-Host "IDENTIFY-DONE found=$($found.Count)"
