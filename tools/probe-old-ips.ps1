# 探测疑似 Espressif 设备 IP（静态配置残留）
foreach ($ip in @('192.168.5.45','192.168.5.46','192.168.5.49','192.168.5.50')) {
    Write-Host "===== $ip ====="
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 4 -UseBasicParsing
        $body = $r.Content.Substring(0, [Math]::Min(150, $r.Content.Length))
        Write-Host "  health: $($r.StatusCode) $body"
    } catch {
        # 再试根路径
        try {
            $r2 = Invoke-WebRequest -Uri "http://$ip/" -TimeoutSec 4 -UseBasicParsing
            Write-Host "  root: $($r2.StatusCode) ct=$($r2.Headers['Content-Type'])"
        } catch { Write-Host "  FAIL: $($_.Exception.Message)" }
    }
}
# 顺便 ping 唤醒
ping -n 1 -w 500 192.168.5.47 | Out-Null
arp -a | Select-String "192.168.5.4[5-9]"
Write-Host "DONE"
