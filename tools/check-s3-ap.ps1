# 检查 s3 是否回退 AP 模式 + 重试 mDNS
Write-Host "== WLAN networks =="
netsh wlan show networks 2>$null | Select-String -Pattern "SSID|信号" | Select-Object -First 30
Write-Host "== fastbee-4.local retry =="
1..5 | ForEach-Object {
    try {
        $r = Resolve-DnsName -Name "fastbee-4.local" -ErrorAction Stop | Where-Object { $_.Type -eq 'A' } | Select-Object -First 1
        if ($r) { Write-Host "  attempt $_ : $($r.IP4Address)"; return }
    } catch {}
    Write-Host "  attempt $_ : not resolved"
    Start-Sleep 4
}
Write-Host "CHECK-DONE"
