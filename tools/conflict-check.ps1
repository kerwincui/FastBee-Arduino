# 冲突排查：.12 与 .55 分别是什么设备
foreach ($ip in @('192.168.5.12','192.168.5.55','192.168.5.40','192.168.5.11')) {
    Write-Host "===== $ip ====="
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 5 -UseBasicParsing
        $ct = $r.Headers['Content-Type']
        $body = $r.Content.Substring(0, [Math]::Min(120, $r.Content.Length)) -replace "`r`n"," "
        Write-Host "  health 200 ct=$ct body=$body"
    } catch { Write-Host "  health FAIL: $($_.Exception.Message)" }
}
Write-Host "== mDNS =="
foreach ($d in @('fastbee.local','fastbee-2.local','fastbee-3.local','fastbee-4.local')) {
    try {
        $r = Resolve-DnsName -Name $d -ErrorAction Stop | Where-Object { $_.Type -eq 'A' } | Select-Object -First 1
        Write-Host "  $d -> $($r.IP4Address)"
    } catch { Write-Host "  $d -> NXDOMAIN" }
}
Write-Host "DONE"
