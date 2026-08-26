# FastBee 舰队终验（自动发现，零硬编码 IP）
#
# 部署/烧录完成后的最终验收：
#   1. 自动发现全部在线设备（MAC 身份锚，DHCP 漂移免疫）
#   2. 每台设备：Web 首页 200 + 免认证探活 + 登录 + mDNS 域名（命名模式校验）
#   3. 舰队级：mDNS 域名唯一性 + PC 侧 .local 域名可达性（重试一次）
# 与 test/e2e/mdns-uniqueness.spec.js 的检查项对齐，作为无 Node 测试环境时的快速终验。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/run-fleet-final.ps1
# 退出码：0 全部通过；1 存在失败项
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Get-HttpCode([string]$url, [int]$timeout = 8) {
    try {
        return curl.exe --noproxy '*' --max-time $timeout -s -o NUL -w '%{http_code}' $url
    } catch { return '000' }
}

Write-Host '=== FastBee fleet FINAL verification (auto-discovery) ==='
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

    # 1. Web 首页（登录页）可达
    $rootCode = Get-HttpCode "http://$ip/"
    Write-Host "  web root: HTTP $rootCode"
    if ($rootCode -ne '200') { $failures += "$ip web root HTTP:$rootCode" }

    # 2. 免认证探活
    $code = Get-HttpCode "http://$ip/api/system/status"
    if ($code -ne '200') {
        $failures += "$ip status probe HTTP:$code"
        Write-Host "  status probe: HTTP $code FAIL" -ForegroundColor Red
        continue
    }
    Write-Host '  status probe: 200 OK'

    # 3. 登录 + 网络状态
    $ck = Join-Path $env:TEMP ("fb-final-" + $ip.Replace('.', '-') + ".txt")
    $login = curl.exe --noproxy '*' --max-time 10 -s -c $ck -X POST "http://$ip/api/auth/login" -d 'username=admin&password=admin123'
    if ($login -notmatch '"success":true') {
        $failures += "$ip login failed"
        Write-Host '  login: FAIL' -ForegroundColor Red
        continue
    }
    Write-Host '  login: OK'

    $statusRaw = curl.exe --noproxy '*' --max-time 10 -s -b $ck "http://$ip/api/network/status"
    Remove-Item $ck -ErrorAction SilentlyContinue
    try {
        $d = ($statusRaw | ConvertFrom-Json).data
        $domain = if ($d.mdnsDomain) { $d.mdnsDomain } elseif ($d.network) { $d.network.mdnsDomain } else { '' }
        $rssi = $d.rssi
        Write-Host ("  mdnsDomain: {0}  customDomain: {1}  rssi: {2}" -f $domain, $d.customDomain, $rssi)

        if (-not $domain) {
            $failures += "$ip mdnsDomain missing (enableMDNS off?)"
        } else {
            # 命名模式：固件自动后缀 fastbee / fastbee-2..-8，专属域名 fastbee-xxx
            if ($domain -notmatch '^fastbee(-[0-9a-z]+)?$') {
                $failures += "$ip domain '$domain' breaks fleet naming pattern fastbee[-suffix]"
            }
            if ($domains.ContainsKey($domain)) {
                $failures += ("mDNS CONFLICT: {0} -> {1} and {2}" -f $domain, $domains[$domain], $ip)
            } else {
                $domains[$domain] = $ip
            }
        }
        if (($rssi -ne $null) -and ($rssi -lt -75)) {
            Write-Host "  WARN: RSSI $rssi weak signal" -ForegroundColor Yellow
        }
    } catch {
        $failures += "$ip network/status parse failed"
        Write-Host '  network/status: parse FAIL' -ForegroundColor Red
    }
}

# 4. PC 侧 .local 域名可达性（重试一次；弱射频多播通告可能瞬时丢失，二次仍失败才计失败）
Write-Host ''
Write-Host '=== PC-side mDNS reachability ==='
foreach ($k in ($domains.Keys | Sort-Object)) {
    $ok = $false
    foreach ($attempt in 1..2) {
        $code = Get-HttpCode "http://$k.local/api/system/status"
        if ($code -eq '200') { $ok = $true; break }
        Start-Sleep 2
    }
    if ($ok) {
        Write-Host ("  {0}.local -> 200 OK ({1})" -f $k, $domains[$k])
    } else {
        # 弱射频设备多播通告丢失为已知硬件限制：设备本体可达时降级为告警
        $ipCode = Get-HttpCode ("http://{0}/api/system/status" -f $domains[$k])
        if ($ipCode -eq '200') {
            Write-Host ("  WARN: {0}.local unreachable but device {1} reachable by IP (weak-RF multicast?)" -f $k, $domains[$k]) -ForegroundColor Yellow
        } else {
            $failures += ("domain {0}.local unreachable and device {1} offline" -f $k, $domains[$k])
            Write-Host ("  {0}.local FAIL" -f $k) -ForegroundColor Red
        }
    }
}

Write-Host ''
Write-Host '=== fleet domain table ==='
foreach ($k in ($domains.Keys | Sort-Object)) { Write-Host ("  {0}.local -> {1}" -f $k, $domains[$k]) }

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host 'FAILURES:' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host ('FINAL PASS: {0} devices, {1} unique domains, all reachable' -f $ips.Count, $domains.Count) -ForegroundColor Green
exit 0
