# PowerShell Script: Multi-Device Deployment Automation
# Purpose: Deploy FastBee firmware to 4 ESP32 devices simultaneously

param(
    [switch]$DeployC6,      # ESP32-C6 on COM11
    [switch]$DeployS3,      # ESP32-S3 on COM15
    [switch]$DeployClassic, # ESP32 Classic on COM16
    [switch]$DeployC3,      # ESP32-C3 on COM4
    [switch]$AllDevices     # Deploy all devices
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

if (-not $DeployC6 -and -not $DeployS3 -and -not $DeployClassic -and -not $DeployC3 -and -not $AllDevices) {
    Write-Host "Usage: ./deploy-multi-device.ps1 [-AllDevices | -DeployC6 | -DeployS3 | -DeployClassic | -DeployC3]" -ForegroundColor Red
    exit 1
}

$ProjectDir = "d:\project\gitee\FastBee-Arduino"
Set-Location $ProjectDir

# Device Configuration Map
$Devices = @{
    "c6"   = @{ Port="COM11"; Env="esp32c6-F4R0"; Config="docs/ecm50a-full-config.json" }
    "s3"   = @{ Port="COM15"; Env="esp32s3-F8R4"; Config="docs/ecm50a-modbus-ethernet.json" }
    "classic" = @{ Port="COM16"; Env="esp32-F4R0"; Config="docs/puzhong-esp32-config.json" }
    "c3"   = @{ Port="COM4"; Env="esp32c3-F4R0"; Config="docs/esp32c3-deployment-config.json" }
}

# Determine which devices to deploy
$Targets = @()
if ($AllDevices) {
    $Targets += @("c6", "s3", "classic", "c3")
} else {
    if ($DeployC6) { $Targets += "c6" }
    if ($DeployS3) { $Targets += "s3" }
    if ($DeployClassic) { $Targets += "classic" }
    if ($DeployC3) { $Targets += "c3" }
}

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "FastBee 多设备并行部署工具 v1.0" -ForegroundColor Cyan
Write-Host "==================================================`n" -ForegroundColor Cyan

if ($Targets.Count -eq 0) {
    Write-Host "无设备待部署" -ForegroundColor Yellow
    exit 0
}

Write-Host "待部署设备列表:" -ForegroundColor Green
foreach ($Target in $Targets) {
    Write-Host "  - $($Devices[$Target].Env): COM$($Devices[$Target].Port)" -ForegroundColor Gray
}
Write-Host ""

# Build firmware in parallel (if needed)
Write-Host "[阶段 1] 构建固件 (并行)..." -ForegroundColor Cyan
$jobs = @()

foreach ($Target in $Targets) {
    $job = Start-Job -ScriptBlock {
        param($envName)
        Write-Host "正在构建 $envName..." -ForegroundColor Gray
        & pio run -e $envName
        if ($LASTEXITCODE -ne 0) { Write-Error "构建失败：$envName"; exit 1 }
        Write-Host "✓ 构建成功：$envName" -ForegroundColor Green
    } -ArgumentList $Devices[$Target].Env
    
    $jobs += $job
}

Wait-Job -Job $jobs | Out-Null
$buildStatus = Get-Job | Select-Object State
if ($buildStatus.State -contains "Failed") {
    Write-Error "部分设备固件构建失败，停止部署" -ForegroundColor Red
    Remove-Job -Job $jobs
    exit 1
}
Remove-Job -Job $jobs
Write-Host "[阶段 1] 完成 ✓`n" -ForegroundColor Green

# Flash firmware sequentially (USB port limitation)
$py = 'C:\Users\kerwin\.platformio\penv\Scripts\python.exe'

foreach ($Target in $Targets) {
    $deviceInfo = $Devices[$Target]
    
    Write-Host "`n==================================================" -ForegroundColor Cyan
    Write-Host "处理设备：$Target ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Cyan
    Write-Host "端口：COM$($deviceInfo.Port)" -ForegroundColor Yellow
    Write-Host "环境：$($deviceInfo.Env)" -ForegroundColor Yellow
    Write-Host "配置文件：$($deviceInfo.Config)" -ForegroundColor Yellow
    Write-Host "==================================================`n" -ForegroundColor Cyan
    
    # Step 1: Detect device
    Write-Host "[1/3] 检测 ESP32 设备..." -ForegroundColor Green
    
    try {
        $result = & $py -m esptool `
            --chip ($if ($Target -match "c6|c3") { "esp32c$([int]($Target[-1]))" } else { "esp32" }) `
            --port "COM$($deviceInfo.Port)" `
            --baud 921600 `
            chip-id 2>&1 | Out-String
        
        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ 检测到芯片：" -ForegroundColor Green -NoNewline
            Write-Host $result.Trim()
        } else {
            throw "设备检测失败"
        }
    } catch {
        Write-Error "✗ 无法连接到 COM$($deviceInfo.Port)，请检查连接" -ForegroundColor Red
        continue
    }
    
    # Step 2: Flash firmware
    Write-Host "`n[2/3] 烧录固件..." -ForegroundColor Green
    
    try {
        $firmwareBin = ".pio/build/$($deviceInfo.Env)/firmware.bin"
        $bootloaderBin = ".pio/build/$($deviceInfo.Env)/bootloader.bin"
        $partitionTable = ".pio/build/$($deviceInfo.Env)/partition_table.bin"
        $littlefsBin = ".pio/build/$($deviceInfo.Env)/littlefs.bin"
        
        if (-not (Test-Path $firmwareBin)) {
            Write-Error "固件文件不存在：$firmwareBin" -ForegroundColor Red
            continue
        }
        
        # 擦除 Flash (首次部署或完全重置时)
        & $py -m esptool `
            -p "COM$($deviceInfo.Port)" `
            -b 921600 `
            --chip ($if ($Target -match "c6|c3") { "esp32c$([int]($target[-1]))" } else { "esp32" }) `
            erase_flash
        
        # 烧录三个部分
        & $py -m esptool `
            -p "COM$($deviceInfo.Port)" `
            -b 921600 `
            --chip ($if ($Target -match "c6|c3") { "esp32c$([int]($target[-1]))" } else { "esp32" }) `
            write_flash `
            -z `
            0x1000 $bootloaderBin `
            0x8000 $partitionTable `
            0x10000 $firmwareBin
        
        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ 固件烧录成功" -ForegroundColor Green
        } else {
            Write-Error "固件烧录失败" -ForegroundColor Red
            continue
        }
        
        # 上传 LittleFS
        if (Test-Path $littlefsBin) {
            Write-Host "  上传 LittleFS 文件系统..." -ForegroundColor Gray
            & $py -m esptool `
                -p "COM$($deviceInfo.Port)" `
                -b 921600 `
                --chip ($if ($Target -match "c6|c3") { "esp32c$([int]($target[-1]))" } else { "esp32" }) `
                write_flash `
                0x200000 $littlefsBin
            
            if ($LASTEXITCODE -eq 0) {
                Write-Host "✓ LittleFS 上传成功" -ForegroundColor Green
            }
        }
        
        Write-Host ""
    } catch {
        Write-Error "✗ 烧录过程出错：$_" -ForegroundColor Red
        continue
    }
    
    # Step 3: Wait for device to boot and configure
    Write-Host "[3/3] 等待设备启动并验证..." -ForegroundColor Green
    
    # 等待 10 秒让设备重启
    Start-Sleep -Seconds 10
    
    # 尝试访问 Web 界面
    $ipAddress = Try-EquipWebAccess -Port $deviceInfo.Port -Timeout 15
    
    if ($ipAddress) {
        Write-Host "✓ 设备在线，IP 地址：$ipAddress" -ForegroundColor Green
        Write-Host "  提示：使用以下命令导入配置文件:" -ForegroundColor Cyan
        Write-Host "  curl -X PUT http://$ipAddress/api/config/import \` -ForegroundColor Gray
        Write-Host "    -H \"Authorization: Bearer \$TOKEN\" \` -ForegroundColor Gray
        Write-Host "    -d @'$($deviceInfo.Config)'`n" -ForegroundColor Gray
        
        # 打开串口监视器 (可选)
        $answer = Read-Host "是否打开串口监视器？(Y/N)"
        if ($answer -eq "Y" -or $answer -eq "y") {
            pio device monitor -e $deviceInfo.Env --port "COM$($deviceInfo.Port)" --baud 115200
        }
    } else {
        Write-Warning "⚠ 无法自动获取 IP 地址，请手动登录 Web 界面完成配置导入" -ForegroundColor Yellow
        Write-Host "  提示：在浏览器中访问 http://192.168.x.$($deviceInfo.Port.Substring(2)) 或 http://$(Get-MdnsName $Target).local`n" -ForegroundColor Yellow
    }
    
    Write-Host "`n[$Target] 部署完成" -ForegroundColor Green
}

Write-Host "`n==================================================" -ForegroundColor Green
Write-Host "✅ 所有设备的部署流程已完成!" -ForegroundColor Green
Write-Host "==================================================`n" -ForegroundColor Green

Write-Host "下一步操作:" -ForegroundColor Cyan
Write-Host "1. 登录每台设备的 Web 管理界面 (http://<IP>)" -ForegroundColor White
Write-Host "2. 导入对应的配置文件 (docs/*.json)" -ForegroundColor White
Write-Host "3. 验证外设功能 (OLED/继电器/AI-DI/Modbus)" -ForegroundColor White
Write-Host "4. 运行长期稳定性测试 (>24h)" -ForegroundColor White
