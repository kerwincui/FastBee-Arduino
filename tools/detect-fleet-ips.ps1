# 舰队设备 IP 发现：优先已知 IP -> mDNS -> 子网扫描
# 输出: label -> ip 映射（控制台 + tools\logs\fleet-ips.json）
Set-Location "d:\project\gitee\FastBee-Arduino"
$logDir = "tools\logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

$labels = @("esp32", "c3", "c6", "s3")
$knownIps = @("192.168.5.49", "192.168.5.47", "192.168.5.45", "192.168.5.46")
$domains  = @("fastbee.local", "fastbee-2.local", "fastbee-3.local", "fastbee-4.local")

function Test-DeviceHttp([string]$ip) {
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 6 -UseBasicParsing
        return ($r.StatusCode -eq 200)
    } catch { return $false }
}

function Get-DeviceInfo([string]$ip) {
    # 通过 /api/system/info 无法免鉴权获取芯片型号，用 health 响应或 chipId 猜测不可靠；
    # 返回登录后的 system/info（admin/admin123）
    try {
        $login = Invoke-RestMethod -Uri "http://$ip/api/auth/login" -Method POST `
            -Body @{ username = "admin"; password = "admin123" } `
            -ContentType "application/x-www-form-urlencoded" -TimeoutSec 10
        if (-not $login.sessionId) { return $null }
        $info = Invoke-RestMethod -Uri "http://$ip/api/system/info" -Headers @{ Authorization = "Bearer $($login.sessionId)" } -TimeoutSec 10
        return $info
    } catch { return $null }
}

$found = @{}

# 阶段1: 已知 IP 探测
Write-Host "== Phase 1: known IPs =="
foreach ($ip in $knownIps) {
    if (Test-DeviceHttp $ip) {
        $info = Get-DeviceInfo $ip
        $chip = ""
        if ($info -and $info.data -and $info.data.chipModel) { $chip = [string]$info.data.chipModel }
        Write-Host "  $ip ONLINE chip=$chip"
        $found[$ip] = $chip
    }
}

# 阶段2: mDNS 域名解析
Write-Host "== Phase 2: mDNS domains =="
foreach ($d in $domains) {
    try {
        $res = Resolve-DnsName -Name $d -ErrorAction Stop | Where-Object { $_.Type -in @('A','AAAA') } | Select-Object -First 1
        if ($res -and $res.IP4Address) {
            $ip = $res.IP4Address
            if (-not $found.ContainsKey($ip) -and (Test-DeviceHttp $ip)) {
                $info = Get-DeviceInfo $ip
                $chip = ""
                if ($info -and $info.data -and $info.data.chipModel) { $chip = [string]$info.data.chipModel }
                Write-Host "  $d -> $ip chip=$chip"
                $found[$ip] = $chip
            }
        }
    } catch { }
}

# 阶段3: 若不足4台，扫描 192.168.5.2-254 TCP:80 + /api/health 指纹
if ($found.Count -lt 4) {
    Write-Host "== Phase 3: subnet sweep (found=$($found.Count)/4) =="
    $candidates = @()
    2..254 | ForEach-Object {
        $ip = "192.168.5.$_"
        if ($found.ContainsKey($ip)) { return }
        try {
            $tcp = New-Object System.Net.Sockets.TcpClient
            $iar = $tcp.BeginConnect($ip, 80, $null, $null)
            if ($iar.AsyncWaitHandle.WaitOne(400) -and $tcp.Connected) { $candidates += $ip }
            $tcp.Close()
        } catch { }
    }
    Write-Host "  TCP-open candidates: $($candidates -join ', ')"
    foreach ($ip in $candidates) {
        if (Test-DeviceHttp $ip) {
            $info = Get-DeviceInfo $ip
            $chip = ""
            if ($info -and $info.data -and $info.data.chipModel) { $chip = [string]$info.data.chipModel }
            Write-Host "  $ip = FastBee device chip=$chip"
            $found[$ip] = $chip
        }
    }
}

Write-Host "== RESULT =="
$result = @()
foreach ($ip in $found.Keys) {
    $result += [pscustomobject]@{ IP = $ip; Chip = $found[$ip] }
    Write-Host ("{0,-16} {1}" -f $ip, $found[$ip])
}
$result | ConvertTo-Json | Out-File -FilePath "$logDir\fleet-ips.json" -Encoding UTF8
Write-Host "Total: $($found.Count) device(s)"
if ($found.Count -lt 4) { exit 1 } else { exit 0 }
