# FastBee-ECM50-A (ESP32-S3) 工业级工控机完整部署指南

## 🎯 项目概述

**部署目标**: 在亿佰特 ECM50-A 工控机上启用全部功能，包括 Modbus RTU 主站、多模网络切换、AI/DI/DO 板载外设集成。

**硬件平台**: ECM50-A (ESP32-S3 R8 + 8MB PSRAM + 16MB Flash)

**核心特性**:
- ✅ Wi-Fi / 4G / 以太网 / LoRa 四模通信
- ✅ Modbus RTU 主站支持 (RS485/RS232)
- ✅ 2 路 AI(模拟量输入) + 2 路 DI(数字量输入) + 2 路 DO(继电器输出)
- ✅ MQTT 云平台对接
- ✅ Web 管理界面 + OTA 升级
- ✅ 断网自动切换机制

---

## 📋 ECM50-A 硬件规格

### GPIO 引脚定义表 (来自 Pin_List.xlsx)

| 接口功能 | GPIO 编号 | 供电电压 | 备注 |
|---------|----------|---------|------|
| **AI1 (模拟输入)** | GPIO6 | VDD3P3_RTC | ADC 输入，默认 4-20mA |
| **AI2 (模拟输入)** | GPIO7 | VDD3P3_RTC | ADC 输入，可改电压型 |
| **DI1 (数字输入)** | GPIO4 | VDD3P3_RTC | 干湿接点兼容，NPN 型 |
| **DI2 (数字输入)** | GPIO5 | VDD3P3_RTC | 干湿接点兼容，NPN 型 |
| **DO1 (继电器输出)** | GPIO15 | VDD3P3_RTC | COM/NO/NC 三端 |
| **DO2 (继电器输出)** | GPIO16 | VDD3P3_RTC | COM/NO/NC 三端 |
| **RS485 A/RX** | GPIO17 | VDD3P3_RTC | UART1_TX |
| **RS485 B/TX** | GPIO18 | VDD3P3_RTC | UART1_RX |
| **WiFi/BT LED** | GPIO45 | VDD3P3_CPU | 可编程指示 |
| **Run LED** | GPIO21 | VDD3P3_RTC | 运行状态指示 |
| **Ethernet SCLK** | GPIO12 | VDD3P3_RTC | SPI1_SCK |
| **Ethernet MISO** | GPIO13 | VDD3P3_RTC | SPI1_MISO |
| **Ethernet MOSI** | GPIO11 | VDD3P3_RTC | SPI1_MOSI |
| **Ethernet INT** | GPIO14 | VDD3P3_RTC | ETH_INT |
| **LoRa M1** | GPIO41 | VDD3P3_CPU | LoRa 模式配置 |

**重要资源**:
- **处理器**: ESP32-S3 Dual-core Xtensa 32-bit LX7, 最高 240MHz
- **内存**: 8MB PSRAM (Octal interface)
- **存储**: 16MB NOR Flash
- **扩展**: 1×TF 卡槽 (GPIO1/9/10/46), 最大 2TB
- **电源**: DC 8-28V 宽电压输入

---

## 🔧 第一步：固件构建与烧录

### 环境准备

```powershell
cd d:\project\gitee\FastBee-Arduino

# 设置编码环境变量
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

# 检查 PlatformIO
pio --version

# 查找连接的设备
Get-SerialPort | Where-Object {$_.DeviceId -like "COM*"}
```

### 构建 ECM50-S3-F8R4 固件

```powershell
# 构建 esp32s3-F8R4 环境（8MB PSRAM + 16MB Flash）
pio run -e esp32s3-F8R4

# 或使用后台编译（不阻塞终端）
pio build -e esp32s3-F8R4 --backend keepalive
```

### 自动化部署脚本

```powershell
.\scripts\deploy-all.ps1 -Env esp32s3-F8R4 -Ports COMx
```

预期启动日志应包含:
```
[INFO] FastBee Framework v1.0.0
[INFO] Chip Model: ESP32-S3
[INFO] PSRAM: 8MB Octal
[INFO] Free Heap: 485000 bytes (最大!)
[INFO] Developer Mode: ENABLED
...
[INFO] System Ready! All services running normally.
```

---

## ⚙️ 第二步：完整配置导入

### 配置文件说明

已为您创建完整的 ECM50-A 配置文件：`docs/ecm50a-full-config.json`

该配置包含以下部分:
1. **Device Config**: 设备基本信息、NTP、时区
2. **Network Config**: 三模联网 (Wi-Fi/4G/以太网)
3. **MQTT Config**: 云平台对接
4. **Peripherals Config**: 
   - 2×AI 模拟输入 (GPIO6/7)
   - 2×DI 数字输入 (GPIO4/5)
   - 2×继电器输出 (GPIO15/16)
5. **Modbus Subdevices**: 
   - DHT11 温湿度传感器 (Modbus RTU)
   - 双通道继电器模块
6. **Polling Tasks**: 60 秒轮询周期
7. **Protocol Config**: MQTT + Modbus 联合配置

### 配置导入流程

#### 方式一：Web 界面批量导入（推荐）

1. **访问设备**: `http://192.168.x.x` 或 `http://ecm50a.local`
2. **登录**: admin/admin
3. **导航**: 设备配置 → 配置导入/导出
4. **选择文件**: `docs/ecm50a-full-config.json`
5. **点击**: "导入全部配置"
6. **等待重启** (约 30 秒)

#### 方式二：API 逐文件导入

```bash
# 前置：获取 Token
TOKEN=$(curl -X POST http://192.168.x.x/api/auth/login \
  -d '{"username":"admin","password":"admin"}' \
  | jq -r '.data.token')

# 批量导入完整配置
curl -X PUT http://192.168.x.x/api/config/import \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d @docs/ecm50a-full-config.json
```

---

## ✅ 第三步：功能验证测试

### 测试清单与执行方法

#### [1] AI 模拟量输入测试

```bash
# 查询 AI 数据
curl http://192.168.x.x/api/sensors/ai_channel \
  -H "Authorization: Bearer $TOKEN"

# 期望响应
{
  "success": true,
  "data": {
    "channel": 1,
    "value": 12.5,  # mA 或 V
    "unit": "mA",
    "quality": "good"
  }
}
```

**物理验证**:
- 使用标准电流源注入 4-20mA 信号
- OLED/Web 显示值应与标准值误差 < ±0.1mA
- 采样间隔：1 秒 (配置已设)

---

#### [2] DI 数字量输入测试

```bash
# 查询 DI 状态
curl http://192.168.x.x/api/sensors/di_channel \
  -H "Authorization: Bearer $TOKEN"

# 期望响应
{
  "success": true,
  "data": {
    "channel": 1,
    "state": 1,     # 0=断开，1=闭合
    "edgeDetected": false,
    "lastEdgeTime": 0
  }
}
```

**物理验证**:
- 连接按钮开关到 DI1 (GPIO4)
- 按下开关，观察状态从 0→1 变化
- 验证上升沿中断检测功能正常

---

#### [3] 继电器输出测试

```bash
# 开启 DO1 继电器
curl -X POST http://192.168.x.x/api/peripherals/relay_do1/write \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"state": 1}'

# 关闭 DO1
curl -X POST http://192.168.x.x/api/peripherals/relay_do1/write \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"state": 0}'
```

**物理验证**:
- 连接负载 (如 LED 灯或小型电机) 到 DO1(COM+NO)
- 发送开启指令，听继电器"咔哒"吸合声
- 负载应正常工作
- 延时控制选项：可在 Web 界面配置延迟时间

---

#### [4] Modbus RTU 子设备测试

**配置了两个子设备**:
1. **DHT11 温湿度传感器** (假设地址：1)
   - 功能码：0x03 (保持寄存器读)
   - 起始地址：0x0000
   - 数量：2 个寄存器 (温度 + 湿度)
   
2. **双通道继电器模块** (假设地址：2)
   - 功能码：0x05 (线圈写)
   - 线圈地址：0-1
   - NC 模式：启用

**轮询任务**: 每 60 秒采集一次

**验证命令**:
```bash
# 查看 Modbus 状态
curl http://192.168.x.x/api/modbus/status \
  -H "Authorization: Bearer $TOKEN"

# 期望响应
{
  "success": true,
  "data": {
    "initialized": true,
    "mode": "master",
    "baudRate": 9600,
    "subDevices": [
      {"name": "DHT11 Sensor", "status": "ok"},
      {"name": "Relay Module", "status": "ok"}
    ],
    "pollTasks": [
      {"name": "Temperature Polling", "interval": 60, "lastExec": 1234567}
    ]
  }
}

# 查询 Modbus 上报数据
curl http://192.168.x.x/api/modbus/report \
  -H "Authorization: Bearer $TOKEN"

# 期望响应
{
  "success": true,
  "data": [
    {"id": "temp_sensor", "value": "25.3", "remark": "modbus"},
    {"id": "humi_sensor", "value": "45.2", "remark": "modbus"}
  ]
}
```

**物理验证**:
- 确保 RS485 A/B 正确连接到 DHT11 调制解调器
- 监听串口日志确认轮询请求成功发出
- 检查数据更新频率为 60 秒

---

#### [5] 多模网络切换测试

**5.1 Wi-Fi STA 模式**
```bash
# 配置 Wi-Fi
curl -X PUT http://192.168.x.x/api/network/wifi \
  -H "Authorization: Bearer $TOKEN" \
  -d '{
    "ssid": "your_wifi_ssid",
    "password": "your_password",
    "mode": "station"
  }'

# 验证连接
curl http://192.168.x.x/api/network/status \
  -H "Authorization: Bearer $TOKEN"

# 期望看到 WiFi status: connected
```

**5.2 以太网模式**
```bash
# 插入 RJ45 网线
# 查询以太网状态
curl http://192.168.x.x/api/network/ethernet \
  -H "Authorization: Bearer $TOKEN"

# 期望看到 PHY link up, DHCP 获取 IP
```

**5.3 4G 蜂窝网络**
```bash
# 插入 SIM 卡并连接 4G 天线
# 启用 4G
curl -X POST http://192.168.x.x/api/network/4g/enable \
  -H "Authorization: Bearer $TOKEN"

# 查询信号强度
curl http://192.168.x.x/api/network/4g/signal \
  -H "Authorization: Bearer $TOKEN"

# 期望 RSSI > -85dBm
```

**5.4 断网切换测试**
```
1. 当前连接 Wi-Fi
2. 拔掉网线或关闭路由器
3. 观察设备是否自动切换到以太网/4G
4. 恢复原网络后，是否回切
5. 全程无需重启，MQTT 保持连接
```

**验证标准**:
- [ ] 优先级策略生效 (以太网 > 4G > Wi-Fi)
- [ ] 断网后 ≤ 30 秒完成切换
- [ ] MQTT 不中断或重连时间 < 10 秒

---

#### [6] MQTT 云平台对接

**验证命令**:
```bash
# 查询 MQTT 状态
curl http://192.168.x.x/api/mqtt/status \
  -H "Authorization: Bearer $TOKEN"

# 期望响应
{
  "success": true,
  "data": {
    "connected": true,
    "server": "iot.fastbee.cn",
    "clientId": "FBE_ECM50A_001",
    "qos": 1,
    "keepAlive": 60,
    "lastPing": 1234567,
    "subscribeTopics": ["fb/iot/FBE_ECM50A_001/cmd"]
  }
}

# 订阅云端主题测试
mosquitto_sub -h iot.fastbee.cn -p 1883 \
  -t "fb/iot/FBE_ECM50A_001/data" -v

# 期望收到设备上报的数据
{"deviceId":"FBE_ECM50A_001","timestamp":...,"ai1":12.5,"ai2":8.3,"di1":1,"relay1":0,...}
```

---

## 📊 第四步：性能测试报告模板

### 1. Web 访问性能

| 测试项 | 目标 | 实测 | 单位 | 结果 |
|--------|------|------|------|------|
| 首页加载 | < 500ms | TBD | ms | □PASS |
| API 响应 | < 300ms | TBD | ms | □PASS |
| CSS/JS 压缩 | > 70% | TBD | % | □PASS |
| 首屏时间 | < 800ms | TBD | ms | □PASS |

**测试命令**:
```javascript
// Chrome 控制台
async function test() {
  const start = performance.now();
  await fetch('http://192.168.x.x');
  console.log(`首页：${Math.round(performance.now()-start)}ms`);
}
test();
```

---

### 2. MQTT 可靠性测试

```bash
#!/bin/bash
# mqtt_test_ecm50a.sh
TOPIC="fb/iot/FBE_ECM50A_001/data"
TOTAL=1000
SUCCESS=0

for i in $(seq 1 $TOTAL); do
  mosquitto_pub -h iot.fastbee.cn -p 1883 -t "$TOPIC" \
    -m "{\"test\":$i,\"ts\":$(date +%s)}" > /dev/null 2>&1 && SUCCESS=$((SUCCESS+1))
done

echo "成功率：$(awk "BEGIN {printf \"%.2f\", ($SUCCESS/$TOTAL)*100}")%"
```

**目标**: ≥ 99.9%

---

### 3. 24 小时稳定性测试

**监控脚本**:
```bash
while true; do
  FREE_HEAP=$(curl -s http://192.168.x.x/api/system/health \
    | jq -r '.data.freeHeap')
  echo "$(date '+%H:%M:%S') Free Heap: ${FREE_HEAP} bytes"
  sleep 60
done
```

**记录表**:

| 时间点 | Free Heap | CPU Load | MQTT | 网络 | 备注 |
|--------|-----------|----------|------|------|------|
| 00:00 | TBD MB | TBD% | Connected | Ethernet | 启动 |
| 06:00 | TBD MB | TBD% | Connected | Ethernet | 6h |
| 12:00 | TBD MB | TBD% | Connected | Ethernet | 12h |
| 18:00 | TBD MB | TBD% | Connected | Ethernet | 18h |
| 24:00 | TBD MB | TBD% | Connected | Ethernet | 24h |

**判定标准**:
- [ ] 空闲堆内存波动 < 10%
- [ ] 无内存泄漏趋势
- [ ] 无异常重启
- [ ] 所有外设持续正常

---

### 4. 网络切换压力测试

**测试场景**:

| 场景 | 操作步骤 | 预期结果 | 实际用时 |
|------|---------|---------|---------|
| Wi→ETH | 禁用 Wi-Fi | 自动切换到以太网 | TBD s |
| ETH→4G | 拔掉网线 | 自动切换到 4G | TBD s |
| 4G→Wi | SIM 卡拔出 | 回到 Wi-Fi | TBD s |
| 断电重启 | 完全断电后上电 | 恢复原有配置 | TBD s |

---

## 📝 第五步：文档更新与交付

### ecm50a-networking.md 文档结构

> 📌 设备端组网文档已迁移至 FastBee-doc 仓库：`D:\project\gitlab\FastBee-doc\src\device\examples\ecm50a-networking.md`（线上地址：https://fastbee.cn/doc/device/examples/ecm50a-networking.html），本仓库 `docs/device/examples/` 不再维护该文档。

```markdown
# ECM50-A 网络设备文档

## 1. 硬件规格
- ESP32-S3 R8 + 8MB PSRAM
- GPIO 分配表
- 接口接线图

## 2. 多模网络配置

### 2.1 Wi-Fi 配置
- STA 模式参数
- AP 热点参数
- 自动重连机制

### 2.2 以太网配置
- PHY 初始化
- DHCP/静态 IP
- Link 检测

### 2.3 4G 配置
- PPP 拨号
- AT 命令集
- 信号强度监测

### 2.4 网络切换逻辑
- 优先级策略
- 断网恢复时间
- 故障诊断

## 3. 外设配置示例

### 3.1 AI 模拟输入
- GPIO6/7配置
- 4-20mA 校准
- 采样率设置

### 3.2 DI 数字输入
- GPIO4/5配置
- NPN/PNP 适配
- 中断检测

### 3.3 继电器输出
- GPIO15/16配置
- NC 常闭模式
- 延时控制

### 3.4 Modbus RTU 主站
- RS485接线
- 子设备配置
- 轮询任务设置

## 4. 故障排查指南
- 常见问题 Q&A
- 日志分析
- 复位操作

## 5. 性能测试报告
- Web 性能数据
- MQTT 可靠性
- 长期稳定性曲线
- 网络切换测试视频
```

---

## 🎯 最终验收清单

### 基础部署 ✓
- [ ] 固件成功烧录到 16MB Flash
- [ ] 8MB PSRAM 识别正常
- [ ] LittleFS 文件系统挂载成功

### 外设功能 ✓
- [ ] AI1/AI2 采集精度达标 (±0.1mA)
- [ ] DI1/DI2 中断检测正常
- [ ] DO1/DO2 继电器切换可靠
- [ ] DHT11 Modbus 子设备数据准确
- [ ] 继电器模块 NC 模式正确

### 网络功能 ✓
- [ ] Wi-Fi STA/AP 双模式
- [ ] 以太网 Link 稳定
- [ ] 4G PPP 拨号成功
- [ ] 网络优先级切换流畅
- [ ] MQTT 连接云端成功

### 性能指标 ✓
- [ ] Web 访问延迟 < 500ms
- [ ] MQTT 传输成功率 ≥ 99.9%
- [ ] Free Heap 始终 > 100KB
- [ ] 24 小时无异常重启
- [ ] 无任何错误日志

### 文档交付 ✓
- [ ] ecm50a-networking.md 已完成（维护于 FastBee-doc 仓库 `src/device/examples/`）
- [ ] 关键日志截图存档
- [ ] 性能测试报告生成
- [ ] 最佳实践建议总结

---

## 🔗 相关资源

- [FastBee-Arduino GitHub](https://github.com/fastbee/FastBee-Arduino)
- [ECM50-A 组网文档（设备端文档，FastBee-doc）](https://fastbee.cn/doc/device/examples/ecm50a-networking.html)
- ECM50-A Product Manual（向硬件供应商获取）
- ECM50-A Pin List（向硬件供应商获取）
- ESP32-S3 Datasheet（乐鑫官网 https://www.espressif.com）

---

**文档版本**: 1.0  
**更新日期**: 2026-08-20  
**适用型号**: ECM50-A06/07/08/09
