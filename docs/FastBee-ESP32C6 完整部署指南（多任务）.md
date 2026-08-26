# FastBee-ESP32C6 设备完整部署指南（多任务执行）

## 📋 目录
1. [项目背景](#项目背景)
2. [硬件准备](#硬件准备)
3. [固件构建与烧录](#固件构建与烧录)
4. [配置导入](#配置导入)
5. [功能验证](#功能验证)
6. [性能测试](#性能测试)
7. [结果报告](#结果报告)

---

## 🎯 项目背景

本次部署目标是基于 **FastBee-Arduino** 框架，为 **ESP32-C6** 开发板完成从固件烧录到功能验证的完整部署流程。使用配置文件 `所有配置.json` 中的 JSON 数据，通过 REST API 或 Web 界面批量导入到设备的 `/config/` 目录下。

### 核心目标
- ✓ 固件刷写与配置导入
- ✓ 外设配置启用（OLED 显示屏、LED 控制）
- ✓ MQTT 通信协议配置
- ✓ 执行规则自动触发
- ✓ 完整的测试验证

### 参考文档
- 设备端文档：`device/examples/esp32c6-blink-quickstart.md`
- 平台支持：[esp32c6-F4R0](../platformio.ini) 环境（原 file:// 绝对路径已改为仓库相对路径）
- GPIO 限制：ESP32-C6 可用 GPIO 0-23（GPIO 24-30 为封装内部接口）

---

## 🔧 硬件准备

### ESP32-C6 开发板规格
- **芯片型号**: ESP32-C6 (RISC-V Wi-Fi + BLE 双模)
- **Flash**: 4MB（推荐 FH4 版本集成 Flash）
- **PSRAM**: 可选（8MB PS8AM 版本）
- **GPIO**: 支持 GPIO 0-23（共 24 个可用引脚）
- **WiFi**: IEEE 802.11 b/g/n (2.4 GHz)
- **BLE**: Bluetooth 5.0 / LE

### 外围设备接线表

#### OLED 显示屏 (I2C 接口 - SSD1306)
```
OLED VCC ──────────── 3.3V (ESP32-C6)     # 电源正极
OLED GND ──────────── GND                  # 地线
OLED SDA ──────────── GPIO21               # 数据引脚（I2C 数据线）
OLED SCL ──────────── GPIO5                # 时钟引脚（I2C 时钟线）
```

**注意事项**:
- I2C 频率：400kHz（高速模式）
- I2C 地址：0x03 或 0x3C（根据模块不同）
- 建议添加 4.7kΩ上拉电阻（部分模块已内置）

#### RGB LED 指示灯
```
RGB LED R(红) ──────── GPIO19 + 220Ω电阻
RGB LED G(绿) ──────── 未使用或 PWM 调速
RGB LED B(蓝) ──────── 未使用或 PWM 调速
RGB LED GND ────────── GND
```

**替代方案**: 如果使用单色 LED
```
LED (+) ────────────── GPIO19 + 220Ω限流电阻
LED (-) ────────────── GND
```

### ESP32-C6 GPIO 映射表

| 功能 | GPIO 编号 | ESP32-C6 状态 | 备注 |
|------|----------|-------------|------|
| OLED SDA | GPIO21 | ✓ 可用 | I2C 数据线 |
| OLED SCL | GPIO5 | ✓ 可用 | I2C 时钟线 |
| RGB LED | GPIO19 | ✓ 可用 | 数字输出/PWM |
| WiFi 天线 | GPIO20 | ⚠️ 保留 | RF 信号引脚 |
| USB D+ | GPIO13 | ⚠️ 保留 | 内置 USB |
| USB D- | GPIO12 | ⚠️ 保留 | 内置 USB |
| UART0 TX | GPIO17 | ⚠️ 保留 | 控制台串口 |
| UART0 RX | GPIO16 | ⚠️ 保留 | 控制台串口 |

**重要**: 不要使用 GPIO 12-17 和 24-30，这些引脚被内部功能占用！

---

## 🏗️ 固件构建与烧录

### 步骤 1: 环境检查

```powershell
# PowerShell 命令（Windows 环境）
cd d:\project\gitee\FastBee-Arduino

# 检查 PlatformIO 安装
pio --version  # 应返回 >= 6.0

# 检查 esptool 安装
python -m esptool --version

# 设置编码环境变量（解决中文乱码问题）
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
```

### 步骤 2: 构建固件

```powershell
# 构建 esp32c6-F4R0 环境固件（4MB Flash 版本）
pio run -e esp32c6-F4R0

# 或者使用后台编译（不阻塞终端）
pio build -e esp32c6-F4R0 --backend keepalive
```

**构建产物位置**:
```
.pio/build/esp32c6-F4R0/
├── firmware.bin          # 主固件镜像
├── bootloader.bin        # Bootloader
├── partition_table.bin   # 分区表
└── littlefs.bin          # LittleFS 文件系统
```

### 步骤 3: 自动部署脚本

**方案 A: 使用现有脚本（推荐）**

项目中已提供 ESP32-C6 连接检测脚本，但需稍作修改（因为原脚本是针对 C3 的）:

```powershell
# 创建 C6 版本的连接脚本
New-Item -Path ".\scripts\c6-connect-loop.ps1" -ItemType File -Force

$py = 'C:\Users\kerwin\.platformio\penv\Scripts\python.exe'
$port = "COM3"  # 替换为您的实际端口号
$baud = 921600
$timeout = [TimeSpan]::FromMinutes(10)

Start-Sleep -Seconds 2

Write-Host "等待 ESP32-C6 连接..." -ForegroundColor Yellow
$startTime = Get-Date

while ((Get-Date) -lt $startTime.Add($timeout)) {
    try {
        $result = & $py -m esptool `
            --chip esp32c6 `
            --port $port `
            --baud $baud `
            --connect-attempts 3 `
            chip-id 2>&1
        
        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ CONNECTED! ESP32-C6 detected on $port" -ForegroundColor Green
            Write-Host $result
            
            # 开始自动部署
            Write-Host "`n==> 开始部署固件..." -ForegroundColor Cyan
            .\scripts\deploy-all.ps1 -Env esp32c6-F4R0 -Ports $port
            
            exit 0
        }
    } catch {
        # 静默失败，继续轮询
    }
    
    Write-Host "[$((Get-Date).ToString('HH:mm:ss'))] 无响应，重试中..." -ForegroundColor Gray
    Start-Sleep -Seconds 8
}

Write-Error "✗ TIMEOUT: ESP32-C6 no response on $port" -ErrorAction Stop
exit 1
```

执行方式:
```powershell
.\scripts\c6-connect-loop.ps1
```

此脚本会：
1. 每 8 秒轮询检测 COM 端口
2. 确认 ESP32-C6 芯片响应
3. 自动执行完整部署（build + flash + uploadfs）

**方案 B: 手动烧录**

```powershell
# 查找串口端口
Get-SerialPort | Where-Object {$_.DeviceId -like "COM*"}

# 擦除 Flash 并烧录固件（如果首次烧录或需要完全清空）
python -m esptool -p COMx -b 921600 --chip esp32c6 erase_flash

# 烧录三个部分（bootloader, firmware, littlefs）
python -m esptool -p COMx -b 921600 --chip esp32c6 write_flash \
  0x0 .pio/build/esp32c6-F4R0/bootloader.bin \
  0x10000 .pio/build/esp32c6-F4R0/firmware.bin \
  0x200000 .pio/build/esp32c6-F4R0/littlefs.bin

# COMx 替换为实际端口号（如 COM3）
```

### 步骤 4: 查看启动日志

```powershell
# 以 115200 波特率打开串口监视器（调试输出默认波特率）
pio device monitor -e esp32c6-F4R0 --port COMx --baud 115200

# 或使用 VS Code 自带的 Serial Monitor 插件
```

**预期完整启动日志**:
```
============================================
FastBee Framework v1.0.0 (Build: 2026-08-20)
Hardware: ESP32-C6, CPU Freq: 160MHz, Flash: 4MB
Free Heap: 325000 bytes, Largest Block: 280000 bytes
Developer Mode: ENABLED
============================================

[INFO] Peripheral Initialization Started...
[INFO] Peripheral Manager: Adding 'oled_display' (LCD, I2C)
[INFO] Peripheral Manager: LCD 'oled_display' initialized 128x64 I2C freq=400000Hz addr=0x03 contrast=255
[INFO] GPIO Pins: SDA=21, SCL=5
[INFO] U8G2 object created successfully
[INFO] Display ready for rendering
[INFO] Peripheral Manager: Adding 'led_indicator' (GPIO_DIGITAL_OUTPUT, GPIO19)
[INFO] Peripheral Manager: GPIO 'led_indicator' initialized pin=19 state=LOW
[INFO] All 2 peripherals initialized successfully
[INFO]
[INFO] Network Initialization Started...
[INFO] WiFi Station mode enabled, SSID: your_wifi_ssid
[INFO] WiFi connected! RSSI: -68 dBm
[INFO] IP Address: 192.168.1.105
[INFO] MDNS: fastbee-c6-001.local available
[INFO]
[INFO] Time Synchronization...
[INFO] NTP Server: http://iot.fastbee.cn/prod-api/iot/tool/ntp
[INFO] Time synced: 2026-08-20 14:30:45 CST-8
[INFO]
[INFO] MQTT Connection...
[INFO] MQTT Client ID: FBE_C6_001
[INFO] Connecting to iot.fastbee.cn:1883...
[INFO] MQTT Connected! Subscribed to: fb/iot/FBE_C6_001/cmd
[INFO]
[INFO] Web Server Started
[INFO] Local URL: http://192.168.1.105
[INFO] MDNS URL: http://fastbee-c6-001.local
[INFO]
[INFO] System Ready! All services running normally.
[INFO] ============================================
```

---

## ⚙️ 配置导入

### 配置文件结构分析

从配置文件 `所有配置.json` 分离出以下文件：

```
/config/
├── device.json      # 设备基本信息
├── network.json     # 网络配置  
├── peripherals.json # 外设配置（2 个）
├── periph_exec.json # 执行规则（3 条）
└── protocol.json    # 通信协议
```

### 方法一：Web 界面批量导入（推荐）

#### 步骤 1: 访问设备 Web 界面
```
http://192.168.x.x  # 从串口日志获取实际 IP
# 或使用 mDNS 域名
http://fastbee-c6-001.local
```

#### 步骤 2: 登录管理后台
```
用户名：admin
密码：admin  # 默认凭据，首次登录后建议修改
```

#### 步骤 3: 导入配置包
1. 导航至：**设备配置 → 配置导入/导出**
2. 选择文件：`docs/esp32c6-deployment-config.json`
3. 点击按钮：**"导入全部配置"**

#### 步骤 4: 验证配置内容
导入后请核对以下内容：

```
✅ Device Config: FBE_C6_001
   ├─ 设备名称：ESP32-C6 OLED 智能显示设备
   ├─ NTP 服务器：http://iot.fastbee.cn/prod-api/iot/tool/ntp
   └─ 时区：CST-8

✅ Network Config: 
   ├─ WiFi 模式：station
   ├─ AP SSID: FastBee-C6-001
   └─ 主机名：fastbee-c6-001

✅ Peripherals: 2 个
   ├─ oled_display (LCD, I2C, GPIO5/21)
   └─ led_indicator (GPIO_DIGITAL_OUTPUT, GPIO19)

✅ Periph Exec: 3 条规则
   ├─ oled_system_status (5 秒周期更新)
   ├─ led_blink_pattern (事件触发)
   └─ periodic_data_upload (60 秒周期上报)

✅ Protocol: 
   ├─ Publish: fb/iot/FBE_C6_001/data
   └─ Subscribe: fb/iot/FBE_C6_001/cmd
```

### 方法二：REST API 逐个导入

如果 Web 界面不可用，可以使用 REST API 逐文件导入。

#### 前置步骤：获取认证 Token

```bash
# POST /api/auth/login
curl -X POST http://192.168.x.x/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{
    "username": "admin",
    "password": "admin"
  }'

# 响应示例
{
  "success": true,
  "data": {
    "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
    "userId": "1",
    "username": "admin"
  }
}
```

保存 `token`，后续所有请求需在 Header 中添加：
```
Authorization: Bearer <your_token_here>
```

#### 导入 device.json（设备基本信息）

```bash
curl -X PUT http://192.168.x.x/api/device/config \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "deviceId": "FBE_C6_001",
    "deviceName": "ESP32-C6 OLED 智能显示设备",
    "productNumber": 1,
    "userId": "1",
    "location": "实验室 B 区",
    "description": "基于 ESP32-C6 的 OLED 显示与 LED 控制设备",
    "enableNTP": true,
    "ntpServer1": "http://iot.fastbee.cn/prod-api/iot/tool/ntp",
    "ntpServer2": "cn.pool.ntp.org",
    "timezone": "CST-8",
    "syncInterval": 3600,
    "logLevel": "INFO",
    "developerModeEnabled": true
  }'
```

#### 导入 network.json（网络配置）

```bash
curl -X PUT http://192.168.x.x/api/network/config \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "wifiMode": "station",
    "wifiSSID": "your_wifi_ssid",
    "wifiPassword": "your_wifi_password",
    "apSSID": "FastBee-C6-001",
    "apPassword": "12345678",
    "hostname": "fastbee-c6-001"
  }'
```

**注意**: 此操作会立即重启 WiFi 连接，可能导致短暂的 Web 服务中断。

#### 导入 peripherals.json（外设配置）

需要**分别 POST**每个外设：

```bash
# 1. 添加 OLED 显示屏
curl -X POST http://192.168.x.x/api/peripherals \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "oled_display",
    "name": "0.96 英寸 OLED 显示屏",
    "type": "LCD",
    "enabled": true,
    "pinCount": 2,
    "pins": [5, 21],
    "params": {
      "width": 128,
      "height": 64,
      "interface": 2,
      "frequency": 400000,
      "address": 3,
      "contrast": 255
    }
  }'

# 2. 添加 LED 指示灯
curl -X POST http://192.168.x.x/api/peripherals \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "led_indicator",
    "name": "RGB LED 指示灯",
    "type": "GPIO_DIGITAL_OUTPUT",
    "enabled": true,
    "pinCount": 1,
    "pins": [19],
    "params": {
      "initialState": 0,
      "passive": false
    }
  }'
```

#### 导入 periph_exec.json（执行规则）

同样需要**分别 POST**每条规则：

```bash
# 规则 1: 系统状态实时显示
curl -X POST http://192.168.x.x/api/periph-exec \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "oled_system_status",
    "name": "系统状态实时显示",
    "enabled": true,
    "triggerType": "interval",
    "interval": 5000,
    "actions": [
      {
        "type": "updateDisplay",
        "peripheralId": "oled_display",
        "params": {
          "template": "WiFi:Connected|Temp:--°C Time:{time}",
          "formatFunc": "formatSystemStatus"
        }
      }
    ]
  }'

# 规则 2: LED 闪烁模式指示
curl -X POST http://192.168.x.x/api/periph-exec \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "led_blink_pattern",
    "name": "LED 闪烁模式指示",
    "enabled": true,
    "triggerType": "event",
    "eventPattern": "system_event",
    "actions": [
      {
        "type": "blinkLed",
        "peripheralId": "led_indicator",
        "params": {
          "blinks": 3,
          "interval": 200
        }
      }
    ]
  }'

# 规则 3: 周期性数据上报
curl -X POST http://192.168.x.x/api/periph-exec \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "periodic_data_upload",
    "name": "周期性数据上报",
    "enabled": true,
    "triggerType": "interval",
    "interval": 60000,
    "actions": [
      {
        "type": "publishData",
        "params": {
          "topic": "fb/iot/FBE_C6_001/data",
          "payloadTemplate": "{\"deviceId\":\"FBE_C6_001\",\"timestamp\":{{timestamp}},\"uptime\":{{uptime}},\"freeHeap\":{{heap}}}"
        }
      }
    ]
  }'
```

#### 导入 protocol.json（通信协议配置）

```bash
curl -X PUT http://192.168.x.x/api/protocol/config \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "version": "1.0",
    "dataFormat": "json",
    "publishTopics": ["fb/iot/FBE_C6_001/data"],
    "subscribeTopics": ["fb/iot/FBE_C6_001/cmd"]
  }'
```

### 配置参数详解

#### 1. OLED 显示屏 (I2C 接口)
- **控制器类型**: SSD1306（128x64）或 SH1106
- **接口模式**: I2C (interface=2)
- **GPIO 分配**: 
  - SDA = GPIO21（ESP32-C6 可用 I2C 数据线）
  - SCL = GPIO5（ESP32-C6 可用 I2C 时钟线）
- **频率**: 400kHz（高速 I2C 模式）
- **地址**: 0x03（根据模块实际情况可能为 0x3C）
- **对比度**: 255（最大值，确保最佳可视性）

**驱动实现**: 
- 使用 U8g2 库进行硬件抽象
- 全缓冲模式（约 1KB 内存占用），性能最优
- 支持刷新节流以避免闪烁

#### 2. RGB LED 控制
- **GPIO 引脚**: GPIO19（ESP32-C6 通用 GPIO）
- **工作模式**: 数字输出（可升级为 PWM 调光）
- **初始状态**: LOW（关闭）
- **电流保护**: 必须串联 220Ω限流电阻

#### 3. MQTT 配置
- **服务器**: `iot.fastbee.cn:1883`（生产环境标准端口）
- **clientId**: `FBE_C6_001`（设备唯一标识）
- **QoS**: Level 1（至少一次送达）
- **Keep Alive**: 60 秒（心跳间隔）
- **自动重连**: 启用（断开后指数退避重连）
- **发布主题**: `fb/iot/FBE_C6_001/data`
- **订阅主题**: `fb/iot/FBE_C6_001/cmd`

#### 4. 执行规则说明

##### 规则 1: 系统状态实时显示 (`oled_system_status`)
- **触发类型**: 定时器（每 5 秒）
- **动作**: 更新 OLED 显示
- **显示内容**:
  ```
  Line1: WiFi:Connected (或 Disconnected)
  Line2: Temp:--°C Time:14:30:45
  ```
- **优点**: 低延迟、高频更新、视觉反馈实时

##### 规则 2: LED 闪烁模式指示 (`led_blink_pattern`)
- **触发类型**: 事件驱动（system_event 事件）
- **动作**: LED 闪烁 3 次，间隔 200ms
- **应用场景**: 系统启动、配置变更、异常告警

##### 规则 3: 周期性数据上报 (`periodic_data_upload`)
- **触发类型**: 定时器（每 60 秒）
- **动作**: 发布 MQTT 消息
- **数据内容**:
  ```json
  {
    "deviceId": "FBE_C6_001",
    "timestamp": 1692526245,
    "uptime": 86400000,
    "freeHeap": 320000
  }
  ```
- **用途**: 监控设备运行状态、内存健康度

---

## ✅ 功能验证

### 验证 1: OLED 显示功能

#### 测试方法 A: Web 诊断页面
1. **访问诊断界面**
   ```
   http://192.168.x.x/#/diagnostics/peripherals?tab=oled_display
   ```

2. **执行显示测试**
   ```bash
   curl -X POST http://192.168.x.x/api/periph-exec/test \
     -H "Authorization: Bearer YOUR_TOKEN" \
     -H "Content-Type: application/json" \
     -d '{
       "peripheralId": "oled_display",
       "action": "displayText",
       "params": {"text": "TEST OK"}
     }'
   ```

#### 测试方法 B: MQTT 命令触发
```bash
# 发布文本显示指令
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/cmd" \
  -m '{
    "command": "oled_text",
    "parameters": {
      "text": "Hello from MQTT",
      "x": 0,
      "y": 0
    }
  }'
```

#### 验证标准
- [ ] OLED 屏幕正常点亮，背光清晰
- [ ] 显示字符无抖动、无乱码
- [ ] 能够实时更新温湿度和时间信息
- [ ] 显示格式符合模板：`WiFi:Connected|Temp:--°C Time:XX:XX:XX`

---

### 验证 2: LED 控制功能

#### 测试方法 A: HTTP API 直接控制
```bash
# 开启 LED
curl -X POST http://192.168.x.x/api/peripherals/led_indicator/write \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"state": 1}'

# 关闭 LED
curl -X POST http://192.168.x.x/api/peripherals/led_indicator/write \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"state": 0}'
```

#### 测试方法 B: MQTT 命令控制
```bash
# 开启 LED（通过规则引擎）
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/cmd" \
  -m '{
    "command": "led 开关",
    "parameters": {"state": 1}
  }'

# 关闭 LED
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/cmd" \
  -m '{
    "command": "led 开关",
    "parameters": {"state": 0}
  }'
```

#### 验证标准
- [ ] LED 能够正常亮灭切换
- [ ] 响应延迟 < 500ms（从发送指令到 LED 变化）
- [ ] 状态切换平滑无卡顿
- [ ] 长时间保持稳定（不闪烁、不误触发）

---

### 验证 3: MQTT 通信测试

#### 测试命令 A: 使用 Mosquitto 工具

```bash
# 1. 订阅设备发布的数据
mosquitto_sub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/data" -v

# 2. 发布测试消息到云端
echo '{"test":"connectivity_check","timestamp":$(date +%s)}' | \
  mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/data"

# 3. 订阅设备可接收的控制指令
mosquitto_sub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/cmd" -v

# 4. 下发控制命令
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C6_001/cmd" \
  -m '{
    "command": "oled_clear",
    "parameters": {}
  }'
```

#### 测试命令 B: 使用 Web 浏览器测试
```javascript
// 在浏览器控制台执行
async function testMQTT() {
  // 模拟发布消息
  const msg = {
    deviceId: 'FBE_C6_001',
    timestamp: Date.now(),
    test: 'web_browser_test'
  };
  
  console.log('发送消息:', msg);
  // 注意：实际需要通过设备发布到 MQTT broker
}

testMQTT();
```

#### 成功标准
- [ ] 设备能成功发布数据到云端 broker
- [ ] 云端下发的指令能被设备实时接收
- [ ] MQTT 连接稳定不掉线（断网后自动重连）
- [ ] clientId 不冲突（同一时间只在线一个设备实例）

---

## 📊 性能测试

### 测试 1: Web 访问性能

#### 测试脚本

```html
<!-- 在 Chrome 开发者控制台执行 -->
<script>
async function testWebPerformance() {
  const url = 'http://192.168.x.x';
  const tests = [];
  
  // 测试首页加载时间
  let start = performance.now();
  await fetch(url);
  let end = performance.now();
  tests.push({
    name: '首页加载',
    duration: Math.round(end - start),
    unit: 'ms',
    target: 500,
    status: (end - start) < 500 ? 'PASS' : 'FAIL'
  });
  
  // 测试资源配置
  start = performance.now();
  await fetch(`${url}/api/peripherals/list`);
  end = performance.now();
  tests.push({
    name: 'API 响应',
    duration: Math.round(end - start),
    unit: 'ms',
    target: 300,
    status: (end - start) < 300 ? 'PASS' : 'FAIL'
  });
  
  console.table(tests);
  return tests;
}

testWebPerformance().then(console.log);
</script>
```

#### 性能指标表

| 测试项目 | 目标值 | 实测值 | 单位 | 状态 |
|---------|--------|--------|------|------|
| 首页响应时间 | < 500ms | TBD | ms | □PASS □FAIL |
| API 响应时间 | < 300ms | TBD | ms | □PASS □FAIL |
| CSS/JS 加载时间 | < 200ms | TBD | ms | □PASS □FAIL |
| 资源压缩率 | > 70% | TBD | % | □PASS □FAIL |

---

### 测试 2: MQTT 消息传输成功率

#### 自动化测试脚本

```bash
#!/bin/bash
# mqtt_reliability_test.sh

TARGET_TOPIC="fb/iot/FBE_C6_001/data"
MESSAGES=1000
SUCCESS=0
FAILURE=0

echo "=================================================="
echo "MQTT 消息可靠性测试 - 1000 条消息"
echo "目标主题：$TARGET_TOPIC"
echo "=================================================="

for i in $(seq 1 $MESSAGES); do
  # 发布消息
  mosquitto_pub -h iot.fastbee.cn -p 1883 \
    -t "$TARGET_TOPIC" \
    -m "{\"seq\":$i,\"timestamp\":$(date +%s),\"test_id\":\"FBE_C6_001\"}" > /dev/null 2>&1
  
  if [ $? -eq 0 ]; then
    SUCCESS=$((SUCCESS + 1))
  else
    FAILURE=$((FAILURE + 1))
  fi
  
  # 每 100 条打印进度
  if [ $((i % 100)) -eq 0 ]; then
    echo "进度：$i/$MESSAGES | 成功：$SUCCESS | 失败：$FAILURE"
  fi
done

# 计算成功率
if [ $MESSAGES -gt 0 ]; then
  SUCCESS_RATE=$(awk "BEGIN {printf \"%.3f\", ($SUCCESS/$MESSAGES)*100}")
else
  SUCCESS_RATE="0.000"
fi

echo ""
echo "=================================================="
echo "测试结果汇总:"
echo "=================================================="
echo "发送总数：$MESSAGES"
echo "成功数：$SUCCESS"
echo "失败数：$FAILURE"
echo "成功率：${SUCCESS_RATE}%"
echo "=================================================="

# 判断是否达标
if awk "BEGIN {exit !($SUCCESS_RATE >= 99.9)}"; then
  echo "🎉 达标！成功率 ≥ 99.9%"
else
  echo "❌ 未达标！成功率 < 99.9%"
fi
echo "=================================================="
```

**执行方式**:
```bash
chmod +x mqtt_reliability_test.sh
./mqtt_reliability_test.sh
```

**期望结果**:
```
成功率：99.95%  ✅ 达标
```

---

### 测试 3: 内存稳定性监控（24 小时）

#### 持续监控脚本

```bash
#!/bin/bash
# memory_monitoring.sh

DEVICE_IP="192.168.x.x"
TOKEN="YOUR_AUTH_TOKEN"
OUTPUT_FILE="memory_monitoring_log.csv"

# 创建 CSV 头文件
echo "Timestamp,FreeHeap,Uptime,LoopNumber" > $OUTPUT_FILE

echo "开始 24 小时内存监控测试..."
echo "设备 IP: $DEVICE_IP"
echo "-------------------------------------------"

TOTAL_LOOPS=1440  # 24 小时 * 60 分钟 * 60 秒 / 60 秒间隔 = 1440 次

for i in $(seq 1 $TOTAL_LOOPS); do
  # 查询系统健康状态
  RESPONSE=$(curl -s -o /dev/null -w "%{http_code}" \
    --max-time 10 \
    "http://$DEVICE_IP/api/system/health" \
    -H "Authorization: Bearer $TOKEN")
  
  if [ "$RESPONSE" -eq 200 ]; then
    # 解析 freeHeap 和 uptime
    FREE_HEAP=$(curl -s \
      "http://$DEVICE_IP/api/system/health" \
      -H "Authorization: Bearer $TOKEN" \
      | jq -r '.data.freeHeap')
    
    UPTIME=$(curl -s \
      "http://$DEVICE_IP/api/system/health" \
      -H "Authorization: Bearer $TOKEN" \
      | jq -r '.data.uptime')
    
    TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
    
    # 记录到 CSV
    echo "$TIMESTAMP,$FREE_HEAP,$UPTIME,$i" >> $OUTPUT_FILE
    
    # 每 100 次打印一次统计
    if [ $((i % 100)) -eq 0 ]; then
      echo "[$i/$TOTAL_LOOPS] Free Heap: ${FREE_HEAP} bytes, Uptime: ${UPTIME}s"
    fi
  else
    echo "[$i] HTTP Error: $RESPONSE" >> $OUTPUT_FILE
  fi
  
  # 休眠 59 秒（避免请求过频）
  sleep 59
done

echo ""
echo "监控完成！数据已保存到：$OUTPUT_FILE"

# 生成统计摘要
echo "=================================================="
echo "统计分析:"
echo "=================================================="
echo "最大堆内存：$(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | sort -n | tail -1) bytes"
echo "最小堆内存：$(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | sort -n | head -1) bytes"
echo "平均堆内存：$(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | awk '{sum+=$1} END {print int(sum/NR)}') bytes"
echo "波动范围：$(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | sort -n | tail -1) - $(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | sort -n | head -1) = $(($(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | sort -n | tail -1) - $(tail -n +2 $OUTPUT_FILE | cut -d',' -f2 | sort -n | head -1))) bytes"
echo "=================================================="
```

**执行方式**:
```bash
chmod +x memory_monitoring.sh
./memory_monitoring.sh
```

**监控数据表示例**:

| 时间点 | 空闲堆内存 (bytes) | 运行时长 (秒) | MQTT 状态 | 备注 |
|--------|-------------------|--------------|-----------|------|
| 00:00:00 | 325000 | 0 | Connected | 设备启动 |
| 01:00:00 | 320500 | 3600 | Connected | 运行 1h |
| 02:00:00 | 319800 | 7200 | Connected | 运行 2h |
| ... | ... | ... | ... | ... |
| 24:00:00 | 315000 | 86400 | Connected | 运行 24h |

**判定标准**:
- [ ] 空闲堆内存波动 < 10%（相对于初始值）
- [ ] 无内存泄漏迹象（非单调下降）
- [ ] 无复位或重启事件发生
- [ ] 所有外设持续正常工作
- [ ] MQTT 连接稳定不中断

---

### 测试 4: NTP 时间同步验证

#### 验证命令

```bash
# 1. 查询设备时间状态
curl -s http://192.168.x.x/api/device/time \
  -H "Authorization: Bearer YOUR_TOKEN" | jq .

# 期望响应
{
  "success": true,
  "data": {
    "datetime": "2026-08-20 14:30:45",
    "timestamp": 1692526245,
    "synced": true,
    "timeValid": true,
    "internetAvailable": true,
    "uptime": 86400000,
    "timezone": "CST-8"
  }
}
```

#### 验证要点
- [ ] NTP 服务器连接成功（`synced=true`）
- [ ] 时间戳准确（与当前系统时间误差 < 1 秒）
- [ ] 时区正确设置为 CST-8（中国标准时间）
- [ ] 同步间隔为 3600 秒（1 小时）
- [ ] 设备重启后仍能正常同步

---

## 📝 结果报告模板

### 部署完成确认清单

**基础部署**
- [ ] **固件烧录**: ESP32-C6 固件已成功写入 Flash（4MB）
- [ ] **文件系统**: LittleFS 文件系统已挂载并验证
- [ ] **网络配置**: WiFi Station 模式成功连接 AP
- [ ] **设备识别**: mDNS 域名（fastbee-c6-001.local）可解析

**外设验证**
- [ ] **OLED 显示屏**: 
  - [ ] I2C 接口初始化成功（GPIO5/21）
  - [ ] SSD1306 控制器识别正常
  - [ ] 显示内容清晰无闪烁
  - [ ] 分辨率匹配（128x64）
- [ ] **LED 控制**: 
  - [ ] GPIO19 数字输出正常
  - [ ] 亮灭切换响应迅速
  - [ ] 无异常发热现象

**通信测试**
- [ ] **MQTT 连接**: 成功连接到 iot.fastbee.cn:1883
- [ ] **主题订阅**: 正确订阅 fb/iot/FBE_C6_001/cmd
- [ ] **数据发布**: 能够发布数据到 fb/iot/FBE_C6_001/data
- [ ] **客户端 ID**: 唯一且无冲突

**配置导入**
- [ ] **device.json**: 设备基本信息已导入
- [ ] **network.json**: WiFi 和网络配置已生效
- [ ] **peripherals.json**: 2 个外设配置已激活
- [ ] **periph_exec.json**: 3 条执行规则已启用
- [ ] **protocol.json**: MQTT 主题配置已完成

---

### 性能测试数据汇总

| 测试项目 | 目标指标 | 实测结果 | 单位 | 状态 |
|---------|---------|---------|------|------|
| Web 访问延迟 | < 500ms | TBD | ms | □PASS □FAIL |
| API 响应时间 | < 300ms | TBD | ms | □PASS □FAIL |
| MQTT 传输成功率 | ≥ 99.9% | TBD | % | □PASS □FAIL |
| OLED 显示刷新 | 正常更新 | TBD | - | □PASS □FAIL |
| LED 控制响应 | < 500ms | TBD | ms | □PASS □FAIL |
| 24h 内存波动 | < 10% | TBD | % | □PASS □FAIL |
| NTP 时间同步 | 误差 < 1s | TBD | s | □PASS □FAIL |
| 时区设置 | CST-8 | TBD | - | □PASS □FAIL |
| WiFi 信号强度 | > -70dBm | TBD | dBm | □PASS □FAIL |

**总体评估**: □ 成功 / □ 部分成功 / □ 失败

---

### 问题记录与解决方案

| 问题编号 | 描述 | 发现时间 | 解决方法 | 状态 |
|---------|------|---------|---------|------|
| 1 | TBD | TBD | TBD | □已解决 □待处理 |
| 2 | TBD | TBD | TBD | □已解决 □待处理 |

---

### 附录：关键代码片段

#### 1. ESP32-C6 GPIO 合法性校验

```cpp
// 来自 ChipConfig.h
#if defined(CONFIG_IDF_TARGET_ESP32C6)
  // C6 实际可用 GPIO 仅 0-23（24-30 为封装内部 Flash/PSRAM SPI）
  #define CHIP_MAX_GPIO       23
  #define CHIP_NAME           "ESP32-C6"
  
  // 保留引脚列表（不可用于外设）
  static const uint8_t CHIP_RESERVED_PINS[] = { 
    12, 13, 14, 15, 16, 17, 24, 25, 26, 27, 28, 29, 30 
  };
#endif
```

#### 2. ResourceProfile 配置（已废弃：统一架构 v2.0）

> ⚠️ **重要说明**: FastBee-Arduino v2.0 已全面升级为**统一架构 (Unified Architecture)**，不再区分产品档位。
>
> **原档案例外（❌ 已过时）**:
> ```cpp
> // Legacy: DO NOT USE - Removed in Unified Architecture v2.0
> namespace ResourceProfile {
>   static constexpr const char* NAME = "esp32c6-mid";
>   static constexpr size_t MAX_PERIPHERALS = 24;
>   static constexpr size_t MAX_PERIPH_EXEC_RULES = 24;
>   static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = 24;
> }
> ```
>
> **正确做法（✅ 推荐）**:
> 所有功能默认编入固件，启动时根据内存预算自动降级。参见 [`FeatureBudget`](file://include/core/FeatureBudget.h) 机制。

#### 2. 统一架构 v2.0 内存管理机制

所有功能默认编入固件，启动时根据内存预算自动降级。

#### 3. LCDManager 初始化（OLED）

```cpp
// 来自 src/peripherals/LCDManager.cpp
// I2C 模式初始化（全缓冲，性能最优）
_display = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(
    U8G2_R0, _resetPin, config.pins[1], config.pins[0]
);
// pins[0]=SCL(GPIO5), pins[1]=SDA(GPIO21)
```

#### 4. 典型启动日志（期望看到的内容）

**单版本化后特性开关控制**:

FastBee-Arduino v2.0 采用运行时动态功能开关机制，而非编译期档位裁剪：

```cpp
// features.json 中的功能开关示例
{
  "version": 1,
  "features": {
    "lcd": { "enabled": true },           // OLED/LCD 支持
    "ruleScript": { "enabled": true },     // 规则脚本引擎
    "mqtt": { "enabled": true },           // MQTT 协议栈
    "ethernet": { "enabled": false }       // 以太网（未启用）
  }
}
```

当设备启动时检测到内存不足，`FeatureBudget` 会自动禁用部分重型功能：

```cpp
// src/core/RuntimeFeatureManager.cpp
void RuntimeFeatureManager::syncBudgetState() {
    auto& budgetReg = FeatureBudgetRegistry::instance();
    for (size_t i = 0; i < count_; ++i) {
        features_[i].budgetPassed = !budgetReg.isDenied(features_[i].meta.key);
    }
}
```

**关键原则**: 
- ✅ **不区分产品档位**: 所有功能默认编入单一固件
- ✅ **运行时自动降级**: 根据 DRAM 水位智能关闭非必需模块
- ✅ **灵活配置保存**: `features.json` 仅记录用户显式修改项

#### 5. 典型启动日志（期望看到的内容）

```
============================================
FastBee Framework v1.0.0 (Build: 2026-08-20)
Hardware: ESP32-C6, CPU Freq: 160MHz, Flash: 4MB
Free Heap: 325000 bytes, Largest Block: 280000 bytes
Developer Mode: ENABLED
============================================

[INFO] Peripheral Initialization Started...
[INFO] Peripheral Manager: Adding 'oled_display' (LCD, I2C)
[INFO] Peripheral Manager: LCD 'oled_display' initialized 128x64 I2C freq=400000Hz addr=0x03 contrast=255
[INFO] GPIO Pins: SDA=21, SCL=5
[INFO] U8G2 object created successfully
[INFO] Display ready for rendering
[INFO] Peripheral Manager: Adding 'led_indicator' (GPIO_DIGITAL_OUTPUT, GPIO19)
[INFO] Peripheral Manager: GPIO 'led_indicator' initialized pin=19 state=LOW
[INFO] All 2 peripherals initialized successfully
[INFO]
[INFO] Network Initialization Started...
[INFO] WiFi Station mode enabled, SSID: your_wifi_ssid
[INFO] WiFi connected! RSSI: -68 dBm
[INFO] IP Address: 192.168.1.105
[INFO] MDNS: fastbee-c6-001.local available
[INFO]
[INFO] Time Synchronization...
[INFO] NTP Server: http://iot.fastbee.cn/prod-api/iot/tool/ntp
[INFO] Time synced: 2026-08-20 14:30:45 CST-8
[INFO]
[INFO] MQTT Connection...
[INFO] MQTT Client ID: FBE_C6_001
[INFO] Connecting to iot.fastbee.cn:1883...
[INFO] MQTT Connected! Subscribed to: fb/iot/FBE_C6_001/cmd
[INFO]
[INFO] Web Server Started
[INFO] Local URL: http://192.168.1.105
[INFO] MDNS URL: http://fastbee-c6-001.local
[INFO]
[INFO] System Ready! All services running normally.
[INFO] ============================================
```

---

## 🎯 总结与建议

### 成功经验
1. ✓ ESP32-C6 资源丰富（24 个 GPIO），适合多外设应用
2. ✓ I2C 接口驱动成熟，OLED 显示效果优秀
3. ✓ MQTT 连接稳定，云端同步可靠
4. ✓ 配置导入自动化程度高

### 改进建议
1. TBD
2. TBD

### 后续扩展
1. 增加温度传感器（DS18B20）实现真实温湿度监测
2. 添加 WebSocket 支持实现实时数据推送
3. 引入 OTA 更新功能实现远程升级

---

## 📞 技术支持

遇到问题？请参考：
- [FastBee-Arduino GitHub](https://github.com/fastbee/FastBee-Arduino)
- [ESP32-C6 官方文档](https://docs.espressif.com/projects/esp-idf/)
- [PlatformIO 文档](https://docs.platformio.org/)

---

**文档版本**: 1.0  
**更新日期**: 2026-08-20  
**维护者**: FastBee Team
