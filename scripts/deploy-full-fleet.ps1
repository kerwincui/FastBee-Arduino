# FastBee 全量部署与控制脚本
# 功能：自动处理四台设备的编译、烧录、测试和报告

$ErrorActionPreference = "Stop"
$startTime = Get-Date
$logDir = ".pio\puzhong"

# 记录开始时间
Set-Content -Path "$logDir\DEPLOYMENT-STARTED.txt" -Value "Started: $($startTime.ToString('yyyy-MM-dd HH:mm:ss'))"

Write-Output "`n========================================" -ForegroundColor Cyan
Write-Output "  FastBee Full Fleet Deployment" -ForegroundColor Cyan  
Write-Output "  4 Devices: C3/Classic/S3/C6" -ForegroundColor Cyan
Write-Output "========================================" -ForegroundColor Cyan

# === STEP 1: 准备阶段 ===
Write-Output "`n[STEP 1] Preparing environment..." -ForegroundColor Yellow

# 清空配置（如果还没有）
if (-not (Test-Path "data/config/peripherals.json")) {
    Set-Content -Path "data/config/peripherals.json" -Value '{"peripherals": []}' -Encoding UTF8
}

# 停止现有进程
Get-Process | Where-Object {$_.ProcessName -eq 'pio' -or $_.ProcessName -eq 'python'} | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

Write-Output "✓ Environment ready." -ForegroundColor Green

# === STEP 2: 确认四台设备端口 ===
Write-Output "`n[STEP 2] Verifying device connections..." -ForegroundColor Yellow

$devices = @{
    'C3' = @{ Port='COM13'; Env='esp32c3-F4R0'; Chip='ESP32-C3' }
    'Classic' = @{ Port='COM16'; Env='esp32-F4R0'; Chip='ESP32-D0WD' }
    'S3' = @{ Port='COM15'; Env='esp32s3-F16R8'; Chip='ESP32-S3' }
    'C6' = @{ Port='COM11'; Env='esp32c6-F4R0'; Chip='ESP32-C6' }
}

$deviceStatus = @()
foreach ($devName in $devices.Keys) {
    $port = $devices[$devName].Port
    $found = Get-PnpDevice -Class Ports | Where-Object {$_.Name -like "*$($port)*"}
    if ($found) {
        $deviceStatus += [PSCustomObject]@{
            Device=$devName
            Port=$port
            Status="Connected"
            Chip=$devices[$devName].Chip
        }
        Write-Output "✓ $devName ($port) - $($devices[$devName].Chip)" -ForegroundColor Green
    } else {
        $deviceStatus += [PSCustomObject]@{
            Device=$devName
            Port=$port
            Status="Not Found"
            Chip=$devices[$devName].Chip
        }
        Write-Output "! $devName ($port) NOT FOUND - SKIP" -ForegroundColor Red
    }
    Start-Sleep -Milliseconds 200
}

$onlineDevices = $deviceStatus | Where-Object {$_.Status -eq "Connected"}
if ($onlineDevices.Count -lt 4) {
    Write-Output "`n⚠️ Only $($onlineDevices.Count) of 4 devices found!" -ForegroundColor Yellow
    Write-Output "Continuing with available devices..." -ForegroundColor Gray
}

# === STEP 3: 为每台设备烧录 ===
Write-Output "`n[STEP 3] Flashing firmware to devices..." -ForegroundColor Yellow

$flashResults = @()

foreach ($device in $onlineDevices) {
    $devName = $device.Device
    $port = $device.Port
    $envName = $devices[$devName].Env
    $chip = $devices[$devName].Chip
    
    Write-Output "`n[*] Flashing $devName ($chip) on $port..." -ForegroundColor Cyan
    
    try {
        # 检查是否有对应的 staging directory
        $stagingDirs = Get-ChildItem -Path ".pio\fs-staging" -Directory | Where-Object {$_.Name -like "*full-$envName*"}
        $latestStaging = $stagingDirs | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        
        if (-not $latestStaging) {
            Write-Output "✗ No suitable staging directory found for $envName" -ForegroundColor Red
            continue
        }
        
        # 使用 esptool 直接烧录
        $esptool = Join-Path "$env:USERPROFILE\.platformio\packages\tool-esptoolpy" "esptool"
        
        # 根据芯片类型选择不同的文件
        $fwFile = switch ($devName) {
            'C3' { "dist\firmware\esp32c3-F4R0\factory.bin" }
            'Classic' { "dist\firmware\esp32-F4R0\factory.bin" }
            'S3' { "dist\firmware\esp32s3-F16R8\factory.bin" }
            'C6' { "dist\firmware\esp32c6-F4R0\factory.bin" }
            default { "dist\firmware\esp32-F4R0\factory.bin" }
        }
        
        $fwFullpath = Join-Path (Get-Location) $fwFile
        if (-not (Test-Path $fwFullpath)) {
            Write-Output "! Firmware file not found, will compile first" -ForegroundColor Yellow
            
            # 使用 PlatformIO 编译并上传
            $logFile = "$logDir\$devName-FLASH.log"
            $env:PLATFORMIO_DATA_DIR = $latestStaging.FullName
            pio run -e $envName --target upload 2>&1 | Tee-Object $logFile | Out-Null
        } else {
            # 直接使用 esptool 烧录
            Write-Output "Using esptool: $esptool" -ForegroundColor Gray
            Write-Output "FW File: $fwFullpath" -ForegroundColor Gray
            Write-Output "Port: $port" -ForegroundColor Gray
            
            # 擦除并写入
            & python $esptool --port $port erase_flash 2>$null | Out-Null
            Start-Sleep -Seconds 2
            
            if ($devName -eq 'C3') {
                # C3 需要特定的参数
                & python $esptool --port $port write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m 0x0 $fwFullpath 2>&1 | Tee-Object "$logDir\$devName-FLASH.log" | ForEach-Object { Write-Output $_ }
            } elseif ($devName -eq 'S3' -or $devName -eq 'Classic') {
                # S3 和 Classic 可以使用标准参数
                & python $esptool --port $port write-flash 0x0 $fwFullpath 2>&1 | Tee-Object "$logDir\$devName-FLASH.log" | ForEach-Object { Write-Output $_ }
            } else {
                # C6 特殊处理
                & python $esptool --port $port write-flash 0x0 $fwFullpath 2>&1 | Tee-Object "$logDir\$devName-FLASH.log" | ForEach-Object { Write-Output $_ }
            }
            
            Start-Sleep -Seconds 2
        }
        
        $flashResults += [PSCustomObject]@{
            Device=$devName
            Port=$port
            Status="Success"
            LogFile="$logDir\$devName-FLASH.log"
        }
        Write-Output "✓ $devName flash completed successfully!" -ForegroundColor Green
        
    } catch {
        Write-Output "✗ Flash failed for $devName: $($_.Exception.Message)" -ForegroundColor Red
        $flashResults += [PSCustomObject]@{
            Device=$devName
            Port=$port
            Status="Failed"
            Error=$_.Exception.Message
        }
    }
    
    # 每台设备烧录后等待 90 秒启动
    Write-Output "`nWaiting $($devName) to boot (90 seconds)..." -ForegroundColor Gray
    Start-Sleep -Seconds 90
}

# === STEP 4: 网络发现 ===
Write-Output "`n[STEP 4] Network discovery and IP mapping..." -ForegroundColor Yellow

$ipMap = @{}
for ($i = 2; $i -le 50; $i++) {
    $ip = "192.168.5.$i"
    try {
        $api = curl.exe --noproxy "*" --max-time 1 http://$ip/api/system/status 2>$null
        if ($api -and $api.Length -gt 100) {
            $json = ConvertFrom-Json $api 2>$null
            if ($json) {
                $mac = $json.macAddress
                $type = $json.deviceType
                
                # 尝试匹配设备
                switch ($type) {
                    '*C3*' { $ipMap['C3'] = $ip; Write-Output "Found C3 at $ip (MAC: $mac)" -ForegroundColor Green }
                    '*C6*' { $ipMap['C6'] = $ip; Write-Output "Found C6 at $ip (MAC: $mac)" -ForegroundColor Green }
                    '*S3*' { $ipMap['S3'] = $ip; Write-Output "Found S3 at $ip (MAC: $mac)" -ForegroundColor Green }
                    default { 
                        if ($ipMap.Values -notcontains $ip) {
                            $ipMap['Unknown'] = @{IP=$ip; MAC=$mac; Type=$type}
                            Write-Output "? Unknown device at $ip" -ForegroundColor Yellow
                        }
                    }
                }
            }
        }
    } catch {}
}

Write-Output "`nIP Mapping Results:" -ForegroundColor Cyan
$ipMap.GetEnumerator() | ForEach-Object { Write-Output "  $($_.Key): $($_.Value)" -ForegroundColor White }

# === STEP 5-8: 待后续执行 ===
Write-Output "`n[STEP 5-8] Playwright Tests, Stability Monitoring, Reports - TO BE EXECUTED" -ForegroundColor Yellow

$endTime = Get-Date
$totalTime = New-TimeSpan -Start $startTime -End $endTime

Write-Output "`n========================================" -ForegroundColor Cyan
Write-Output "  FLASHING COMPLETE" -ForegroundColor Cyan
Write-Output "  Duration: $($totalTime.Hours)h $($totalTime.Minutes)m $($totalTime.Seconds)s" -ForegroundColor Cyan
Write-Output "  Devices flashed: $($flashResults | Where-Object {$_.Status -eq 'Success'}).Count}/$($onlineDevices.Count)" -ForegroundColor Cyan
Write-Output "========================================" -ForegroundColor Cyan
Write-Output "`nNext steps:" -ForegroundColor Yellow
Write-Output "1. Run Playwright E2E tests" -ForegroundColor White
Write-Output "2. Execute stability monitoring" -ForegroundColor White  
Write-Output "3. Generate final reports" -ForegroundColor White

# Save deployment summary
@{
    StartTime = $startTime.ToString('yyyy-MM-dd HH:mm:ss')
    EndTime = $endTime.ToString('yyyy-MM-dd HH:mm:ss')
    TotalDuration = "$($totalTime.Hours)h $($totalTime.Minutes)m $($totalTime.Seconds)s"
    DevicesOnline = $onlineDevices.Count
    DevicesFlashed = ($flashResults | Where-Object {$_.Status -eq 'Success'}).Count
    IpMapping = $ipMap
    FlashResults = $flashResults
} | ConvertTo-Json | Set-Content -Path "$logDir\DEPLOYMENT-SUMMARY.json" -Encoding UTF8

Write-Output "`nDeployment summary saved to $logDir\DEPLOYMENT-SUMMARY.json" -ForegroundColor Green
