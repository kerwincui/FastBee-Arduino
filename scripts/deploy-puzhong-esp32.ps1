# PowerShell Script: Puzhong ESP32 Deployment Automation
# Purpose: Automated firmware flashing and configuration import for Puzhong ESP32 dev board

param(
    [string]$Port = "COM3",
    [int]$Baud = 115200,
    [switch]$SkipBuild,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

$ProjectDir = "d:\project\gitee\FastBee-Arduino"
Set-Location $ProjectDir

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "普中 ESP32 开发板自动化部署工具 v1.0" -ForegroundColor Cyan
Write-Host "==================================================`n" -ForegroundColor Cyan

Write-Host "配置信息:" -ForegroundColor Yellow
Write-Host "  - 设备端口：$Port" -ForegroundColor Yellow
Write-Host "  - 波特率：$Baud" -ForegroundColor Yellow
Write-Host "  - 构建环境：esp32-F4R0" -ForegroundColor Yellow
Write-Host ""

# =====================
# Step 1: Build Firmware
# =====================
if (-not $SkipBuild) {
    Write-Host "[1/4] 构建固件..." -ForegroundColor Green
    
    if ($IsWindows) {
        & pio run -e esp32-F4R0
    } else {
        pio run -e esp32-F4R0
    }
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "✗ 固件构建失败"
        exit 1
    }
    
    Write-Host "✓ 固件构建完成" -ForegroundColor Green
    Write-Host ""
} else {
    Write-Host "[1/4] 跳过构建（使用已有固件）" -ForegroundColor Gray
    Write-Host ""
}

# =====================
# Step 2: Detect Device
# =====================
Write-Host "[2/4] 检测 ESP32 设备..." -ForegroundColor Green

$py = if ($IsWindows) {
    'C:\Users\kerwin\.platformio\penv\Scripts\python.exe'
} else {
    'python'
}

try {
    $result = & $py -m esptool `
        --chip esp32 `
        --port $Port `
        --baud $Baud `
        chip-id 2>&1 | Out-String
    
    if ($LASTEXITCODE -eq 0 -and $result -match "ESP32") {
        Write-Host "✓ 检测到 ESP32 芯片" -ForegroundColor Green
        Write-Host $result.Trim()
    } else {
        throw "无法检测到 ESP32 芯片或连接失败"
    }
} catch {
    Write-Error "✗ 设备检测失败：$_"
    Write-Host "`n建议检查:" -ForegroundColor Yellow
    Write-Host "  1. USB 线是否连接正常" -ForegroundColor Yellow
    Write-Host "  2. COM 端口号是否正确（可用 Get-SerialPort 查看）" -ForegroundColor Yellow
    Write-Host "  3. 是否需要按住 BOOT 键再重启" -ForegroundColor Yellow
    exit 1
}

# =====================
# Step 3: Flash Firmware
# =====================
if (-not $NoFlash) {
    Write-Host "`n[3/4] 烧录固件到 Flash..." -ForegroundColor Green
    
    # 擦除 NVS 分区（首次烧录或需要重置配置时）
    # & $py -m esptool -p $Port -b 921600 --chip esp32 erase_nvs
    
    # 烧录三个部分
    & $py -m esptool `
        -p $Port `
        -b 921600 `
        --chip esp32 `
        write_flash `
        -z `
        0x1000 .pio/build/esp32-F4R0/bootloader.bin `
        0x8000 .pio/build/esp32-F4R0/partition_table.bin `
        0x10000 .pio/build/esp32-F4R0/firmware.bin
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "✗ 固件烧录失败"
        exit 1
    }
    
    Write-Host "✓ 固件烧录完成" -ForegroundColor Green
    
    # 上传 LittleFS 文件系统
    Write-Host "`n  正在上传 LittleFS 文件系统..." -ForegroundColor Gray
    
    $fsFile = ".pio/build/esp32-F4R0/littlefs.bin"
    if (Test-Path $fsFile) {
        & $py -m esptool `
            -p $Port `
            -b 921600 `
            --chip esp32 `
            write_flash `
            0x200000 $fsFile
        
        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ LittleFS 文件系统上传完成" -ForegroundColor Green
        } else {
            Write-Warning "⚠ LittleFS 上传可能失败"
        }
    } else {
        Write-Info "⚠ LittleFS 文件不存在，跳过上传"
    }
    
    Write-Host ""
} else {
    Write-Host "[3/4] 跳过烧录（仅用于调试）" -ForegroundColor Gray
    Write-Host ""
}

# =====================
# Step 4: Open Monitor
# =====================
Write-Host "[4/4] 启动串口监视器..." -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "✅ 部署成功！请等待设备重启并观察日志..." -ForegroundColor Green
Write-Host "==================================================`n" -ForegroundColor Green

Write-Host "提示：按 Enter 键打开串口监视器（Ctrl+C 退出）" -ForegroundColor Yellow
$null = Read-Host

& pio device monitor -e esp32-F4R0 --port $Port --baud 115200
