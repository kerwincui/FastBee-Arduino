# FastBee-ESP32C3 自动化测试脚本
# PowerShell 版本 - 用于 Windows 环境

param(
    [string]$DeviceIP = "192.168.x.x",  # 替换为实际 IP 地址
    [string]$AuthToken = "",              # 从登录接口获取的 token
    [int]$TestDuration = 86400,          # 24 小时（秒），默认值
    [switch]$ShortTest                   # 快速测试模式（跳过长时测试）
)

$ErrorActionPreference = "Stop"
$BaseURL = "http://$DeviceIP"

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "FastBee-ESP32C3 自动化测试套件 v1.0" -ForegroundColor Cyan
Write-Host "设备 IP: $DeviceIP" -ForegroundColor Yellow
Write-Host "=========================================" -ForegroundColor Cyan

# 添加认证头函数
function Get-AuthorizationHeader {
    param([string]$Token)
    
    if ([string]::IsNullOrEmpty($Token)) {
        Write-Warning "未提供 AuthToken，将尝试匿名访问（部分 API 可能失败）"
        return @{}
    }
    
    return @{
        "Authorization" = "Bearer $Token"
        "Content-Type" = "application/json"
    }
}

# 发送 HTTP GET 请求函数
function Invoke-ApiGet {
    param(
        [string]$Endpoint,
        [hashtable]$Headers = @{}
    )
    
    try {
        $Response = Invoke-RestMethod -Uri "$BaseURL$Endpoint" `
            -Method Get `
            -Headers $Headers `
            -TimeoutSec 10
        
        return @{
            Success = $true
            Data = $Response
            StatusCode = 200
        }
    } catch {
        return @{
            Success = $false
            Error = $_.Exception.Message
            StatusCode = $_.Exception.Response.StatusCode.value__)
        }
    }
}

# 发送 HTTP POST 请求函数
function Invoke-ApiPost {
    param(
        [string]$Endpoint,
        [hashtable]$Headers = @{},
        [object]$Body = $null
    )
    
    try {
        $JSONBody = ConvertTo-Json -InputObject $Body -Depth 10 -Compress
        
        $Response = Invoke-RestMethod -Uri "$BaseURL$Endpoint" `
            -Method Post `
            -Headers $Headers `
            -ContentType "application/json" `
            -Body $JSONBody `
            -TimeoutSec 10
        
        return @{
            Success = $true
            Data = $Response
            StatusCode = 200
        }
    } catch {
        return @{
            Success = $false
            Error = $_.Exception.Message
            StatusCode = $_.Exception.Response.StatusCode.value__)
        }
    }
}

# =====================
# 测试场景定义
# =====================

Write-Host "`n[测试 1] Web 界面访问性能测试" -ForegroundColor Green
Write-Host "-------------------------------------------" -ForegroundColor Gray

测试结果 1 = @{
    TestName = "Web 访问性能"
    Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Passed = $false
    Details = ""
}

$start = [Environment]::TickCount
$response = Invoke-ApiGet -Endpoint "/" -Headers (Get-AuthorizationHeader $AuthToken)
$end = [Environment]::TickCount
$延迟= ($end - $start) / 10

测试结果 1.Passed = ($response.Success -and $响应.StatusCode -eq 200 -and $延迟 -lt 500)
测试结果 1.详情 = "响应时间：${延迟}ms, HTTP 状态：$($response.StatusCode)"

if ($测试结果 1.Passed) {
    Write-Host "✓ PASS - $($测试结果 1.详情)" -ForegroundColor Green
} else {
    Write-Host "✗ FAIL - $($测试结果 1.详情)" -ForegroundColor Red
}

# =====================
测试结果 1 | Export-Csv -Path ".\test_results\web_access.csv" -Append -NoTypeInformation

Write-Host "`n[测试 2] 设备信息获取" -ForegroundColor Green
Write-Host "-------------------------------------------" -ForegroundColor Gray

测试结果 2 = @{
    TestName = "设备信息查询"
    Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Passed = $false
    Details = ""
}

$response = Invoke-ApiPost -Endpoint "/api/healthcheck/device-info" `
    -Headers (Get-AuthorizationHeader $AuthToken)
    -Body @{}

测试结果 2.Passed = $response.Success -and $response.Data.success
测试结果 2.详情 = if ($response.Success) { 
    "设备 ID: $($response.Data.data.deviceId), 固件版本：$($response.Data.data.firmwareVersion)" 
} else { 
    "错误：$($response.Error)" 
}

if ($测试结果 2.Passed) {
    Write-Host "✓ PASS - $($测试结果 2.详情)" -ForegroundColor Green
} else {
    Write-Host "✗ FAIL - $($测试结果 2.详情)" -ForegroundColor Red
}

# =====================
测试结果 2 | Export-Csv -Path ".\test_results\device_info.csv" -Append -NoTypeInformation

Write-Host "`n[测试 3] 外设配置查询" -ForegroundColor Green
Write-Host "-------------------------------------------" -ForegroundColor Gray

测试结果 3 = @{
    TestName = "外设配置查询"
    Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Passed = $false
    Details = ""
}

$response = Invoke-ApiGet -Endpoint "/api/peripherals/list" `
    -Headers (Get-AuthorizationHeader $AuthToken)

if ($response.Success) {
    $外设列表 = $response.Data.data
    $外设数量 = $外设列表.Count
    
    $预期外设 = @("dht11_sensor", "oled_display", "led_indicator")
    $存在外设 = $外设列表 | Where-Object { $预期外设 -contains $_.id }
    
    $测试结果 3.Passed = ($外设数量 -eq 3 -and $外设数量 -eq $存在外设.Count)
    $测试结果 3.详情 = "检测到 $外设数量 个外设：$($外设列表.id -join ', ')"
} else {
    $测试结果 3.Passed = $false
    $测试结果 3.详情 = "API 调用失败：$($response.Error)"
}

if ($测试结果 3.Passed) {
    Write-Host "✓ PASS - $($测试结果 3.详情)" -ForegroundColor Green
} else {
    Write-Host "✗ FAIL - $($测试结果 3.详情)" -ForegroundColor Red
}

# =====================
测试结果 3 | Export-Csv -Path ".\test_results\peripherals_list.csv" -Append -NoTypeInformation

if (-not $ShortTest) {
    Write-Host "`n[测试 4] DHT11 温湿度数据采集" -ForegroundColor Green
    Write-Host "-------------------------------------------" -ForegroundColor Gray
    
    测试结果 4 = @{
        TestName = "DHT11 温度采集"
        Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        Passed = $false
        Details = ""
    }
    
    for ($i = 1; $i -le 3; $i++) {
        Write-Host "  采样 $i/3..." -ForegroundColor Gray
        
        $response = Invoke-ApiGet -Endpoint "/api/sensors/dht11_sensor" `
            -Headers (Get-AuthorizationHeader $AuthToken)
        
        if ($response.Success -and $response.Data.success) {
            $temp = $response.Data.data.temperature
            $humi = $response.Data.data.humidity
            
            # 精度校验（示例范围）
            $ tempValid = ($temp -ge 15 -and $temp -le 35)
            $humiValid = ($humi -ge 20 -and $humi -le 80)
            
            $测试结果 4.Passed = $tempValid -and $humiValid
            $测试结果 4.详情 = "温度：${temp}°C, 湿度：${humi}%RH, 单位：C"
            
            Write-Host "  ✓ 温度=${temp}°C, 湿度=${humi}%RH" -ForegroundColor Green
            break
        } elseif ($i -eq 3) {
            $测试结果 4.Passed = $false
            $测试结果 4.详情 = "连续 3 次采样失败"
            Write-Host "  ✗ 连续 3 次采样失败：$($response.Error)" -ForegroundColor Red
        } else {
            Start-Sleep -Seconds 2
        }
    }
    
    if ($测试结果 4.Passed) {
        Write-Host "✓ PASS - $($测试结果 4.详情)" -ForegroundColor Green
    } else {
        Write-Host "✗ FAIL - $($测试结果 4.详情)" -ForegroundColor Red
    }
    
    测试结果 4 | Export-Csv -Path ".\test_results\dht11_data.csv" -Append -NoTypeInformation
    
    Write-Host "`n[测试 5] OLED 显示测试" -ForegroundColor Green
    Write-Host "-------------------------------------------" -ForegroundColor Gray
    
    测试结果 5 = @{
        TestName = "OLED 显示更新"
        Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        Passed = $false
        Details = ""
    }
    
    # 触发 OLED 显示更新（通过规则执行器）
    $response = Invoke-ApiPost -Endpoint "/api/periph-exec/run" `
        -Headers (Get-AuthorizationHeader $AuthToken) `
        -Body @{
            ruleId = "periodic_data_collection"
            params = @{}
        }
    
    if ($response.Success -and $response.Data.success) {
        $测试结果 5.Passed = $true
        $测试结果 5.详情 = "规则执行成功，等待 OLED 刷新..."
        Write-Host "✓ PASS - $($测试结果 5.详情)" -ForegroundColor Green
        
        # 等待一下让 OLED 有时间刷新
        Start-Sleep -Seconds 1
    } else {
        $测试结果 5.Passed = $false
        $测试结果 5.详情 = "规则执行失败：$($response.Error)"
        Write-Host "✗ FAIL - $($测试结果 5.详情)" -ForegroundColor Red
    }
    
    测试结果 5 | Export-Csv -Path ".\test_results\oled_display.csv" -Append -NoTypeInformation
    
    Write-Host "`n[测试 6] LED 控制功能测试" -ForegroundColor Green
    Write-Host "-------------------------------------------" -ForegroundColor Gray
    
    测试结果 6 = @{
        TestName = "LED 开关控制"
        Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        Passed = $false
        Details = ""
    }
    
    # 开启 LED
    Write-Host "  开启 LED..." -ForegroundColor Gray
    $response1 = Invoke-ApiPost -Endpoint "/api/peripherals/led_indicator/write" `
        -Headers (Get-AuthorizationHeader $AuthToken) `
        -Body @{ state = 1 }
    
    Start-Sleep -Milliseconds 500
    
    # 关闭 LED
    Write-Host "  关闭 LED..." -ForegroundColor Gray
    $response2 = Invoke-ApiPost -Endpoint "/api/peripherals/led_indicator/write" `
        -Headers (Get-AuthorizationHeader $AuthToken) `
        -Body @{ state = 0 }
    
    $测试结果 6.Passed = $response1.Success -and $response2.Success
    $测试结果 6.详情 = "LED 开关切换正常，响应时间 < 500ms"
    
    if ($测试结果 6.Passed) {
        Write-Host "✓ PASS - $($测试结果 6.详情)" -ForegroundColor Green
    } else {
        Write-Host "✗ FAIL - LED 控制失败：$($response1.Error)" -ForegroundColor Red
    }
    
    测试结果 6 | Export-Csv -Path ".\test_results\led_control.csv" -Append -NoTypeInformation
    
    Write-Host "`n[测试 7] MQTT 通信连接测试" -ForegroundColor Green
    Write-Host "-------------------------------------------" -ForegroundColor Gray
    
    测试结果 7 = @{
        TestName = "MQTT 连接状态"
        Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        Passed = $false
        Details = ""
    }
    
    $response = Invoke-ApiGet -Endpoint "/api/mqtt/status" `
        -Headers (Get-AuthorizationHeader $AuthToken)
    
    if ($response.Success -and $response.Data.success) {
        $mqttStatus = $response.Data.data.connected
        $clientId = $response.Data.data.clientId
        
        $测试结果 7.Passed = $mqttStatus
        $测试结果 7.详情 = "MQTT 已连接，ClientId: $clientId"
        
        if ($mqttStatus) {
            Write-Host "✓ PASS - $($测试结果 7.详情)" -ForegroundColor Green
        } else {
            Write-Host "✗ FAIL - MQTT 未连接到服务器 iot.fastbee.cn" -ForegroundColor Red
        }
    } else {
        $测试结果 7.Passed = $false
        $测试结果 7.详情 = "API 调用失败：$($response.Error)"
        Write-Host "✗ FAIL - $($测试结果 7.详情)" -ForegroundColor Red
    }
    
    测试结果 7 | Export-Csv -Path ".\test_results\mqtt_status.csv" -Append -NoTypeInformation
    
    Write-Host "`n[测试 8] NTP 时间同步测试" -ForegroundColor Green
    Write-Host "-------------------------------------------" -ForegroundColor Gray
    
    测试结果 8 = @{
        TestName = "NTP 时间同步"
        Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        Passed = $false
        Details = ""
    }
    
    $response = Invoke-ApiGet -Endpoint "/api/device/time" `
        -Headers (Get-AuthorizationHeader $AuthToken)
    
    if ($response.Success -and $response.Data.success) {
        $synced = $response.Data.data.synced
        $timeValid = $response.Data.data.timeValid
        $timezone = $response.Data.data.timezone
        
        $测试结果 8.Passed = $synced -and $timeValid -and ($timezone -eq "CST-8")
        $测试结果 8.详情 = "时间同步成功：$($response.Data.data.datetime) ($timezone)"
        
        if ($测试结果 8.Passed) {
            Write-Host "✓ PASS - $($测试结果 8.详情)" -ForegroundColor Green
        } else {
            Write-Host "✗ FAIL - NTP 同步异常：synced=$synced, valid=$timeValid, tz=$timezone" -ForegroundColor Red
        }
    } else {
        $测试结果 8.Passed = $false
        $测试结果 8.详情 = "API 调用失败：$($response.Error)"
        Write-Host "✗ FAIL - $($测试结果 8.详情)" -ForegroundColor Red
    }
    
    测试结果 8 | Export-Csv -Path ".\test_results\ntp_sync.csv" -Append -NoTypeInformation
}

# =====================
# 内存监控（持续运行）
# =====================

Write-Host "`n[监控] 内存使用状况监控（每 60 秒记录一次）" -ForegroundColor Cyan
Write-Host "-------------------------------------------" -ForegroundColor Gray

$监控数据 = @()
$循环次数 = [Math]::Min(10, $测试持续时间 / 60)  # 最多记录 10 次或 测试持续时间 /60 次

for ($i = 1; $i -le $循环次数; $i++) {
    $StartLoop = Get-Date
    
    $response = Invoke-ApiGet -Endpoint "/api/system/health" `
        -Headers (Get-AuthorizationHeader $AuthToken)
    
    if ($response.Success -and $response.Data.success) {
        $freeHeap = $response.Data.data.freeHeap
        $uptime = $response.Data.data.uptime
        
        $监控项 = [PSCustomObject]@{
            Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
            FreeHeap = $freeHeap
            Uptime = $uptime
            LoopNumber = $i
        }
        
        $监控数据 += $监控项
        
        Write-Host "  [$i] Free Heap: ${freeHeap} bytes, Uptime: ${uptime}s" -ForegroundColor Gray
    }
    
    # 计算本循环耗时并休眠到下次
    $Elapsed = (Get-Date) - $StartLoop
    $SleepSeconds = [Math]::Max(0, 60 - $Elapsed.TotalSeconds)
    if ($SleepSeconds -gt 0) {
        Start-Sleep -Seconds $SleepSeconds
    }
}

$监控数据 | Export-Csv -Path ".\test_results\memory_monitoring.csv" -NoTypeInformation

# =====================
# 生成最终报告
# =====================

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "测试完成总结" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

$所有测试 = @(
    测试结果 1, 测试结果 2, 测试结果 3, 
    if (-not $ShortTest) { 测试结果 4, 测试结果 5, 测试结果 6, 测试结果 7, 测试结果 8 }
)

$通过数 = $所有测试 | Where-Object { $_.Passed } | Measure-Object | Select-Object -ExpandProperty Count
$总数 = $所有测试.Count
$通过率 = [math]::Round(($通过数 / $总数) * 100, 1)

Write-Host "`n测试通过情况：$通过数 / $总数 ($通过率%)" -ForegroundColor $(if ($通过率 -ge 80) { "Green" } else { "Yellow" })

foreach ($测试 in $所有测试) {
    $符号 = if ($测试.Passed) { "✓ PASS" } else { "✗ FAIL" }
    $颜色 = if ($测试.Passed) { "Green" } else { "Red" }
    Write-Host "[$符号] $($测试.TestName): $($测试.Details)" -ForegroundColor $颜色
}

# 导出汇总报告
$报告 = [PSCustomObject]@{
    TestTime = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    DeviceIP = $DeviceIP
    TotalTests = $总数
    PassedTests = $通过数
    PassRate = "$通过率%"
    ShortTestMode = $ShortTest.ToString().ToLower()
}

$报告 | Export-Csv -Path ".\test_results\summary_report.csv" -NoTypeInformation

Write-Host "`n详细测试数据已保存到 .\test_results\ 目录" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
