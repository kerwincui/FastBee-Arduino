<#
.SYNOPSIS
    FastBee 多设备并行长期稳定性测试脚本

.DESCRIPTION
    对多台设备同时执行多轮全量浏览器自动化测试，验证设备在长时间压力下的稳定性。
    每台设备独立运行 Playwright 稳定性测试（2 workers），设备间完全并行。
    每轮结束后自动检测设备健康状态（崩溃、内存泄漏、uptime 重置）。

.PARAMETER Rounds
    每台设备的测试轮数（默认 5）

.PARAMETER Workers
    每台设备的并行 worker 数（默认 2）

.PARAMETER TestDelayMs
    测试间隔延迟毫秒（默认 3000）

.PARAMETER AutoReset
    启用崩溃自动串口复位

.PARAMETER StopOnFailure
    任一轮失败率超过阈值时停止后续轮次

.PARAMETER MaxFailureRatePercent
    单轮最大允许失败率百分比（默认 10）

.PARAMETER ReportDir
    报告输出目录（默认 .pio\test-results\stability）

.EXAMPLE
    # 单设备测试
    .\scripts\run-stability-tests.ps1 -Devices @(@{IP="192.168.1.100"; Serial="COM15"})

.EXAMPLE
    # 多设备并行测试
    .\scripts\run-stability-tests.ps1 -Devices @(
        @{IP="192.168.1.100"; Serial="COM15"},
        @{IP="192.168.1.101"; Serial="COM13"}
    ) -Rounds 10
#>

[CmdletBinding()]
param(
    [hashtable[]]$Devices = @(),

    [int]$Rounds = 5,

    [int]$Workers = 2,

    [int]$TestDelayMs = 3000,

    [switch]$AutoReset,

    [switch]$StopOnFailure,

    [double]$MaxFailureRatePercent = 10,

    [string]$ReportDir = ".pio\test-results\stability",

    [string]$ConfigFile = "playwright.stability.config.ts"
)

$ErrorActionPreference = "Continue"
$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$BrowserTestDir = Join-Path $ProjectDir "test\browser"
Set-Location $ProjectDir

# ─── 设备发现 ─────────────────────────────────────────

if ($Devices.Count -eq 0) {
    Write-Host "[Stability] 未指定设备，使用默认设备配置..." -ForegroundColor Yellow
    $Devices = @(
        @{ IP = "192.168.4.1"; Serial = ""; Label = "default" }
    )
}

# 为没有 Label 的设备生成标签
foreach ($dev in $Devices) {
    if (-not $dev.Label) {
        $dev.Label = $dev.IP -replace '\.', '-'
    }
    if (-not $dev.Serial) {
        $dev.Serial = ""
    }
}

Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║     FastBee 多设备并行长期稳定性测试                ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host "  设备数量    : $($Devices.Count)" -ForegroundColor White
Write-Host "  测试轮数    : $Rounds" -ForegroundColor White
Write-Host "  Workers/设备: $Workers" -ForegroundColor White
Write-Host "  测试延迟    : ${TestDelayMs}ms" -ForegroundColor White
Write-Host "  自动复位    : $(if ($AutoReset) { '启用' } else { '关闭' })" -ForegroundColor White
Write-Host "  失败率阈值  : ${MaxFailureRatePercent}%" -ForegroundColor White
Write-Host ""
Write-Host "  设备列表:" -ForegroundColor White
foreach ($dev in $Devices) {
    $serial = if ($dev.Serial) { $dev.Serial } else { "N/A" }
    Write-Host "    - $($dev.Label): $($dev.IP) (串口: $serial)" -ForegroundColor Gray
}
Write-Host ""

# ─── 确保报告目录存在 ─────────────────────────────────

$ReportDir = Join-Path $ProjectDir $ReportDir
if (-not (Test-Path $ReportDir)) {
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
}

# ─── 设备健康检查函数 ─────────────────────────────────

function Test-DeviceHealth {
    param([string]$BaseUrl, [int]$TimeoutSec = 10)

    try {
        $resp = Invoke-RestMethod -Uri "$BaseUrl/api/health" -TimeoutSec $TimeoutSec -ErrorAction Stop
        return @{ Ok = $true; Data = $resp }
    } catch {
        return @{ Ok = $false; Data = $null }
    }
}

function Get-DeviceInfo {
    param([string]$BaseUrl, [int]$TimeoutSec = 10)

    try {
        $resp = Invoke-RestMethod -Uri "$BaseUrl/api/system/info?probe=1" -TimeoutSec $TimeoutSec -ErrorAction Stop
        return $resp
    } catch {
        return $null
    }
}

function Reset-DeviceSerial {
    param([string]$Port)

    if ([string]::IsNullOrWhiteSpace($Port)) { return $false }
    try {
        $sq = [char]39
        $pyCode = 'import serial,time;s=serial.Serial({0}{1}{0},115200,timeout=2);s.rts=True;time.sleep(0.1);s.rts=False;time.sleep(0.1);s.close()' -f $sq, $Port
        & python -c $pyCode 2>$null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

# ─── 单设备稳定性测试函数 ─────────────────────────────

function Invoke-DeviceStabilityTest {
    param(
        [hashtable]$Device,
        [int]$Round,
        [int]$TotalRounds,
        [string]$OutputBase
    )

    $label = $Device.Label
    $ip = $Device.IP
    $serial = $Device.Serial
    $roundDir = Join-Path $OutputBase "$label\round-$Round"

    if (-not (Test-Path $roundDir)) {
        New-Item -ItemType Directory -Force -Path $roundDir | Out-Null
    }

    Write-Host "  [$label] 轮次 $Round/$TotalRounds 开始..." -ForegroundColor Cyan

    $env:DEVICE_IP = $ip
    $env:STABILITY_ROUNDS = "1"  # 每轮由外部脚本控制
    $env:STABILITY_WORKERS = [string]$Workers
    $env:TEST_DELAY_MS = [string]$TestDelayMs

    if ($serial -and $AutoReset) {
        $env:DEVICE_SERIAL = $serial
        $env:DEVICE_AUTO_RESET = "1"
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    try {
        $pwArgs = @(
            "test",
            "-c", $ConfigFile,
            "--reporter", "json"
        )

        $jsonOut = Join-Path $roundDir "results.json"
        $pwArgs += @("--output", $jsonOut)

        Set-Location $BrowserTestDir
        $output = & npx @pwArgs 2>&1
        Set-Location $ProjectDir

        $sw.Stop()

        # 解析结果
        $result = @{
            Label = $label
            Round = $Round
            Passed = $true
            DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            TotalTests = 0
            PassedTests = 0
            FailedTests = 0
            SkippedTests = 0
            FailureRate = 0
            Error = ""
        }

        if (Test-Path $jsonOut) {
            try {
                $jsonText = [System.IO.File]::ReadAllText($jsonOut, [System.Text.Encoding]::UTF8)
                $report = $jsonText | ConvertFrom-Json
                $suites = $report.suites
                if ($suites) {
                    # 递归统计测试结果
                    $allTests = @()
                    function Collect-Tests($suite) {
                        if ($suite.specs) {
                            foreach ($spec in $suite.specs) {
                                foreach ($test in $spec.tests) {
                                    $allTests += $test
                                }
                            }
                        }
                        if ($suite.suites) {
                            foreach ($child in $suite.suites) {
                                Collect-Tests $child
                            }
                        }
                    }
                    Collect-Tests $suites

                    $result.TotalTests = $allTests.Count
                    $result.PassedTests = @($allTests | Where-Object {
                        $_.results | Where-Object { $_.status -eq "expected" -or $_.status -eq "flaky" }
                    }).Count
                    $result.FailedTests = @($allTests | Where-Object {
                        $_.results | Where-Object { $_.status -eq "unexpected" }
                    }).Count
                    $result.SkippedTests = @($allTests | Where-Object {
                        $_.results | Where-Object { $_.status -eq "skipped" }
                    }).Count
                }
            } catch {
                $result.Error = "JSON 解析失败: $_"
            }
        } else {
            # 没有 JSON 输出，从 stdout 解析
            $passMatch = ($output | Out-String) -match '(\d+)\s+passed'
            $failMatch = ($output | Out-String) -match '(\d+)\s+failed'
            if ($passMatch) { $result.PassedTests = [int]$Matches[1] }
            if ($failMatch) { $result.FailedTests = [int]$Matches[1] }
            $result.TotalTests = $result.PassedTests + $result.FailedTests
        }

        if ($result.TotalTests -gt 0) {
            $result.FailureRate = [Math]::Round(($result.FailedTests / $result.TotalTests) * 100, 1)
        }
        if ($result.FailedTests -gt 0) {
            $result.Passed = $false
        }

        return $result
    }
    catch {
        $sw.Stop()
        Set-Location $ProjectDir
        return @{
            Label = $label
            Round = $Round
            Passed = $false
            DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            TotalTests = 0
            PassedTests = 0
            FailedTests = 0
            SkippedTests = 0
            FailureRate = 100
            Error = $_.Exception.Message
        }
    }
    finally {
        # 清理环境变量
        $env:DEVICE_IP = $null
        $env:STABILITY_ROUNDS = $null
        $env:STABILITY_WORKERS = $null
        $env:TEST_DELAY_MS = $null
        $env:DEVICE_SERIAL = $null
        $env:DEVICE_AUTO_RESET = $null
    }
}

# ─── 主流程：多设备并行 + 多轮循环 ────────────────────

$allResults = @()
$deviceHealthLog = @{}
$startTime = Get-Date

Write-Host "═══════════════════════════════════════════════════════" -ForegroundColor Yellow
Write-Host "  开始稳定性测试 - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Yellow
Write-Host "═══════════════════════════════════════════════════════" -ForegroundColor Yellow
Write-Host ""

for ($round = 1; $round -le $Rounds; $round++) {
    Write-Host "┌───────────────────────────────────────────────────┐" -ForegroundColor Magenta
    Write-Host "│  轮次 $round / $Rounds  $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Magenta
    Write-Host "└───────────────────────────────────────────────────┘" -ForegroundColor Magenta

    # 并行启动所有设备的测试
    $jobs = @()
    foreach ($dev in $Devices) {
        $jobState = @{
            Device = $dev
            Round = $round
            TotalRounds = $Rounds
            OutputBase = $ReportDir
        }
        $jobs += $jobState
    }

    # 串行执行每台设备的当前轮次（避免本机资源竞争）
    # 注意：如果需要完全并行，可改用 Start-Job，但嵌入式测试场景下串行更稳定
    $roundResults = @()
    foreach ($job in $jobs) {
        $result = Invoke-DeviceStabilityTest @job
        $roundResults += $result
        $allResults += $result

        # 输出轮次结果
        $statusColor = if ($result.Passed) { "Green" } else { "Red" }
        $statusText = if ($result.Passed) { "PASS" } else { "FAIL" }
        Write-Host "  [$($result.Label)] $statusText  $($result.PassedTests)/$($result.TotalTests) 通过  失败率: $($result.FailureRate)%  耗时: $($result.DurationSec)s" -ForegroundColor $statusColor
        if ($result.Error) {
            Write-Host "         错误: $($result.Error)" -ForegroundColor DarkRed
        }
    }

    # ─── 轮次间健康检查 ──────────────────────────────

    if ($round -lt $Rounds) {
        Write-Host ""
        Write-Host "  ── 轮次间健康检查 ──" -ForegroundColor DarkCyan

        foreach ($dev in $Devices) {
            $baseUrl = "http://$($dev.IP)"
            $health = Test-DeviceHealth $baseUrl 10

            if ($health.Ok) {
                $info = Get-DeviceInfo $baseUrl 10
                if ($info) {
                    $heapFree = [int](if ($info.heapFree) { $info.heapFree } else { 0 })
                    $uptime = [int](if ($info.uptime) { $info.uptime } else { 0 })

                    # 初始化该设备的健康日志
                    if (-not $deviceHealthLog.ContainsKey($dev.Label)) {
                        $deviceHealthLog[$dev.Label] = @{
                            InitialHeap = $heapFree
                            MinHeap = $heapFree
                            CrashCount = 0
                            LastUptime = $uptime
                        }
                    }

                    $log = $deviceHealthLog[$dev.Label]

                    # 检测崩溃（uptime 重置）
                    if ($uptime -lt $log.LastUptime -and $log.LastUptime -gt 10000) {
                        $log.CrashCount++
                        Write-Host "  [$($dev.Label)] ⚠ 检测到设备重启! 崩溃次数: $($log.CrashCount)" -ForegroundColor Red
                    }
                    $log.LastUptime = $uptime

                    # 追踪最低内存
                    if ($heapFree -lt $log.MinHeap) {
                        $log.MinHeap = $heapFree
                    }

                    $heapDrop = $log.InitialHeap - $heapFree
                    $heapDropKB = [Math]::Round($heapDrop / 1024, 1)
                    Write-Host "  [$($dev.Label)] ✓ 在线  DRAM空闲: $([Math]::Round($heapFree/1024, 1))KB  下降: ${heapDropKB}KB  uptime: $([Math]::Round($uptime/1000))s" -ForegroundColor Green
                } else {
                    Write-Host "  [$($dev.Label)] ✓ 在线 (信息获取失败)" -ForegroundColor Yellow
                }
            } else {
                Write-Host "  [$($dev.Label)] ✗ 离线!" -ForegroundColor Red

                if ($AutoReset -and $dev.Serial) {
                    Write-Host "  [$($dev.Label)] 尝试串口复位..." -ForegroundColor Yellow
                    $resetOk = Reset-DeviceSerial $dev.Serial
                    if ($resetOk) {
                        Write-Host "  [$($dev.Label)] 等待设备恢复 (60s)..." -ForegroundColor Yellow
                        Start-Sleep -Seconds 30
                        $recovered = $false
                        for ($retry = 0; $retry -lt 6; $retry++) {
                            $h = Test-DeviceHealth $baseUrl 10
                            if ($h.Ok) { $recovered = $true; break }
                            Start-Sleep -Seconds 5
                        }
                        if ($recovered) {
                            Write-Host "  [$($dev.Label)] ✓ 复位成功" -ForegroundColor Green
                        } else {
                            Write-Host "  [$($dev.Label)] ✗ 复位失败" -ForegroundColor Red
                        }
                    }
                }

                # 记录崩溃
                if ($deviceHealthLog.ContainsKey($dev.Label)) {
                    $deviceHealthLog[$dev.Label].CrashCount++
                }
            }
        }

        # 检查失败率阈值
        if ($StopOnFailure) {
            $failedDevices = @($roundResults | Where-Object { $_.FailureRate -gt $MaxFailureRatePercent })
            if ($failedDevices.Count -gt 0) {
                Write-Host ""
                Write-Host "  ⚠ 设备失败率超过 ${MaxFailureRatePercent}% 阈值，停止后续轮次" -ForegroundColor Red
                foreach ($fd in $failedDevices) {
                    Write-Host "    - $($fd.Label): $($fd.FailureRate)%" -ForegroundColor Red
                }
                break
            }
        }

        Write-Host ""
        # 轮次间短暂休息
        Start-Sleep -Seconds 3
    }
}

# ─── 汇总报告 ─────────────────────────────────────────

$totalDuration = (Get-Date) - $startTime

Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║              稳定性测试汇总报告                      ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# 按设备汇总
foreach ($dev in $Devices) {
    $devResults = @($allResults | Where-Object { $_.Label -eq $dev.Label })
    $totalTests = ($devResults | Measure-Object -Property TotalTests -Sum).Sum
    $passedTests = ($devResults | Measure-Object -Property PassedTests -Sum).Sum
    $failedTests = ($devResults | Measure-Object -Property FailedTests -Sum).Sum
    $totalDurationSec = ($devResults | Measure-Object -Property DurationSec -Sum).Sum
    $passedRounds = @($devResults | Where-Object { $_.Passed }).Count
    $totalRoundsExec = $devResults.Count

    $healthInfo = ""
    if ($deviceHealthLog.ContainsKey($dev.Label)) {
        $log = $deviceHealthLog[$dev.Label]
        $heapDrop = $log.InitialHeap - $log.MinHeap
        $healthInfo = "  崩溃次数: $($log.CrashCount)  DRAM最大下降: $([Math]::Round($heapDrop/1024, 1))KB"
    }

    $failRate = if ($totalTests -gt 0) { [Math]::Round(($failedTests / $totalTests) * 100, 1) } else { 0 }
    $statusColor = if ($failRate -eq 0) { "Green" } elseif ($failRate -le $MaxFailureRatePercent) { "Yellow" } else { "Red" }

    Write-Host "  [$($dev.Label)] $($dev.IP)" -ForegroundColor White
    Write-Host "    轮次: $passedRounds/$totalRoundsExec 通过  测试: $passedTests/$totalTests 通过  失败率: $failRate%" -ForegroundColor $statusColor
    Write-Host "    总耗时: $([Math]::Round($totalDurationSec, 1))s"
    if ($healthInfo) { Write-Host "    $healthInfo" -ForegroundColor DarkCyan }
    Write-Host ""
}

Write-Host "  总耗时: $([Math]::Round($totalDuration.TotalSeconds, 1))s ($([Math]::Round($totalDuration.TotalMinutes, 1))min)" -ForegroundColor White
Write-Host ""

# ─── 导出 CSV 报告 ────────────────────────────────────

$csvPath = Join-Path $ReportDir ("stability-summary-{0}.csv" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
$allResults | ForEach-Object {
    [pscustomobject]@{
        Label = $_.Label
        Round = $_.Round
        Passed = $_.Passed
        TotalTests = $_.TotalTests
        PassedTests = $_.PassedTests
        FailedTests = $_.FailedTests
        FailureRate = $_.FailureRate
        DurationSec = $_.DurationSec
        Error = $_.Error
    }
} | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

Write-Host "  报告已导出: $csvPath" -ForegroundColor DarkCyan

# ─── 退出码 ───────────────────────────────────────────

$failedAll = @($allResults | Where-Object { -not $_.Passed })
if ($failedAll.Count -gt 0) {
    $totalFailed = ($failedAll | Measure-Object -Property FailedTests -Sum).Sum
    $totalAll = ($allResults | Measure-Object -Property TotalTests -Sum).Sum
    $overallRate = if ($totalAll -gt 0) { [Math]::Round(($totalFailed / $totalAll) * 100, 1) } else { 100 }

    if ($overallRate -gt $MaxFailureRatePercent) {
        Write-Host ""
        Write-Host "  ✗ 稳定性测试未通过: 总失败率 ${overallRate}% 超过阈值 ${MaxFailureRatePercent}%" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "  ✓ 稳定性测试通过" -ForegroundColor Green
exit 0
