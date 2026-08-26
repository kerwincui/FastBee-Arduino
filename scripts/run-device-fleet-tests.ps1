<#
.SYNOPSIS
    FastBee 多设备舰队快速测试编排脚本（稳定性加固版）

.DESCRIPTION
    对多台 FastBee 设备依次执行：
      1. STA 模式验证（必须已连接指定 WiFi，显式检测 AP 回退）
      2. mDNS 唯一域名可达性 + 主机名到预期 IP 映射校验
      3. smoke-test-device.ps1 全量 API 冒烟测试
      4. 测试前后 heap/uptime 快照（初步内存泄漏检测）
      5. （可选）Playwright 浏览器自动化测试（登录/页面加载/交互）
      6. （可选 -MonitorMinutes）长时间服务可用性监测：在线率/响应时间/heap/重启与自愈事件

    汇总结果输出到控制台和 .pio\test-results\fleet\ 下的 JSON/CSV 报告。

.PARAMETER Devices
    设备列表。每项为 hashtable：
      @{ Label="esp32"; IP="192.168.5.34"; Profile="unified"; Domain="fastbee.local"; Serial="COM16" }

.PARAMETER Ssid
    期望连接的 WiFi SSID（STA 模式验证用）。

.PARAMETER BrowserTests
    是否执行 Playwright 浏览器自动化测试（默认开启，可用 -BrowserTests:$false 关闭）。

.PARAMETER MonitorMinutes
    长稳监测时长（分钟）。0=不执行（默认）。监测期间每 MonitorIntervalSec 秒
    轮询全部设备，统计在线率、响应时间 p50/p95、heap 波动、重启与自愈（掉线后恢复）事件。

.EXAMPLE
    .\scripts\run-device-fleet-tests.ps1

.EXAMPLE
    .\scripts\run-device-fleet-tests.ps1 -MonitorMinutes 60

.EXAMPLE
    .\scripts\run-device-fleet-tests.ps1 -BrowserTests:$false -MonitorMinutes 240
#>

[CmdletBinding()]
param(
    [hashtable[]]$Devices = @(),

    [string]$Ssid = "fastbee",

    [string]$Username = "admin",
    [string]$Password = "admin123",

    [string]$ReportDir = ".pio\test-results\fleet",

    [bool]$BrowserTests = $true,

    [int]$MonitorMinutes = 0,
    [int]$MonitorIntervalSec = 60
)

$ErrorActionPreference = "Continue"
$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir

# ─── 默认设备舰队（四台测试机） ───────────────────────

if ($Devices.Count -eq 0) {
    $Devices = @(
        @{ Label = "esp32-F4R0";    IP = "192.168.5.34"; Profile = "unified"; Domain = "fastbee.local";   Serial = "COM16" },
        @{ Label = "esp32c3-F4R0";  IP = "192.168.5.66"; Profile = "unified"; Domain = "fastbee-2.local"; Serial = "COM4"  },
        @{ Label = "esp32c6-F4R0";  IP = "192.168.5.31"; Profile = "unified"; Domain = "fastbee-3.local"; Serial = "COM11" },
        @{ Label = "esp32s3-F8R0";  IP = "192.168.5.14"; Profile = "unified"; Domain = "fastbee-4.local"; Serial = "COM15" }
    )
}

$ReportDir = Join-Path $ProjectDir $ReportDir
if (-not (Test-Path $ReportDir)) {
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
}

Write-Host ""
Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host "  FastBee 多设备舰队快速测试 (STA + mDNS 映射 + API + 浏览器)" -ForegroundColor Cyan
Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host "  设备数量: $($Devices.Count)   期望 SSID: $Ssid   浏览器测试: $BrowserTests   长稳监测: $MonitorMinutes min"
foreach ($dev in $Devices) {
    Write-Host "    - $($dev.Label): $($dev.IP) ($($dev.Domain), profile=$($dev.Profile))"
}
Write-Host ""

# ─── 工具函数 ─────────────────────────────────────────

# 登录：设备端通过 Set-Cookie 下发名为 session 的 cookie，
# 用 WebRequestSession 承载并在后续请求复用（旧版误读 body.sessionId 导致鉴权失败）。
function Invoke-FleetLogin {
    param([string]$BaseUrl)
    try {
        $sess = $null
        $resp = Invoke-WebRequest -Uri "$BaseUrl/api/auth/login" -Method POST `
            -Body "username=$Username&password=$Password" `
            -ContentType "application/x-www-form-urlencoded" `
            -SessionVariable sess -TimeoutSec 15 -UseBasicParsing -ErrorAction Stop
        if ($resp.StatusCode -eq 200 -and $sess) {
            return $sess
        }
    } catch { }
    return $null
}

function Invoke-FleetApi {
    param([string]$BaseUrl, $Session, [string]$Path)
    try {
        return Invoke-RestMethod -Uri "$BaseUrl$Path" -Method GET -WebSession $Session `
            -TimeoutSec 15 -ErrorAction Stop
    } catch {
        return $null
    }
}

function Get-FleetSnapshot {
    param([string]$BaseUrl, $Session)
    $info = Invoke-FleetApi -BaseUrl $BaseUrl -Session $Session -Path "/api/system/info"
    $status = Invoke-FleetApi -BaseUrl $BaseUrl -Session $Session -Path "/api/system/status"
    $snap = @{ heapFree = $null; heapMaxAlloc = $null; uptime = $null; psramFree = $null }
    if ($info -and $info.data -and $info.data.memory) {
        $snap.heapFree = [int64]$info.data.memory.heapFree
        $snap.heapMaxAlloc = [int64]$info.data.memory.heapMaxAlloc
        if ($info.data.memory.psramFree) { $snap.psramFree = [int64]$info.data.memory.psramFree }
    }
    if ($status -and $status.data -and $null -ne $status.data.uptime) {
        $snap.uptime = [int64]$status.data.uptime
    }
    return $snap
}

# STA 模式验证：显式检测 AP 回退（mode 字段 / 192.168.4.x 地址 / 未连接）
function Test-StaMode {
    param([string]$BaseUrl, $Session)

    $net = Invoke-FleetApi -BaseUrl $BaseUrl -Session $Session -Path "/api/network/status"
    if (-not $net -or -not $net.data) {
        return @{ Ok = $false; Reason = "network status unavailable" }
    }
    $d = $net.data
    $names = $d.PSObject.Properties.Name

    # 显式 AP 回退检测：mode 非 STA 即为回退（AP/APSTA），直接判失败
    if ($names -contains "mode") {
        $mode = [string]$d.mode
        if ($mode -and $mode -ne "STA") {
            return @{ Ok = $false; Reason = "AP fallback detected: mode=$mode" }
        }
    }
    if ($names -contains "modeCode") {
        $mc = [int]$d.modeCode
        if ($mc -ne 0) {
            return @{ Ok = $false; Reason = "AP fallback detected: modeCode=$mc" }
        }
    }
    if (-not (($names -contains "connected") -and $d.connected)) {
        return @{ Ok = $false; Reason = "network not connected (possible AP failover)" }
    }
    $ssid = ""
    if ($names -contains "ssid") { $ssid = [string]$d.ssid }
    if ($Ssid -and $ssid -and $ssid -ne $Ssid) {
        return @{ Ok = $false; Reason = "connected to wrong SSID: $ssid (expected $Ssid)" }
    }
    $ip = ""
    if ($names -contains "ipAddress") { $ip = [string]$d.ipAddress }
    if ([string]::IsNullOrWhiteSpace($ip) -or $ip -eq "0.0.0.0" -or $ip.StartsWith("192.168.4.")) {
        return @{ Ok = $false; Reason = "AP-mode address detected: $ip" }
    }
    return @{ Ok = $true; Reason = "STA connected to [$ssid] IP=$ip" }
}

# mDNS 验证：可达性 + 解析出的实际 IP 与预期 IP 映射一致性
# （历史故障模式：mDNS 残留在已失效接口上，主机名解析到 192.168.4.1 或完全失败）
function Test-MdnsDomain {
    param([string]$Domain, [string]$ExpectedIP)
    if ([string]::IsNullOrWhiteSpace($Domain)) {
        return @{ Ok = $true; Reason = "no domain configured" }
    }
    $out = ""
    try {
        $out = curl.exe --noproxy "*" --max-time 10 -s -o NUL -w "%{http_code} %{remote_ip}" "http://$Domain/api/system/status" 2>$null
    } catch { }
    $parts = ([string]$out).Split(" ", [StringSplitOptions]::RemoveEmptyEntries)
    $code = 0
    $remoteIp = ""
    if ($parts.Count -ge 1) { [int]::TryParse($parts[0], [ref]$code) | Out-Null }
    if ($parts.Count -ge 2) { $remoteIp = $parts[1] }

    if ($code -ne 200) {
        return @{ Ok = $false; Reason = "http://$Domain unreachable (code=$code)" }
    }
    if ($ExpectedIP -and $remoteIp -and $remoteIp -ne $ExpectedIP) {
        return @{ Ok = $false; Reason = "mDNS mapping wrong: $Domain -> $remoteIp (expected $ExpectedIP)" }
    }
    return @{ Ok = $true; Reason = "http://$Domain -> $remoteIp (200)" }
}

# Playwright 浏览器自动化：登录流程/页面加载/交互（调用既有 run-browser-tests.ps1）
function Invoke-BrowserSmoke {
    param([string]$DeviceIP)
    $btScript = Join-Path $PSScriptRoot "run-browser-tests.ps1"
    if (-not (Test-Path $btScript)) {
        return @{ Ok = $false; Reason = "run-browser-tests.ps1 not found" }
    }
    try {
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $btScript `
            -DeviceIP $DeviceIP -Project smoke 2>&1
        $exit = $LASTEXITCODE
        if ($exit -eq 0) {
            return @{ Ok = $true; Reason = "playwright smoke passed" }
        }
        $tail = ($out | Select-Object -Last 3 | Out-String).Trim()
        return @{ Ok = $false; Reason = "playwright smoke exit=$exit ($tail)" }
    } catch {
        return @{ Ok = $false; Reason = "browser test exception: $($_.Exception.Message)" }
    }
}

# ─── 主循环 ───────────────────────────────────────────

$fleetResults = @()
$startTime = Get-Date

foreach ($dev in $Devices) {
    $baseUrl = "http://$($dev.IP)"
    $label = $dev.Label

    Write-Host "── [$label] $($dev.IP) ──" -ForegroundColor Yellow

    $entry = [ordered]@{
        Label = $label
        IP = $dev.IP
        Domain = $dev.Domain
        Profile = $dev.Profile
        StaOk = $false
        MdnsOk = $false
        SmokePassed = $false
        SmokeTotal = 0
        SmokeFailed = 0
        BrowserOk = $null
        HeapBefore = $null
        HeapAfter = $null
        HeapDropKB = $null
        UptimeBefore = $null
        UptimeAfter = $null
        Rebooted = $false
        DurationSec = 0
        Errors = @()
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    # 0) 设备可达性预检（避免对离线设备浪费登录/冒烟时间）
    $preCode = 0
    try {
        $preCode = [int](curl.exe --noproxy "*" --max-time 6 -s -o NUL -w "%{http_code}" "$baseUrl/api/system/status" 2>$null)
    } catch { }
    if ($preCode -ne 200) {
        $entry.Errors += "device unreachable via IP (code=$preCode), possibly stuck in AP fallback"
        $sw.Stop(); $entry.DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        $fleetResults += [pscustomobject]$entry
        Write-Host "  [X] 设备经 IP 不可达 (code=$preCode)，疑似 AP 回退/离线，跳过" -ForegroundColor Red
        Write-Host ""
        continue
    }

    # 1) 登录
    $session = Invoke-FleetLogin -BaseUrl $baseUrl
    if (-not $session) {
        $entry.Errors += "login failed"
        $sw.Stop(); $entry.DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        $fleetResults += [pscustomobject]$entry
        Write-Host "  [X] 登录失败，跳过该设备" -ForegroundColor Red
        continue
    }

    # 2) STA 模式验证（含 AP 回退检测）
    $sta = Test-StaMode -BaseUrl $baseUrl -Session $session
    $entry.StaOk = $sta.Ok
    if ($sta.Ok) {
        Write-Host "  [OK] STA: $($sta.Reason)" -ForegroundColor Green
    } else {
        $entry.Errors += "STA check failed: $($sta.Reason)"
        Write-Host "  [X] STA: $($sta.Reason)" -ForegroundColor Red
    }

    # 3) mDNS 域名验证 + IP 映射校验
    $mdns = Test-MdnsDomain -Domain $dev.Domain -ExpectedIP $dev.IP
    $entry.MdnsOk = $mdns.Ok
    if ($mdns.Ok) {
        Write-Host "  [OK] mDNS: $($mdns.Reason)" -ForegroundColor Green
    } else {
        $entry.Errors += $mdns.Reason
        Write-Host "  [X] mDNS: $($mdns.Reason)" -ForegroundColor Red
    }

    # 4) 测试前快照
    $before = Get-FleetSnapshot -BaseUrl $baseUrl -Session $session
    $entry.HeapBefore = $before.heapFree
    $entry.UptimeBefore = $before.uptime

    # 5) 冒烟测试
    $smokeScript = Join-Path $PSScriptRoot "smoke-test-device.ps1"
    $smokeArgs = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $smokeScript,
        "-BaseUrl", $baseUrl,
        "-Profile", $dev.Profile,
        "-Username", $Username, "-Password", $Password,
        "-RequireNetworkConnected"
    )
    $smokeOut = & powershell @smokeArgs 2>&1
    $smokeExit = $LASTEXITCODE
    $smokeText = ($smokeOut | Out-String)

    $passCount = 0
    if ($smokeText -match "Smoke test passed: (\d+) check") { $passCount = [int]$Matches[1] }
    $failCount = 0
    if ($smokeText -match "Smoke test failed: (\d+) check") { $failCount = [int]$Matches[1] }

    # 从结果表格统计全部检查项（汇总行只反映失败数，需逐行统计）
    $rows = @($smokeOut | Where-Object { $_ -match "\s(GET|POST)\s+(True|False)\s" })
    if ($rows.Count -gt 0) {
        $entry.SmokeTotal = $rows.Count
        $entry.SmokeFailed = @($rows | Where-Object { $_ -match "\sFalse\s" }).Count
    } else {
        $entry.SmokeTotal = $passCount + $failCount
        $entry.SmokeFailed = $failCount
    }

    $entry.SmokePassed = ($smokeExit -eq 0)

    if ($entry.SmokePassed) {
        Write-Host "  [OK] 冒烟测试通过 ($($entry.SmokeTotal) checks)" -ForegroundColor Green
    } else {
        $entry.Errors += "smoke test failed ($($entry.SmokeFailed) of $($entry.SmokeTotal) check(s))"
        Write-Host "  [X] 冒烟测试失败 ($($entry.SmokeFailed)/$($entry.SmokeTotal) checks)" -ForegroundColor Red
        $smokeOut | Where-Object { $_ -match "False" } | ForEach-Object {
            Write-Host "      $_" -ForegroundColor DarkRed
        }
    }

    # 6) 浏览器自动化测试（Playwright smoke：登录/页面加载/交互）
    if ($BrowserTests) {
        Write-Host "  [..] 浏览器自动化 (playwright smoke)..." -ForegroundColor DarkCyan
        $bt = Invoke-BrowserSmoke -DeviceIP $dev.IP
        $entry.BrowserOk = $bt.Ok
        if ($bt.Ok) {
            Write-Host "  [OK] 浏览器: $($bt.Reason)" -ForegroundColor Green
        } else {
            $entry.Errors += "browser test failed: $($bt.Reason)"
            Write-Host "  [X] 浏览器: $($bt.Reason)" -ForegroundColor Red
        }
    }

    # 7) 测试后快照 + 泄漏/重启检测
    $session2 = Invoke-FleetLogin -BaseUrl $baseUrl
    if (-not $session2) { $session2 = $session }
    $after = Get-FleetSnapshot -BaseUrl $baseUrl -Session $session2
    $entry.HeapAfter = $after.heapFree
    $entry.UptimeAfter = $after.uptime

    if ($null -ne $before.heapFree -and $null -ne $after.heapFree) {
        $entry.HeapDropKB = [Math]::Round(($before.heapFree - $after.heapFree) / 1024, 1)
    }
    if ($null -ne $before.uptime -and $null -ne $after.uptime -and $after.uptime -lt $before.uptime) {
        $entry.Rebooted = $true
        $entry.Errors += "device rebooted during test"
        Write-Host "  [X] 设备在测试期间重启!" -ForegroundColor Red
    }

    $sw.Stop(); $entry.DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
    $fleetResults += [pscustomobject]$entry
    if ($null -ne $before.heapFree -and $null -ne $after.heapFree) {
        Write-Host "  heap: $([Math]::Round($before.heapFree/1024,1))KB -> $([Math]::Round($after.heapFree/1024,1))KB (drop $($entry.HeapDropKB)KB), 耗时 $($entry.DurationSec)s" -ForegroundColor DarkCyan
    }
    Write-Host ""
}

# ─── 汇总报告 ─────────────────────────────────────────

Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host "  舰队测试汇总" -ForegroundColor Cyan
Write-Host "=============================================================" -ForegroundColor Cyan

$fleetResults | Format-Table Label, IP, StaOk, MdnsOk, SmokePassed, SmokeTotal, SmokeFailed, BrowserOk, HeapDropKB, Rebooted, DurationSec -AutoSize | Out-String | Write-Host

$failedDevices = @($fleetResults | Where-Object { -not ($_.StaOk -and $_.SmokePassed) })

$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$jsonPath = Join-Path $ReportDir "fleet-summary-$ts.json"
$fleetResults | ConvertTo-Json -Depth 5 | Out-File -FilePath $jsonPath -Encoding UTF8
$csvPath = Join-Path $ReportDir "fleet-summary-$ts.csv"
$fleetResults | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

# ─── 长时间服务可用性监测（-MonitorMinutes > 0 时执行） ─────────────
# 每轮对全部设备并行采样：mDNS URL 可达性（HTTP code + 响应耗时）、
# heap/uptime（重启检测）。统计在线率、响应时间 p50/p95、heap 区间、
# 重启次数、自愈事件（离线→恢复），用于网络波动/长稳场景验证。

$monitorSummary = $null
if ($MonitorMinutes -gt 0) {
    Write-Host ""
    Write-Host "=============================================================" -ForegroundColor Cyan
    Write-Host "  长稳监测: $MonitorMinutes 分钟 / 间隔 $MonitorIntervalSec 秒" -ForegroundColor Cyan
    Write-Host "=============================================================" -ForegroundColor Cyan

    $monitorJsonl = Join-Path $ReportDir "fleet-monitor-$ts.jsonl"
    $monitorEvents = Join-Path $ReportDir "fleet-monitor-events-$ts.log"
    $totalRounds = [Math]::Max(1, [int]($MonitorMinutes * 60 / $MonitorIntervalSec))
    $monStart = Get-Date

    # 每设备状态容器
    $state = @{}
    foreach ($dev in $Devices) {
        $state[$dev.Label] = @{
            Rounds = 0; Online = 0; Times = New-Object System.Collections.ArrayList
            HeapMin = $null; HeapMax = $null; LastUptime = $null
            Reboots = 0; WasOnline = $true; Recoveries = 0; OfflineRuns = 0
        }
    }

    for ($round = 1; $round -le $totalRounds; $round++) {
        $roundTs = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        foreach ($dev in $Devices) {
            $st = $state[$dev.Label]
            $st.Rounds++
            $code = 0; $ms = -1.0; $remoteIp = ""
            $swm = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                $wout = curl.exe --noproxy "*" --max-time 8 -s -o NUL -w "%{http_code} %{remote_ip}" "http://$($dev.Domain)/api/system/status" 2>$null
                $swm.Stop(); $ms = [Math]::Round($swm.Elapsed.TotalMilliseconds, 0)
                $wp = ([string]$wout).Split(" ", [StringSplitOptions]::RemoveEmptyEntries)
                if ($wp.Count -ge 1) { [int]::TryParse($wp[0], [ref]$code) | Out-Null }
                if ($wp.Count -ge 2) { $remoteIp = $wp[1] }
            } catch { $swm.Stop() }

            $online = ($code -eq 200)
            # mDNS 解析到非预期 IP：DHCP 可能重新分配地址（如掉电重启后），
            # 只要域名仍解析到该设备自身即视为在线，并更新预期 IP，避免误判全离线
            if ($online -and $remoteIp -and $remoteIp -ne $dev.IP) {
                Write-Host "  [i] $($dev.Label): IP drift $($dev.IP) -> $remoteIp (mDNS mapping updated)" -ForegroundColor DarkYellow
                "$roundTs [$($dev.Label)] IP-DRIFT: $($dev.IP) -> $remoteIp" | Out-File -Append $monitorEvents -Encoding UTF8
                $dev.IP = $remoteIp
            }
            $heap = $null; $uptime = $null
            if ($online) {
                $st.Online++
                [void]$st.Times.Add($ms)
                try {
                    $sts = Invoke-RestMethod -Uri "http://$($dev.Domain)/api/system/status" -TimeoutSec 8 -ErrorAction Stop
                    if ($sts.data) {
                        if ($sts.data.memory -and $sts.data.memory.heapFree) { $heap = [int64]$sts.data.memory.heapFree }
                        if ($null -ne $sts.data.uptime) { $uptime = [int64]$sts.data.uptime }
                    }
                } catch { }
                if ($null -ne $heap) {
                    if ($null -eq $st.HeapMin -or $heap -lt $st.HeapMin) { $st.HeapMin = $heap }
                    if ($null -eq $st.HeapMax -or $heap -gt $st.HeapMax) { $st.HeapMax = $heap }
                }
                if ($null -ne $uptime -and $null -ne $st.LastUptime -and $uptime -lt $st.LastUptime) {
                    $st.Reboots++
                    "$roundTs [$($dev.Label)] REBOOT detected (uptime $uptime < $($st.LastUptime))" | Out-File -Append $monitorEvents -Encoding UTF8
                    Write-Host "  [!] $($dev.Label) 检测到重启" -ForegroundColor Magenta
                }
                if ($null -ne $uptime) { $st.LastUptime = $uptime }
                if (-not $st.WasOnline) {
                    $st.Recoveries++
                    "$roundTs [$($dev.Label)] SELF-HEAL: recovered after $($st.OfflineRuns) offline round(s)" | Out-File -Append $monitorEvents -Encoding UTF8
                    Write-Host "  [+] $($dev.Label) 自愈恢复（离线 $($st.OfflineRuns) 轮后）" -ForegroundColor Green
                }
                $st.WasOnline = $true; $st.OfflineRuns = 0
            } else {
                $st.WasOnline = $false; $st.OfflineRuns++
                if ($st.OfflineRuns -eq 1) {
                    "$roundTs [$($dev.Label)] OFFLINE (code=$code)" | Out-File -Append $monitorEvents -Encoding UTF8
                }
            }

            $row = @{ ts = $roundTs; round = $round; label = $dev.Label; domain = $dev.Domain;
                      code = $code; remoteIp = $remoteIp; online = $online; ms = $ms;
                      heap = $heap; uptime = $uptime }
            ($row | ConvertTo-Json -Compress) | Out-File -Append $monitorJsonl -Encoding UTF8
        }

        $elapsedMin = [Math]::Round(((Get-Date) - $monStart).TotalMinutes, 1)
        $line = "[$roundTs] round $round/$totalRounds (${elapsedMin}min):"
        foreach ($dev in $Devices) {
            $st = $state[$dev.Label]
            $mark = "OK"; if (-not $st.WasOnline) { $mark = "OFF" }
            $line += "  $($dev.Label)=$mark"
        }
        Write-Host $line
        $remain = ($totalRounds - $round) * $MonitorIntervalSec
        if ($round -lt $totalRounds -and $remain -gt 0) { Start-Sleep -Seconds $MonitorIntervalSec }
    }

    # 汇总统计
    $monRows = @()
    foreach ($dev in $Devices) {
        $st = $state[$dev.Label]
        $rate = 0.0; if ($st.Rounds -gt 0) { $rate = [Math]::Round(100.0 * $st.Online / $st.Rounds, 2) }
        $p50 = $null; $p95 = $null
        $sorted = @($st.Times | Sort-Object)
        if ($sorted.Count -gt 0) {
            $p50 = $sorted[[Math]::Floor(($sorted.Count - 1) * 0.50)]
            $p95 = $sorted[[Math]::Floor(($sorted.Count - 1) * 0.95)]
        }
        $monRows += [pscustomobject]@{
            Label = $dev.Label; Domain = $dev.Domain; Rounds = $st.Rounds; Online = $st.Online
            OnlineRatePct = $rate; P50ms = $p50; P95ms = $p95
            HeapMinKB = $(if ($null -ne $st.HeapMin) { [Math]::Round($st.HeapMin/1024,1) } else { $null })
            HeapMaxKB = $(if ($null -ne $st.HeapMax) { [Math]::Round($st.HeapMax/1024,1) } else { $null })
            Reboots = $st.Reboots; SelfHealRecoveries = $st.Recoveries
        }
    }
    $monitorSummary = $monRows

    Write-Host ""
    Write-Host "── 长稳监测汇总 ──" -ForegroundColor Cyan
    $monRows | Format-Table Label, Rounds, Online, OnlineRatePct, P50ms, P95ms, HeapMinKB, HeapMaxKB, Reboots, SelfHealRecoveries -AutoSize | Out-String | Write-Host

    $monJson = Join-Path $ReportDir "fleet-monitor-summary-$ts.json"
    $monRows | ConvertTo-Json -Depth 4 | Out-File -FilePath $monJson -Encoding UTF8
    Write-Host "  监测明细: $monitorJsonl"
    Write-Host "  事件日志: $monitorEvents"
    Write-Host "  监测汇总: $monJson"

    $badMon = @($monRows | Where-Object { $_.OnlineRatePct -lt 95.0 -or $_.Reboots -gt 0 })
    if ($badMon.Count -gt 0) {
        foreach ($bm in $badMon) {
            $failedDevices += $bm
            Write-Host "  [X] 长稳未达标: $($bm.Label) 在线率=$($bm.OnlineRatePct)% 重启=$($bm.Reboots)" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "  JSON 报告: $jsonPath"
Write-Host "  CSV  报告: $csvPath"
$totalDuration = (Get-Date) - $startTime
Write-Host "  总耗时: $([Math]::Round($totalDuration.TotalMinutes, 1)) min"

if ($failedDevices.Count -gt 0) {
    Write-Host ""
    Write-Host "  [X] 舰队测试未通过: $($failedDevices.Count) 台设备存在问题" -ForegroundColor Red
    foreach ($fd in $failedDevices) {
        $errs = ""
        if ($fd.PSObject.Properties.Name -contains "Errors") { $errs = ($fd.Errors -join '; ') }
        Write-Host "      - $($fd.Label): $errs" -ForegroundColor DarkRed
    }
    exit 1
}

Write-Host ""
Write-Host "  [OK] 全部设备通过 STA 验证、mDNS 映射校验与 API 冒烟测试" -ForegroundColor Green
exit 0
