# FastBee-ESP32（普中开发板）完整部署指南

## 📋 项目概述

**部署目标**: 将 FastBee 设备部署到普中 ESP32 开发板，完成固件刷写、配置导入、功能验证的全流程。

**硬件平台**: 普中 ESP32-WROOM-32 开发板（经典 ESP32-D0WDQ6）
- **芯片型号**: ESP32-WROOM-32 (Dual-core Xtensa 32-bit LX6)
- **Flash**: 4MB (ESP32 标准配置)
- **PSRAM**: No (普中开发板通常不带 PSRAM)
- **WiFi**: IEEE 802.11 b/g/n (2.4 GHz)
- **Bluetooth**: v4.2 BR/EDR and LE

**外设配置** (基于您的配置文件):
- OLED 显示屏 (I2C, GPIO5-6)
- LED 指示灯 (GPIO8)  
- DHT11 温湿度传感器 (GPIO10)
- MQTT 通信 (iot.fastbee.cn)

---

## 🔧 硬件准备

### 普中 ESP32 开发板引脚图参考

| 功能 | GPIO | 状态 | 备注 |
|------|------|------|------|
| OLED SCL | GPIO5 | ✓ 可用 | I2C 时钟线 |
| OLED SDA | GPIO6 | ⚠️ 注意 | 连接 Flash，需谨慎使用 |
| LED 指示灯 | GPIO8 | ✓ 可用 | 板载 LED |
| DHT11 DATA | GPIO10 | ✓ 可用 | 数字信号输入 |

### ⚠️ 重要警告：关于 GPIO6-11

**普中 ESP32 开发板使用的 ESP32-WROOM-32 模块中，以下引脚被内部 Flash SPI 占用**:
- **GPIO 6-11**: Flash 接口引脚（不可用于外设）
- **GPIO 0, 2, 3**: Bootloader 相关（可特殊用途）

**解决方案**: 
如果配置文件中使用 GPIO6 作为 I2C SCL 引脚，**必须修改为其他可用引脚**。推荐方案：

```
原配置:  OLED SDA = GPIO6, OLED SCL = GPIO5
修改为:  OLED SDA = GPIO21, OLED SCL = GPIO5
```

### 正确的接线方案

#### OLED 显示屏 (SSD1306 I2C)
```
OLED VCC ──────────── 3.3V (ESP32)       # 电源正极
OLED GND ──────────── GND                # 地线
OLED SDA ──────────── GPIO21             # 数据引脚 (替代原 GPIO6)
OLED SCL ──────────── GPIO5              # 时钟引脚 (保持不变)
```

#### LED 指示灯
```
LED (+) ───────────── GPIO8 + 220Ω限流电阻
LED (-) ───────────── GND
```

#### DHT11 温湿度传感器
```
DHT11 VCC ─────────── 3.3V (ESP32)       # 电源正极（部分 DHT11 支持 3.3V）
DHT11 DAT ─────────── GPIO10             # 数据引脚
DHT11 GND ─────────── GND                # 地线
```
**注意**: DHT11 通常需要 3.3V-5V 供电，数据线上建议添加 10kΩ上拉电阻到 3.3V

### 完整的硬件连接示意图

```
┌─────────────────────────────────────────────┐
│           ESP32-WROOM-32 Dev Board          │
│                                             │
│  GPIO5 ────────────┐                       │
│  GPIO6 ⚠️保留      │  (不接任何设备)        │
│  GPIO8 ────────────┼─── LED 指示灯          │
│  GPIO10 ───────────┼─── DHT11 DATA         │
│  GPIO21 ───────────┼─── OLED SDA           │
│  GPIO22 ───────────┤ (备用 I2C 可选)         │
│                     │                       │
│  3.3V ─────────────┬─┼─────────────────────┤
│  GND  ─────────────┼─┼─────────────────────┤
│                     │                       │
└─────────────────────┴─┴─────────────────────┘
                       │
        ┌──────────────┴──────────────┐
        │                             │
   ┌────┴────┐                   ┌────┴────┐
   │ OLED    │                   │ DHT11   │
   │ 0.96"   │                   │ Sensor  │
   │ SSD1306 │                   │         │
   └─────────┘                   └─────────┘
```

---

## 🏗️ 固件构建与烧录

### 步骤 1: 环境检查

```powershell
cd d:\project\gitee\FastBee-Arduino

# 检查 PlatformIO
pio --version

# 检查 esptool
python -m esptool --version

# 设置编码环境变量
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
```

### 步骤 2: 构建固件（经典 ESP32 版本）

```powershell
# 构建 esp32-F4R0 环境（经典 ESP32, 4MB Flash）
pio run -e esp32-F4R0

# 或使用后台编译
pio build -e esp32-F4R0 --backend keepalive
```

**构建产物位置**:
```
.pio/build/esp32-F4R0/
├── firmware.bin          # 主固件
├── bootloader.bin
├── partition_table.bin
└── littlefs.bin
```

### 步骤 3: 查看串口信息

```powershell
# 查找连接的 ESP32 设备
Get-SerialPort | Where-Object {$_.DeviceId -like "COM*"}

# 常见的 COM 端口: COM3, COM4, COM5...
# 确认 USB-UART 转换芯片类型（CP2102, CH340, FT232 等）
```

### 步骤 4: 自动部署脚本

针对普中 ESP32 开发板创建专用脚本：

```powershell
# .\scripts\puzhong-esp32-deploy.ps1
param(
    [string]$Port = "COM3",  # 默认端口，可根据实际情况修改
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

# 设置编码
$env:PYTHONIOENCODING = "utf-8"

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "普中 ESP32 开发板自动化部署工具" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "串口端口：$Port" -ForegroundColor Yellow
Write-Host "波特率：$Baud" -ForegroundColor Yellow
Write-Host ""

# 尝试连接到设备
$py = 'C:\Users\kerwin\.platformio\penv\Scripts\python.exe'

Write-Host "[1/4] 检测 ESP32 设备..." -ForegroundColor Green
try {
    $result = & $py -m esptool `
        --chip esp32 `
        --port $Port `
        --baud $Baud `
        chip-id
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "✓ 检测到 ESP32 芯片" -ForegroundColor Green
        Write-Host $result
    } else {
        throw "无法检测到 ESP32 芯片"
    }
} catch {
    Write-Error "✗ 设备检测失败：$_" -ErrorAction Stop
}

# 擦除 Flash（首次部署或重新烧录时需要）
Write-Host "`n[2/4] 擦除 Flash..." -ForegroundColor Green
& $py -m esptool -p $Port -b 921600 --chip esp32 erase_flash

if ($LASTEXITCODE -ne 0) {
    Write-Error "✗ Flash 擦除失败"
    exit 1
}
Write-Host "✓ Flash 擦除完成" -ForegroundColor Green

# 烧录固件
Write-Host "`n[3/4] 烧录固件..." -ForegroundColor Green
& $py -m esptool -p $Port -b 921600 --chip esp32 write_flash `
    -z `
    0x1000 .pio/build/esp32-F4R0/bootloader.bin `
    0x8000 .pio/build/esp32-F4R0/partition_table.bin `
    0x10000 .pio/build/esp32-F4R0/firmware.bin

if ($LASTEXITCODE -ne 0) {
    Write-Error "✗ 固件烧录失败"
    exit 1
}
Write-Host "✓ 固件烧录完成" -ForegroundColor Green

# 上传文件系统
Write-Host "`n[4/4] 上传 LittleFS 文件系统..." -ForegroundColor Green
& $py -m esptool -p $Port -b 921600 --chip esp32 write_flash `
    0x200000 .pio/build/esp32-F4R0/littlefs.bin

if ($LASTEXITCODE -ne 0) {
    Write-Warning "⚠ LittleFS 上传可能失败（文件为空）"
} else {
    Write-Host "✓ LittleFS 上传完成" -ForegroundColor Green
}

Write-Host "`n==================================================" -ForegroundColor Green
Write-Host "✅ 部署完成！请重启设备查看启动日志" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Green

# 打开串口监视器
Write-Host "`n正在启动串口监视器..." -ForegroundColor Cyan
Start-Sleep -Seconds 3  # 等待设备重启
& pio device monitor -e esp32-F4R0 --port $Port --baud 115200
```

执行方式:
```powershell
.\scripts\puzhong-esp32-deploy.ps1 -Port COM3
```

---

## ⚙️ 配置文件准备

### 关键修正：GPIO6 问题

**原始配置文件问题**:
```json
{
  "id": "oled_display",
  "type": "LCD",
  "pins": [5, 6],  // ❌ GPIO6 错误！这是 Flash 接口引脚
  ...
}
```

**正确配置修正**:
```json
{
  "id": "oled_display",
  "type": "LCD",
  "pins": [5, 21],  // ✅ GPIO5 (SCL) + GPIO21 (SDA)
  ...
}
```

### 完整的配置文件（修正版）

我为您创建了适配普中 ESP32 的配置文件：`docs/puzhong-esp32-config.json`

```json
{
  "device": {
    "deviceId": "FBE_PZ_001",
    "deviceName": "普中 ESP32 环境监测设备",
    "productNumber": 1,
    "userId": "1",
    "description": "基于普中 ESP32 开发板的温湿度监测与 OLED 显示系统",
    "location": "实验室 A 区",
    "enableNTP": true,
    "ntpServer1": "http://iot.fastbee.cn/prod-api/iot/tool/ntp",
    "ntpServer2": "cn.pool.ntp.org",
    "timezone": "CST-8",
    "syncInterval": 3600,
    "logLevel": "INFO",
    "developerModeEnabled": true
  },
  "network": {
    "wifiMode": "station",
    "wifiSSID": "your_wifi_ssid",
    "wifiPassword": "your_wifi_password",
    "apSSID": "FastBee-PZ-001",
    "apPassword": "12345678",
    "hostname": "fastbee-pz-001"
  },
  "mqtt": {
    "server": "iot.fastbee.cn",
    "port": 1883,
    "sslEnabled": false,
    "clientId": "FBE_PZ_001",
    "keepAlive": 60,
    "cleanSession": true,
    "autoReconnect": true,
    "publishTopic": "fb/iot/FBE_PZ_001/data",
    "subscribeTopic": "fb/iot/FBE_PZ_001/cmd"
  },
  "peripherals": [
    {
      "id": "oled_display",
      "name": "0.96 英寸 OLED 显示屏",
      "type": "LCD",
      "enabled": true,
      "pinCount": 2,
      "pins": [5, 21],  // ✅ 修正：SCL=GPIO5, SDA=GPIO21
      "params": {
        "width": 128,
        "height": 64,
        "interface": 2,  // I2C 模式
        "frequency": 400000,
        "address": 3,
        "contrast": 255
      }
    },
    {
      "id": "led_indicator",
      "name": "板载 LED 指示灯",
      "type": "GPIO_DIGITAL_OUTPUT",
      "enabled": true,
      "pinCount": 1,
      "pins": [8],
      "params": {
        "initialState": 0,
        "passive": false
      }
    },
    {
      "id": "dht11_sensor",
      "name": "DHT11 温湿度传感器",
      "type": "SENSOR_DHT11",
      "enabled": true,
      "pinCount": 1,
      "pins": [10],
      "params": {
        "sampleInterval": 2000,
        "timeout": 1000
      }
    }
  ],
  "periph_exec": [
    {
      "id": "periodic_data_collection",
      "name": "周期性数据采集上报",
      "enabled": true,
      "triggerType": "interval",
      "interval": 60000,  // 每 60 秒
      "actions": [
        {
          "type": "readPeripheral",
          "peripheralId": "dht11_sensor",
          "params": {}
        },
        {
          "type": "updateDisplay",
          "peripheralId": "oled_display",
          "params": {
            "template": "Temp: {{temp}}°C Humi: {{humi}}%RH"
          }
        },
        {
          "type": "publishData",
          "params": {
            "topic": "fb/iot/FBE_PZ_001/data",
            "payloadTemplate": "{\"deviceId\":\"FBE_PZ_001\",\"temperature\":{{temp}},\"humidity\":{{humi}},\"timestamp\":{{timestamp}}}"
          }
        }
      ]
    },
    {
      "id": "led_control_rule",
      "name": "LED 开关控制规则",
      "enabled": true,
      "triggerType": "command",
      "commandPattern": "led 开关",
      "actions": [
        {
          "type": "writePin",
          "peripheralId": "led_indicator",
          "params": {
            "state": "{{value}}"
          }
        }
      ]
    }
  ],
  "protocol": {
    "version": "1.0",
    "dataFormat": "json",
    "publishTopics": ["fb/iot/FBE_PZ_001/data"],
    "subscribeTopics": ["fb/iot/FBE_PZ_001/cmd"]
  }
}
```

**保存到**: `docs/puzhong-esp32-config.json`

---

## 📥 配置导入流程

### Web 界面导入（推荐方式）

#### 步骤 1: 访问设备 Web 界面
```
http://192.168.x.x  # 从串口日志获取 IP
# 例如：http://192.168.1.100
```

#### 步骤 2: 登录管理后台
```
用户名：admin
密码：admin
```

#### 步骤 3: 导航至配置导入页面
路径：**设备配置 → 配置导入/导出**

#### 步骤 4: 选择并上传配置文件
1. 点击 **"选择文件"** 按钮
2. 选择 `docs/puzhong-esp32-config.json`
3. 点击 **"导入全部配置"**

#### 步骤 5: 验证配置内容
导入后需核对以下内容：

```
✅ Device Config: FBE_PZ_001
   ├─ 设备名称：普中 ESP32 环境监测设备
   ├─ 产品编号：1
   └─ NTP 服务器：已配置

✅ Network Config:
   ├─ WiFi 模式：station
   ├─ AP SSID: FastBee-PZ-001
   └─ 主机名：fastbee-pz-001

✅ Peripherals (3 个):
   ├─ oled_display (I2C, GPIO5/21) ✅ 已修正
   ├─ led_indicator (GPIO8)
   └─ dht11_sensor (GPIO10)

✅ Periph Exec (2 条规则):
   ├─ periodic_data_collection (60 秒周期)
   └─ led_control_rule (命令触发)

✅ Protocol:
   ├─ Publish: fb/iot/FBE_PZ_001/data
   └─ Subscribe: fb/iot/FBE_PZ_001/cmd
```

### REST API 导入（备选方案）

如果 Web 界面不可用，可以使用以下命令：

```bash
# 前置：获取认证 Token
curl -X POST http://192.168.x.x/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"admin"}' \
  > token.txt

TOKEN=$(jq -r '.data.token' token.txt)

# 批量导入配置文件
curl -X PUT http://192.168.x.x/api/config/import \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d @"docs\puzhong-esp32-config.json"
```

---

## ✅ 功能验证测试

### 测试 1: OLED 显示验证

#### HTTP API 测试
```bash
# 测试 OLED 显示更新
curl -X POST http://192.168.x.x/api/peripherals/oled_display/test \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "action": "displayText",
    "params": {"text": "OK"}
  }'
```

**预期结果**:
- [ ] OLED 屏幕正常点亮
- [ ] 显示 "OK" 字样清晰无抖动
- [ ] 背光亮度适中
- [ ] 刷新流畅无闪烁

#### 物理观察要点
- ✅ 屏幕能正常初始化（启动时应有闪屏）
- ✅ I2C 总线地址识别正确（应检测到设备）
- ✅ 字符渲染无乱码或缺笔画

---

### 测试 2: LED 控制验证

#### HTTP API 测试
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

**预期结果**:
- [ ] LED 能够正常亮灭切换
- [ ] 响应延迟 < 500ms
- [ ] 切换过程平滑无异常
- [ ] LED 亮度合适（不过亮导致刺眼）

---

### 测试 3: DHT11 温湿度传感器验证

#### HTTP API 测试
```bash
# 读取传感器数据
curl http://192.168.x.x/api/sensors/dht11_sensor \
  -H "Authorization: Bearer YOUR_TOKEN"
```

**预期响应**:
```json
{
  "success": true,
  "data": {
    "temperature": 25.3,
    "humidity": 45.2,
    "unit": "C",
    "quality": "good",
    "timestamp": 1692526245
  }
}
```

#### 精度验证方法
1. **温度对比**: 与标准温度计比较，误差应在 ±2°C 范围内
2. **湿度对比**: 与标准湿度计比较，误差应在 ±5%RH 范围内
3. **采样频率**: 确认能正常工作（DHT11 最低采样间隔 2 秒）

**预期现象**:
- [ ] OLED 实时显示温度和湿度值
- [ ] 数值随环境变化而变化
- [ ] 数据格式正确（带小数点）
- [ ] 无异常跳变（如突然显示 999 或 -999）

---

### 测试 4: MQTT 通信验证

#### 订阅云端数据（需要 Mosquitto 工具）
```bash
# 订阅设备发布的数据
mosquitto_sub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_PZ_001/data" -v
```

**预期输出**:
```
{"deviceId":"FBE_PZ_001","temperature":25.3,"humidity":45.2,"timestamp":1692526245}
{"deviceId":"FBE_PZ_001","temperature":25.4,"humidity":45.1,"timestamp":1692526305}
...
```

#### 发送控制指令
```bash
# 下发 LED 开关指令
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_PZ_001/cmd" \
  -m '{"command":"led 开关","parameters":{"value":1}}'
```

**预期结果**:
- [ ] 设备接收到指令并响应
- [ ] LED 灯亮起（如果发送 value=1）
- [ ] MQTT 连接稳定不掉线

---

## 📊 性能测试报告模板

### 1. Web 访问性能测试

| 测试项目 | 目标值 | 实测值 | 单位 | 状态 |
|---------|--------|--------|------|------|
| 首页加载时间 | < 500ms | TBD | ms | □PASS □FAIL |
| API 响应时间 | < 300ms | TBD | ms | □PASS □FAIL |
| CSS/JS 加载 | < 200ms | TBD | ms | □PASS □FAIL |
| 资源压缩率 | > 70% | TBD | % | □PASS □FAIL |

**测试命令**（浏览器控制台）:
```javascript
async function testPerformance() {
  const start = performance.now();
  await fetch('http://192.168.x.x');
  const end = performance.now();
  console.log(`首页加载：${Math.round(end - start)}ms`);
}
testPerformance();
```

---

### 2. MQTT 可靠性测试

**测试参数**:
- 发送消息数：1000 条
- 目标成功率：≥ 99.9%
- 主题：fb/iot/FBE_PZ_001/data

**测试结果汇总**:
```
发送总数：1000
成功数：TBD
失败数：TBD
成功率：TBD%
结论：□达标 / □未达标
```

---

### 3. 内存稳定性监控（24 小时）

| 时间点 | 空闲堆内存 | 运行时长 | MQTT 状态 | 备注 |
|--------|-----------|---------|-----------|------|
| 00:00 | TBD bytes | 0s | Connected | 启动 |
| 06:00 | TBD bytes | 21600s | Connected | 运行 6h |
| 12:00 | TBD bytes | 43200s | Connected | 运行 12h |
| 18:00 | TBD bytes | 64800s | Connected | 运行 18h |
| 24:00 | TBD bytes | 86400s | Connected | 运行 24h |

**统计指标**:
- 最大堆内存：TBD bytes
- 最小堆内存：TBD bytes
- 平均波动：TBD%
- 是否泄漏：□是 / □否

---

### 4. 传感器精度验证

| 测试项 | 测量值 | 标准值 | 误差 | 精度要求 | 状态 |
|--------|--------|--------|------|---------|------|
| 温度 | TBD °C | TBD °C | ±TBD °C | ±2°C | □PASS □FAIL |
| 湿度 | TBD %RH | TBD %RH | ±TBD %RH | ±5%RH | □PASS □FAIL |

---

## 🎯 常见问题排查

### 问题 1: OLED 黑屏或不显示

**可能原因**:
1. GPIO6 冲突（使用了 Flash 引脚）
2. I2C 地址错误
3. 接线松动

**解决方法**:
```bash
# 1. 检查 I2C 总线扫描
curl http://192.168.x.x/api/i2c/scanner

# 2. 确认 OLED 地址被检测到（通常是 0x3C 或 0x3D）
# 3. 如果没检测到，检查接线和上拉电阻
```

---

### 问题 2: DHT11 读取出错

**可能原因**:
1. 引脚接线错误
2. 缺少上拉电阻
3. 采样频率过高

**解决方法**:
- 确保 GPIO10 正确连接
- 添加 10kΩ上拉电阻到 3.3V
- 采样间隔不低于 2 秒

---

### 问题 3: MQTT 连接失败

**可能原因**:
1. WiFi 未连接
2. 服务器地址错误
3. clientId 冲突

**排查步骤**:
```bash
# 1. 检查网络状态
curl http://192.168.x.x/api/network/status

# 2. 检查 MQTT 状态
curl http://192.168.x.x/api/mqtt/status

# 3. 查看详细日志
pio device monitor -e esp32-F4R0 --port COMx
```

---

## 📝 最终验收清单

### 基础部署
- [ ] 固件成功烧录
- [ ] LittleFS 文件系统挂载正常
- [ ] 所有配置文件正确导入 `/config/` 目录

### 外设功能
- [ ] OLED 显示屏正常点亮且显示内容正确
- [ ] LED 能够正常亮灭切换
- [ ] DHT11 能够采集温湿度数据

### 通信功能
- [ ] WiFi 成功连接到 AP
- [ ] MQTT 成功连接到 iot.fastbee.cn
- [ ] 数据发布到云端主题
- [ ] 接收云端控制指令

### 性能指标
- [ ] Web 访问延迟 < 500ms
- [ ] MQTT 传输成功率 ≥ 99.9%
- [ ] 24 小时内存稳定无泄漏
- [ ] 传感器数据精度达标

### 长期运行
- [ ] 连续运行 24 小时无异常
- [ ] 无复位或重启事件
- [ ] NTP 时间同步正常
- [ ] 所有外设持续正常工作

---

## 🔗 相关资源

- [FastBee-Arduino GitHub](https://github.com/fastbee/FastBee-Arduino)
- [ESP32 GPIO 引脚映射表](../include/core/ChipConfig.h)
- [PlatformIO 官方文档](https://docs.platformio.org/)
- [DHT11 传感器数据手册](https://blog.csdn.net/...)

---

**文档版本**: 1.0  
**更新日期**: 2026-08-20  
**适用平台**: 普中 ESP32-WROOM-32 开发板
