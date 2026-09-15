# FastBee-Arduino 浏览器自动化测试运行脚本
# 用法:
#   .\run-browser-tests.ps1 -DeviceIP 192.168.1.100
#   .\run-browser-tests.ps1 -SerialPort COM4 -Project smoke
#   .\run-browser-tests.ps1 -DeviceIP 192.168.1.100 -Project smoke
#   .\run-browser-tests.ps1 -DeviceIP 192.168.1.100 -Project perf -Headed

param(
    [string]$DeviceIP = "",

    [string]$SerialPort = "",

    [string]$DevicePort = "80",
    [string]$Project = "",          # smoke|core|features|auxiliary|regression|perf|linkage|web
    [string]$Grep = "",             # 可选用例筛选，例如 DEV-057|DEV-058|DEV-059
    [switch]$Headed,                # 有头模式（可视化）
    [switch]$DebugMode,            # 调试模式
    [switch]$InstallOnly,           # 仅安装依赖
    [string]$WiFiSSID = $env:FASTBEE_STA_SSID,
    [string]$WiFiPassword = $env:FASTBEE_STA_PASSWORD,
    [string]$MQTTBroker = "",
    [string]$MQTTClientId = "",
    [string]$MQTTUsername = "",
    [string]$MQTTPassword = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BrowserTestDir = Join-Path $ProjectRoot "test\browser"

# An explicit grep can select a read-only case from a broad project (for
# example a UI regression). Do not require credentials unless the selected
# subset can execute a Wi-Fi/network write; an unrestricted project remains
# conservative.
$hasExplicitSubset = -not [string]::IsNullOrWhiteSpace($Grep)
$writesNetwork = if ($hasExplicitSubset) {
    $Grep -match '(?i)(NET-|WIFI-|network|wifi)'
} else {
    [string]::IsNullOrWhiteSpace($Project) -or $Project -in @("core", "regression")
}
if ($writesNetwork -and ([string]::IsNullOrWhiteSpace($WiFiSSID) -or [string]::IsNullOrWhiteSpace($WiFiPassword))) {
    throw "Network-writing browser tests require -WiFiSSID/-WiFiPassword or FASTBEE_STA_SSID/FASTBEE_STA_PASSWORD."
}
$writesMqtt = [string]::IsNullOrWhiteSpace($Project) -or $Project -eq "core"
if ($writesMqtt -and @($MQTTBroker, $MQTTClientId, $MQTTUsername, $MQTTPassword).Where({ [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
    throw "MQTT browser tests require explicit broker, client ID, username, and password values."
}

if (-not $DeviceIP) {
    if (-not $SerialPort) {
        throw "必须提供 -DeviceIP，或提供 -SerialPort 通过串口自动发现当前 IP。"
    }

    $healthJson = Join-Path ([IO.Path]::GetTempPath()) ("fastbee-health-{0}.json" -f [guid]::NewGuid())
    try {
        Write-Host "正在从 $SerialPort 获取设备当前地址..." -ForegroundColor Cyan
        & node (Join-Path $ProjectRoot "scripts\test-device-health.js") --port $SerialPort --seconds 35 --require-sta --json $healthJson
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $healthJson)) {
            throw "无法从 $SerialPort 读取设备健康状态。"
        }
        $healthResult = Get-Content -LiteralPath $healthJson -Raw | ConvertFrom-Json
        $deviceResult = $healthResult.results | Where-Object { $_.port -eq $SerialPort.ToUpperInvariant() } | Select-Object -First 1
        if (-not $deviceResult.telemetry.ip) {
            throw "$SerialPort 在采集窗口内没有输出网络 IP。"
        }
        if ($deviceResult.telemetry.networkMode -ne "STA") {
            throw "$SerialPort 当前处于 $($deviceResult.telemetry.networkMode) 模式，地址 $($deviceResult.telemetry.ip)，尚未接入局域网。"
        }
        $DeviceIP = $deviceResult.telemetry.ip
        Write-Host "已发现 $SerialPort -> $DeviceIP" -ForegroundColor Green
    } finally {
        Remove-Item -LiteralPath $healthJson -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " FastBee-Arduino 浏览器自动化测试" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "设备地址: http://${DeviceIP}:${DevicePort}" -ForegroundColor Yellow
Write-Host "测试目录: $BrowserTestDir" -ForegroundColor Yellow

# 切换到测试目录
Push-Location $BrowserTestDir

try {
    # 1. 安装依赖
    if (-not (Test-Path "node_modules") -or $InstallOnly) {
        Write-Host "`n[1/4] 安装依赖..." -ForegroundColor Green
        $env:PYTHONIOENCODING = "utf-8"
        npm install
        npx playwright install chromium
        if ($InstallOnly) {
            Write-Host "依赖安装完成" -ForegroundColor Green
            return
        }
    } else {
        Write-Host "`n[1/4] 依赖已安装，跳过" -ForegroundColor DarkGray
    }

    # 2. 健康检查
    Write-Host "`n[2/4] 设备健康检查..." -ForegroundColor Green
    $healthUrl = "http://${DeviceIP}:${DevicePort}/api/health"
    $healthOk = $false
    $lastHealthError = $null
    # Wi-Fi reconnect and low-memory admission control can make one request slow;
    # retry a bounded number of times without hiding a genuinely unreachable device.
    for ($attempt = 1; $attempt -le 3 -and -not $healthOk; $attempt++) {
        try {
            $resp = Invoke-WebRequest -Uri $healthUrl -TimeoutSec 10 -UseBasicParsing
            if ($resp.StatusCode -eq 200) {
                Write-Host "  设备在线: $healthUrl -> 200 OK (第 $attempt 次尝试)" -ForegroundColor Green
                $healthOk = $true
            } else {
                $lastHealthError = "HTTP $($resp.StatusCode)"
            }
        } catch {
            $lastHealthError = $_.Exception.Message
        }
        if (-not $healthOk -and $attempt -lt 3) {
            Write-Host "  健康检查第 $attempt 次失败，3 秒后重试: $lastHealthError" -ForegroundColor DarkYellow
            Start-Sleep -Seconds 3
        }
    }
    if (-not $healthOk) {
        Write-Host "  设备不可达: $healthUrl" -ForegroundColor Red
        Write-Host "  请确认设备已启动且网络可达" -ForegroundColor Red
        Write-Host "  错误: $lastHealthError" -ForegroundColor Red
        throw "设备不可达: $healthUrl"
    }

    # 3. 设置环境变量
    Write-Host "`n[3/4] 配置环境变量..." -ForegroundColor Green
    $env:DEVICE_IP = $DeviceIP
    $env:DEVICE_PORT = $DevicePort
    $env:WIFI_SSID = $WiFiSSID
    $env:WIFI_PASSWORD = $WiFiPassword
    $env:MQTT_BROKER = $MQTTBroker
    $env:MQTT_CLIENT_ID = $MQTTClientId
    $env:MQTT_USERNAME = $MQTTUsername
    $env:MQTT_PASSWORD = $MQTTPassword
    Write-Host "  WiFi SSID: $WiFiSSID"
    Write-Host "  MQTT Broker: $MQTTBroker"

    # 4. 运行测试
    Write-Host "`n[4/4] 运行测试..." -ForegroundColor Green

    # Invoke the local Playwright CLI through node instead of npx.cmd.  On
    # Windows, cmd.exe treats a regex alternation passed to --grep (for
    # example `MEM-|WEB-`) as a shell pipeline when npx forwards arguments.
    # Calling the JS entrypoint directly preserves the argument as data.
    $playwrightArgs = @("test")
    if ($Project -eq "smoke") {
        $playwrightArgs += "--config=playwright.device.config.ts"
        Write-Host "  受限设备策略: 单 worker、零重试、首次失败停止" -ForegroundColor Yellow
    }
    if ($Project) {
        $playwrightArgs += "--project=$Project"
        Write-Host "  测试项目: $Project" -ForegroundColor Yellow
    }
    if ($Grep) {
        $playwrightArgs += @("--grep", $Grep)
        Write-Host "  用例筛选: $Grep" -ForegroundColor Yellow
    }
    if ($Headed) {
        $playwrightArgs += "--headed"
        Write-Host "  模式: 有头（可视化）" -ForegroundColor Yellow
    }
    if ($DebugMode) {
        $playwrightArgs += "--debug"
        Write-Host "  模式: 调试" -ForegroundColor Yellow
    }

    $cmd = "node node_modules/@playwright/test/cli.js " + ($playwrightArgs -join ' ')
    Write-Host "  命令: $cmd" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "--------------------------------------------" -ForegroundColor DarkGray

    $playwrightCli = Join-Path $BrowserTestDir "node_modules\@playwright\test\cli.js"
    if (-not (Test-Path -LiteralPath $playwrightCli -PathType Leaf)) {
        throw "Playwright CLI not found: $playwrightCli"
    }
    & node $playwrightCli @playwrightArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Browser tests failed with exit code $LASTEXITCODE."
    }

    Write-Host "`n--------------------------------------------" -ForegroundColor DarkGray
    Write-Host "测试完成！" -ForegroundColor Green
    Write-Host "查看报告: cd test\browser; npx playwright show-report reports\html" -ForegroundColor Yellow

} finally {
    Pop-Location
}
