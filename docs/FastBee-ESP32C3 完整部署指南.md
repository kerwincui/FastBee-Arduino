# FastBee-ESP32C3 设备完整部署指南

## 📋 目录
1. [硬件准备](#硬件准备)
2. [固件构建与烧录](#固件构建与烧录)
3. [配置导入](#配置导入)
4. [功能验证](#功能验证)
5. [性能测试](#性能测试)
6. [结果报告](#结果报告)

---

## 🔧 硬件准备

### ESP32-C3 开发板规格要求
- **芯片型号**: ESP32-C3 (RISC-V 架构)
- **Flash**: 4MB (推荐)
- **PSRAM**: 无 (C3 通常不带 PSRAM)
- **GPIO**: 支持 GPIO 0-19

### 外围设备连接

#### DHT11 温湿度传感器
```
DHT11 VCC ──────── 3.3V (ESP32-C3)
DHT11 DAT ─────── GPIO10
DHT11 GND ─────── GND
```

#### OLED 显示屏 (I2C 接口)
```
OLED VCC ──────── 3.3V (ESP32-C3)
OLED GND ──────── GND
OLED SCL ──────── GPIO5
OLED SDA ──────── GPIO6
```

#### 板载 LED
```
LED ───────────── GPIO8 (通过限流电阻)
```

---

## 🏗️ 固件构建与烧录

### 步骤 1: 环境检查

```powershell
# PowerShell 命令
# 检查 PlatformIO 安装
pio --version

# 检查 esptool 安装
python -m esptool --version

# 设置编码环境变量
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
```

### 步骤 2: 构建固件

```powershell
# 进入项目目录
cd d:\project\gitee\FastBee-Arduino

# 构建 esp32c3-F4R0 环境固件（4MB Flash）
pio run -e esp32c3-F4R0

# 构建成功后产物位置：
# .pio/build/esp32c3-F4R0/firmware.bin
# .pio/build/esp32c3-F4R0/filesystem.jar
```

### 步骤 3: 自动部署脚本

**方案 A: 使用现有脚本（推荐）**

```powershell
# 等待设备连接并自动烧录
.\scripts\c3-connect-loop.ps1
```

此脚本会：
1. 轮询检测 COM 端口（每 8 秒）
2. 确认 ESP32-C3 芯片响应
3. 自动执行完整部署（`deploy-all.ps1`）

**方案 B: 手动烧录**

```powershell
# 查找串口端口
Get-SerialPort | Where-Object {$_.DeviceId -like "COM*"}

# 擦除 Flash 并烧录固件
python -m esptool -p COMx -b 921600 --chip esp32c3 \
  erase_flash && \
  write_flash -z 0x0 .pio/build/esp32c3-F4R0/partition_table.bin \
              0x8000 .pio/build/esp32c3-F4R0/bootloader.bin \
              0x10000 .pio/build/esp32c3-F4R0/firmware.bin \
              0x200000 .pio/build/esp32c3-F4R0/littlefs.bin

# COMx 替换为实际端口号（如 COM3）
```

### 步骤 4: 查看启动日志

```powershell
# 以 921600 波特率打开串口监视器
pio device monitor -e esp32c3-F4R0 --port COMx --baud 921600
```

预期日志：
```
[INFO] FastBee Framework v1.0.0
[INFO] Chip Model: ESP32-C3
[INFO] Free Heap: 285000 bytes
[INFO] Initializing peripherals...
[INFO] Peripheral Manager: Added peripheral 'dht11_sensor' (SENSOR_DHT11)
[INFO] Peripheral Manager: Added peripheral 'oled_display' (LCD)
[INFO] Peripheral Manager: Added peripheral 'led_indicator' (GPIO_DIGITAL_OUTPUT)
[INFO] All peripherals initialized successfully
[INFO] Network Manager: Starting WiFi station mode...
[INFO] WiFi connected to your_wifi_ssid
[INFO] IP Address: 192.168.x.x
[INFO] MQTT Client: Connecting to iot.fastbee.cn:1883...
[INFO] MQTT Connected!
[INFO] Web server started at http://192.168.x.x
```

---

## ⚙️ 配置导入

### 配置文件结构

从 `docs/esp32c3-deployment-config.json` 分离出以下文件：

```
/config/
├── device.json      # 设备基本信息
├── network.json     # 网络配置
├── peripherals.json # 外设配置（3 个）
├── periph_exec.json # 执行规则（2 条）
└── protocol.json    # 通信协议
```

### 方法一：Web 界面批量导入（推荐）

1. **访问设备 Web 界面**
   ```
   http://192.168.x.x  （从串口日志获取 IP）
   ```

2. **登录管理后台**
   - 用户名：`admin`
   - 密码：`admin` （默认）

3. **导入配置**
   - 导航至：设备配置 → 配置导入/导出
   - 选择 `esp32c3-deployment-config.json`
   - 点击"导入全部配置"
   
4. **配置内容验证**
   ```
   ✓ Device Config: FBE_C3_001
   ✓ Network Config: WiFi Station 模式
   ✓ Peripherals: 3 个（DHT11, OLED, LED）
   ✓ Periph Exec: 2 条规则
   ✓ Protocol: JSON 格式，MQTT 主题已配置
   ```

### 方法二：API 逐个导入

如果 Web 界面不可用，可以使用 REST API 导入：

```bash
# 获取认证 Token（先登录）
POST http://192.168.x.x/api/auth/login
Content-Type: application/json

{"username":"admin","password":"admin"}

# 响应示例：
{"success":true,"data":{"token":"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..."}}

# 设置 Authorization header: Bearer <token>

# 1. 导入 device.json（设备信息）
PUT http://192.168.x.x/api/device/config
Content-Type: application/json

{
  "deviceId": "FBE_C3_001",
  "deviceName": "ESP32-C3 温湿度监测设备",
  "productNumber": 1,
  "userId": "1",
  "location": "实验室 A 区",
  "description": "基于 ESP32-C3 的温湿度监测与 OLED 显示设备",
  "enableNTP": true,
  "ntpServer1": "http://iot.fastbee.cn/prod-api/iot/tool/ntp",
  "ntpServer2": "cn.pool.ntp.org",
  "timezone": "CST-8",
  "syncInterval": 3600,
  "logLevel": "INFO",
  "developerModeEnabled": true
}

# 2. 导入 network.json（网络配置）
PUT http://192.168.x.x/api/network/config
Content-Type: application/json

{
  "wifiMode": "station",
  "wifiSSID": "your_wifi_ssid",
  "wifiPassword": "your_wifi_password",
  "apSSID": "FastBee-C3-001",
  "apPassword": "12345678",
  "hostname": "fastbee-c3-001"
}

# 3. 导入 peripherals.json（外设配置）
# 需要 POST 每个外设到 /api/peripherals
curl -X POST http://192.168.x.x/api/peripherals \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{
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
  }'

# 重复添加 oled_display 和 led_indicator

# 4. 导入 periph_exec.json（执行规则）
# POST 每条规则到 /api/periph-exec
curl -X POST http://192.168.x.x/api/periph-exec \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "periodic_data_collection",
    "name": "周期性数据采集上报",
    "enabled": true,
    "triggerType": "interval",
    "interval": 60000,
    "actions": [...]
  }'

# 5. 导入 protocol.json（协议配置）
PUT http://192.168.x.x/api/protocol/config
Content-Type: application/json

{
  "version": "1.0",
  "dataFormat": "json",
  "publishTopics": ["fb/iot/FBE_C3_001/data"],
  "subscribeTopics": ["fb/iot/FBE_C3_001/cmd"]
}
```

### 配置参数详解

#### 1. DHT11 温湿度传感器
- **GPIO 引脚**: GPIO10（ESP32-C3 可用 GPIO 范围：0-19）
- **采样间隔**: 2000ms（符合 DHT11 最低 2s 采样要求）
- **超时设置**: 1000ms
- **数据精度**: ±2°C / ±5%RH

#### 2. OLED 显示屏 (I2C 接口)
- **接口**: I2C 模式（interface=2）
- **GPIO**: SCL=GPIO5, SDA=GPIO6
- **分辨率**: 128x64
- **频率**: 400kHz（高速 I2C）
- **地址**: 0x03（OLED 模块 I2C 地址）
- **对比度**: 255（最大值）

#### 3. 板载 LED
- **GPIO 引脚**: GPIO8
- **类型**: GPIO 数字输出
- **初始状态**: LOW（关闭）
- **控制方式**: PWM/数字开关

#### 4. MQTT 配置
- **服务器**: iot.fastbee.cn:1883（生产环境）
- **clientId**: FBE_C3_001（唯一标识）
- **QoS**: 1（至少一次）
- **Keep Alive**: 60 秒
- **自动重连**: 启用
- **发布主题**: fb/iot/FBE_C3_001/data
- **订阅主题**: fb/iot/FBE_C3_001/cmd

---

## ✅ 功能验证

### 验证 1: 温湿度数据采集

**测试方法:**
```bash
# 方法 1: 通过 MQTT 主题订阅实时数据
mosquitto_sub -h iot.fastbee.cn -p 1883 -t "fb/iot/FBE_C3_001/data" -v

# 方法 2: 通过 HTTP API 查询
curl http://192.168.x.x/api/sensor/dht11_sensor
```

**预期结果:**
```json
{
  "success": true,
  "data": {
    "temperature": 25.3,
    "humidity": 45.2,
    "unit": "C",
    "timestamp": 1692345678
  }
}
```

**精度验证标准:**
- 温度误差：±2°C（与标准温度计对比）
- 湿度误差：±5%RH（与标准湿度计对比）
- 采样频率：每 2 秒 1 次（符合 DHT11 限制）

### 验证 2: OLED 显示功能

**测试方法:**
1. **访问 Web 诊断页面**
   ```
   http://192.168.x.x/#/diagnostics/peripherals?tab=oled_display
   ```

2. **执行显示测试命令**
   ```bash
   curl -X POST http://192.168.x.x/api/periph-exec/test \
     -H "Content-Type: application/json" \
     -d '{
       "peripheralId": "oled_display",
       "action": "displayText",
       "params": {"text": "Test OK"}
     }'
   ```

**预期效果:**
- OLED 屏幕正常点亮
- 显示字符清晰无抖动
- 能够实时更新温湿度数值
- 显示格式：`Temp: 25.3°C Humi: 45.2%RH`

### 验证 3: LED 控制功能

**测试方法:**
```bash
# 开启 LED
curl -X POST http://192.168.x.x/api/peripherals/led_indicator/write \
  -H "Content-Type: application/json" \
  -d '{"state": 1}'

# 关闭 LED
curl -X POST http://192.168.x.x/api/peripherals/led_indicator/write \
  -H "Content-Type: application/json" \
  -d '{"state": 0}'
```

**或者通过 MQTT 命令:**
```bash
# 发布控制指令
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C3_001/cmd" \
  -m '{"command":"led 控制","parameters":{"value":1}}'
```

**预期效果:**
- LED 灯能够正常亮灭
- 响应延迟 < 500ms
- 状态切换无卡顿

### 验证 4: MQTT 通信测试

**测试命令:**
```bash
# 1. 发布测试消息
mosquitto_pub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C3_001/data" \
  -m '{"test":"mqtt connectivity check","timestamp":1692345678}'

# 2. 订阅主题接收
mosquitto_sub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C3_001/data" -v

# 3. 测试双向通信
mosquitto_sub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_C3_001/cmd" -v
```

**成功标准:**
- [ ] 设备能成功发布数据到云端
- [ ] 云端下发的指令能被设备接收
- [ ] MQTT 连接稳定不掉线
- [ ] clientId 不冲突（同一时间只在线一个设备）

---

## 📊 性能测试

### 测试 1: Web 访问性能

**测试工具:**
```html
<!-- 在浏览器控制台执行 -->
const url = 'http://192.168.x.x'; // 设备 IP

// 测试首页加载时间
async function testWebPerformance() {
  const start = performance.now();
  await fetch(url);
  const end = performance.now();
  console.log(`首页加载：${(end - start).toFixed(0)}ms`);
  
  // 测试资源配置
  const resources = await fetch(`${url}/api/peripherals/list`);
  const data = await resources.json();
  console.log('外设列表:', data);
}

testWebPerformance();
```

**性能指标:**
| 指标 | 目标值 | 实测值 | 结果 |
|------|--------|--------|------|
| 首页响应时间 | < 500ms | TBD | □通过/□失败 |
| API 响应时间 | < 300ms | TBD | □通过/□失败 |
| CSS/JS 加载时间 | < 200ms | TBD | □通过/□失败 |

### 测试 2: MQTT 消息传输成功率

**测试脚本:**
```bash
#!/bin/bash
# mqtt_reliability_test.sh

TARGET_TOPIC="fb/iot/FBE_C3_001/data"
MESSAGES=1000
SUCCESS=0

for i in $(seq 1 $MESSAGES); do
  mosquitto_pub -h iot.fastbee.cn -p 1883 \
    -t "$TARGET_TOPIC" \
    -m "{\"seq\":$i,\"timestamp\":$(date +%s)}" > /dev/null 2>&1
  
  if [ $? -eq 0 ]; then
    SUCCESS=$((SUCCESS + 1))
  fi
  
  # 每 100 条打印进度
  if [ $((i % 100)) -eq 0 ]; then
    echo "Progress: $i/$MESSAGES ($SUCCESS% success)"
  fi
done

echo ""
echo "=================================="
echo "MQTT 消息传输测试结果:"
echo "发送总数：$MESSAGES"
echo "成功数：$SUCCESS"
echo "成功率：$(awk "BEGIN {printf \"%.3f\", ($SUCCESS/$MESSAGES)*100}")%"
echo "=================================="
```

**测试结果记录:**
```
目标成功率：≥ 99.9%
实测成功率：TBD %
结论：□通过 / □失败
```

### 测试 3: 内存稳定性测试

**监控方法:**
```bash
# 方法 1: 通过 HTTP API 定期查询
while true; do
  FREE_HEAP=$(curl -s http://192.168.x.x/api/system/health \
    | grep -o '"freeHeap":[0-9]*' \
    | cut -d':' -f2)
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
  echo "$TIMESTAMP - Free Heap: ${FREE_HEAP} bytes"
  sleep 60
done
```

**长期测试（24 小时）监控表:**
| 时间点 | 空闲堆内存 (bytes) | CPU 负载 (%) | MQTT 状态 | 备注 |
|--------|-------------------|--------------|-----------|------|
| 00:00 | TBD | TBD | Connected | 启动 |
| 06:00 | TBD | TBD | Connected | 运行 6h |
| 12:00 | TBD | TBD | Connected | 运行 12h |
| 18:00 | TBD | TBD | Connected | 运行 18h |
| 24:00 | TBD | TBD | Connected | 运行 24h |

**判断标准:**
- [ ] 空闲堆内存波动 < 10%
- [ ] 无内存泄漏迹象（单调下降）
- [ ] 无复位或重启事件
- [ ] 所有外设持续正常工作

### 测试 4: NTP 时间同步测试

**验证命令:**
```bash
# 查询设备时间状态
curl http://192.168.x.x/api/device/time

# 预期响应
{
  "success": true,
  "data": {
    "datetime": "2024-08-20 14:30:45",
    "timestamp": 1692526245,
    "synced": true,
    "timeValid": true,
    "internetAvailable": true,
    "uptime": 86400000,
    "timezone": "CST-8"
  }
}
```

**验证要点:**
- [ ] NTP 服务器连接成功
- [ ] 时间戳准确（与系统时间误差 < 1 秒）
- [ ] 时区正确设置为 CST-8（中国标准时间）
- [ ] 同步间隔为 3600 秒（1 小时）

---

## 📝 结果报告模板

### 部署完成确认清单

- [ ] **固件烧录**: ESP32-C3 固件已成功写入 Flash
- [ ] **外设识别**: 
  - [ ] DHT11 传感器（GPIO10）
  - [ ] OLED 显示屏（I2C, GPIO5/6）
  - [ ] LED 指示灯（GPIO8）
- [ ] **网络连接**: WiFi Station 模式成功连接 AP
- [ ] **MQTT 通信**: 成功连接到 iot.fastbee.cn
- [ ] **配置导入**: 所有配置文件正确加载
- [ ] **Web 界面**: 可正常访问和操作

### 性能测试数据汇总

| 测试项目 | 目标指标 | 实测结果 | 状态 |
|---------|---------|---------|------|
| Web 访问延迟 | < 500ms | TBD ms | □PASS □FAIL |
| MQTT 传输成功率 | ≥ 99.9% | TBD % | □PASS □FAIL |
| 24 小时内存稳定性 | 波动 < 10% | TBD % | □PASS □FAIL |
| 温湿度精度 | ±2°C / ±5%RH | TBD | □PASS □FAIL |
| NTP 时间同步 | 误差 < 1s | TBD s | □PASS □FAIL |
| LED 控制响应 | < 500ms | TBD ms | □PASS □FAIL |
| OLED 显示刷新 | 正常更新 | TBD | □PASS □FAIL |

### 问题记录与解决

| 问题编号 | 描述 | 解决方法 | 状态 |
|---------|------|---------|------|
| 1 | TBD | TBD | □已解决 |
| 2 | TBD | TBD | □待处理 |

### 结论与建议

**总体评估:** □成功 / □部分成功 / □失败

**改进建议:**
1. TBD
2. TBD

---

## 🎯 附录：关键代码片段参考

### 1. ESP32-C3 GPIO 映射表

| 功能 | GPIO 编号 | ESP32-C3 限制 |
|------|----------|-------------|
| DHT11 DATA | GPIO10 | ✓ 可用 |
| OLED SCL | GPIO5 | ✓ 可用 |
| OLED SDA | GPIO6 | ✓ 可用 |
| LED | GPIO8 | ✓ 可用 |

### 2. ResourceProfile 配置

ESP32-C3 的资源配额（来自 `include/core/ResourceProfile.h`）:
```cpp
namespace ResourceProfile {
  static constexpr const char* NAME = "esp32c3-lite";
  static constexpr size_t MAX_PERIPHERALS = 16;    // GPIO/内存限制
  static constexpr size_t MAX_PERIPH_EXEC_RULES = 12;
  static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = 16;
}
```

### 3. 典型启动日志（期望看到的内容）

```
[INFO] ============================================================
[INFO] FastBee Framework v1.0.0 (Build: 2024-08-20)
[INFO] Hardware: ESP32-C3, CPU Freq: 160MHz, Flash: 4MB
[INFO] Free Heap: 285000 bytes, Largest Block: 245000 bytes
[INFO] Developer Mode: ENABLED
[INFO] 
[INFO] Peripheral Initialization Started...
[INFO] Peripheral Manager: Adding 'dht11_sensor' (SENSOR_DHT11, GPIO10)
[INFO] Peripheral Manager: Generic Sensor 'dht11_sensor' initialized pin=10 sensorType=1 interval=2000ms
[INFO] Peripheral Manager: Adding 'oled_display' (LCD, I2C)
[INFO] Peripheral Manager: LCD 'oled_display' initialized 128x64 I2C freq=400000Hz addr=0x03 contrast=255
[INFO] Peripheral Manager: Adding 'led_indicator' (GPIO_DIGITAL_OUTPUT, GPIO8)
[INFO] Peripheral Manager: GPIO 'led_indicator' initialized pin=8 state=LOW
[INFO] All 3 peripherals initialized successfully
[INFO]
[INFO] Network Initialization Started...
[INFO] WiFi Station mode enabled, SSID: your_wifi_ssid
[INFO] WiFi connected! RSSI: -65 dBm
[INFO] IP Address: 192.168.1.100
[INFO] MDNS: fastbee-c3-001.local available
[INFO]
[INFO] Time Synchronization...
[INFO] NTP Server: http://iot.fastbee.cn/prod-api/iot/tool/ntp
[INFO] Time synced: 2024-08-20 14:30:45 CST-8
[INFO]
[INFO] MQTT Connection...
[INFO] MQTT Client ID: FBE_C3_001
[INFO] Connecting to iot.fastbee.cn:1883...
[INFO] MQTT Connected! Subscribed to: fb/iot/FBE_C3_001/cmd
[INFO]
[INFO] Web Server Started
[INFO] Local URL: http://192.168.1.100
[INFO] MDNS URL: http://fastbee-c3-001.local
[INFO]
[INFO] System Ready! All services running normally.
[INFO] ============================================================
```

---

## 📞 技术支持

遇到问题？请参考：
- [FastBee-Arduino GitHub](https://github.com/fastbee/FastBee-Arduino)
- [设备文档库](d:/project/gitee/FastBee-Arduino/docs/)
- [PlatformIO 官方文档](https://docs.platformio.org/)

---

**文档版本:** 1.0  
**更新日期:** 2024-08-20  
**维护者:** FastBee Team
