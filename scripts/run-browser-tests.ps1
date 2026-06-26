# FastBee-Arduino 浏览器自动化测试运行脚本
# 用法:
#   .\run-browser-tests.ps1 -DeviceIP 192.168.1.100
#   .\run-browser-tests.ps1 -DeviceIP 192.168.1.100 -Project smoke
#   .\run-browser-tests.ps1 -DeviceIP 192.168.1.100 -Project perf -Headed

param(
    [Parameter(Mandatory=$true)]
    [string]$DeviceIP,

    [string]$DevicePort = "80",
    [string]$Project = "",          # smoke|core|features|auxiliary|regression|perf|linkage
    [switch]$Headed,                # 有头模式（可视化）
    [switch]$DebugMode,            # 调试模式
    [switch]$InstallOnly,           # 仅安装依赖
    [string]$WiFiSSID = "fastbee",
    [string]$WiFiPassword = "15208747707",
    [string]$MQTTBroker = "d23de4e7b2.st1.iotda-device.cn-east-3.myhuaweicloud.com",
    [string]$MQTTClientId = "6a03ed2d18855b39c518fbc7_xfxt_esp32_0_0_2026061809",
    [string]$MQTTUsername = "6a03ed2d18855b39c518fbc7_xfxt_esp32",
    [string]$MQTTPassword = "e027294c696eff9a35b9f950a1b6d2a2cf9832b74206afee7dfbb552d2e58bb3"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BrowserTestDir = Join-Path $ProjectRoot "test\browser"

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
    try {
        $healthUrl = "http://${DeviceIP}:${DevicePort}/api/health"
        $resp = Invoke-WebRequest -Uri $healthUrl -TimeoutSec 10 -UseBasicParsing
        if ($resp.StatusCode -eq 200) {
            Write-Host "  设备在线: $healthUrl -> 200 OK" -ForegroundColor Green
        } else {
            Write-Host "  设备响应异常: $($resp.StatusCode)" -ForegroundColor Red
            return
        }
    } catch {
        Write-Host "  设备不可达: $healthUrl" -ForegroundColor Red
        Write-Host "  请确认设备已启动且网络可达" -ForegroundColor Red
        Write-Host "  错误: $($_.Exception.Message)" -ForegroundColor Red
        return
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

    $cmd = "npx playwright test"
    if ($Project) {
        $cmd += " --project=$Project"
        Write-Host "  测试项目: $Project" -ForegroundColor Yellow
    }
    if ($Headed) {
        $cmd += " --headed"
        Write-Host "  模式: 有头（可视化）" -ForegroundColor Yellow
    }
    if ($DebugMode) {
        $cmd += " --debug"
        Write-Host "  模式: 调试" -ForegroundColor Yellow
    }

    Write-Host "  命令: $cmd" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "--------------------------------------------" -ForegroundColor DarkGray

    Invoke-Expression $cmd

    Write-Host "`n--------------------------------------------" -ForegroundColor DarkGray
    Write-Host "测试完成！" -ForegroundColor Green
    Write-Host "查看报告: cd test\browser; npx playwright show-report reports\html" -ForegroundColor Yellow

} finally {
    Pop-Location
}
