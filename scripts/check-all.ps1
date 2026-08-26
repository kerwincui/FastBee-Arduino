# FastBee 舰队快速健康检查（自动发现，零硬编码 IP）
#
# 背景：路由器 DHCP 租约会隔夜漂移，硬编码 IP 的健康检查脚本会整体失效。
# 本脚本通过 test/e2e/helpers/discover-devices.js 按 MAC 身份锚自动发现在线设备，
# 逐台检查：Web 探活 -> 登录 -> mDNS 域名，并做舰队级域名唯一性校验。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/check-all.ps1
# 退出码：0 全部健康；1 存在失败项
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

Write-Host '=== FastBee fleet health check (auto-discovery) ==='
$ipsRaw = node "$root\test\e2e\helpers\discover-devices.js" --ips 2>$null
if (-not $ipsRaw) {
    Write-Host 'FATAL: no FastBee devices discovered on subnet' -ForegroundColor Red
    exit 1
}
$ips = ($ipsRaw -split ',') | Where-Object { $_ }
Write-Host ("discovered: " + ($ips -join ', '))

$domains = @{}   # mdnsDomain -> ip
$failures = @()

foreach ($ip in $ips) {
    Write-Host ""
    Write-Host "--- $ip ---"
    # 1. 免认证探活
    $code = curl.exe --noproxy '*' --max-time 8 -s -o NUL -w '%{http_code}' "http://$ip/api/system/status"
    if ($code -ne '200') {
        $failures += "$ip status probe HTTP:$code"
        Write-Host "  status probe: HTTP $code FAIL" -ForegroundColor Red
        continue
    }
    Write-Host "  status probe: 200 OK"

    # 2. 登录
    $ck = Join-Path $env:TEMP ("fb-check-" + $ip.Replace('.', '-') + ".txt")
    $login = curl.exe --noproxy '*' --max-time 10 -s -c $ck -X POST "http://$ip/api/auth/login" -d 'username=admin&password=admin123'
    if ($login -notmatch '"success":true') {
        $failures += "$ip login failed"
        Write-Host "  login: FAIL ($login)" -ForegroundColor Red
        continue
    }
    Write-Host '  login: OK'

    # 3. 网络状态与 mDNS 域名
    $statusRaw = curl.exe --noproxy '*' --max-time 10 -s -b $ck "http://$ip/api/network/status"
    try {
        $status = $statusRaw | ConvertFrom-Json
        $d = $status.data
        $domain = if ($d.mdnsDomain) { $d.mdnsDomain } elseif ($d.network) { $d.network.mdnsDomain } else { '' }
        $rssi = $d.rssi
        Write-Host ("  mdnsDomain: {0}  rssi: {1}" -f $domain, $rssi)
        if (-not $domain) {
            $failures += "$ip mdnsDomain missing"
        } elseif ($domains.ContainsKey($domain)) {
            $failures += ("mDNS conflict: {0} -> {1}, {2}" -f $domain, $domains[$domain], $ip)
        } else {
            $domains[$domain] = $ip
        }
        if (($rssi -ne $null) -and ($rssi -lt -75)) {
            Write-Host "  WARN: RSSI $rssi weak signal, device may be intermittently offline" -ForegroundColor Yellow
        }
    } catch {
        $failures += "$ip network/status parse failed"
        Write-Host '  network/status: parse FAIL' -ForegroundColor Red
    }
    Remove-Item $ck -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host '=== fleet mDNS domain table ==='
foreach ($k in ($domains.Keys | Sort-Object)) { Write-Host ("  {0}.local -> {1}" -f $k, $domains[$k]) }

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host 'FAILURES:' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host ('ALL HEALTHY: {0} devices, {1} unique domains' -f $ips.Count, $domains.Count) -ForegroundColor Green
exit 0
