# 快速开始

本文面向第一次使用 FastBee-Arduino 的用户,按最短路径完成从烧录固件到创建联动规则的完整流程。全程无需编程,仅需 5 步即可让 ESP32 变成可控的物联网终端。

## 环境准备

### 必需工具
- **VSCode** + **PlatformIO IDE 插件** (推荐),或 PlatformIO CLI
- **ESP32 系列开发板** (ESP32/ESP32-S3/ESP32-C3/ESP32-C6/ESP32-S2)
- **USB 数据线** (支持数据传输)

### 可选工具
- DHT11 温湿度传感器
- 继电器模块
- 面包板和杜邦线

## 第一步: 烧录固件

### 1.1 选择固件版本

本项目基于 **Arduino-ESP32 3.x**（ESP-IDF 5.1+）构建，支持三个版本层级：

| 固件环境 | 版本层级 | 适用芯片 | 推荐用途 |
|---------|---------|---------|----------|
| `esp32c3` | 精简版 | ESP32-C3 | 低成本节点,¥9-12 |
| `esp32c6` | 精简版 | ESP32-C6 | WiFi 6 新一代低成本,¥12-15 |
| `esp32s2` | 精简版 | ESP32-S2 | WiFi only (无 BLE) |
| `esp32` | 标准版 | ESP32 | 以太网/4G/Modbus + 常规项目 (推荐) |
| `esp32s3` | 标准版 | ESP32-S3 | 高性能,支持 I2C/RFID/IR |
| `esp32s3-full` | 全功能版 | ESP32-S3 | OTA + 多用户 + RuleScript + 多语言 |

**建议**: 首次使用优先选择 `esp32` 标准版。低成本场景选择 `esp32c3`。

> **注意**: ESP32-C6 需使用 [pioarduino](https://github.com/pioarduino/platform-espressif32) 社区平台，详见 [版本对比指南](system/edition-comparison.md)。

### 1.2 构建 Web 文件系统

打开终端,进入项目目录:

```powershell
cd D:\project\gitee\FastBee-Arduino
```

生成并上传 Web 资源:

```powershell
node scripts/gzip-www.js --web-slim --no-monitor
```

> 如只需生成不上传,添加 `--no-upload` 参数。

### 1.3 编译并烧录固件

```powershell
pio run -e esp32 --target upload
```

ESP32-S3 全功能版:

```powershell
pio run -e esp32s3-full --target upload
```

### 1.4 查看串口日志

```powershell
pio device monitor -e esp32 -b 115200
```

应看到芯片信息、内存状态和启动日志。

## 第二步: 初始配置

### 2.1 连接设备

1. 设备首次启动或 WiFi 未配置时,自动进入 **AP 模式**
2. 手机/电脑 WiFi 连接设备热点: `FastBee-XXXX` (无密码)
3. 浏览器访问: `http://192.168.4.1` 或 `http://fastbee.local`
4. 使用默认账号登录:
   - 用户名: `admin`
   - 密码: `admin123`

### 2.2 配置网络

1. 进入 **网络配置** 页面
2. 选择联网方式:
   - **WiFi STA** (所有版本): 填写您的 WiFi SSID 和密码
   - **以太网** (标准版/全功能版): 确认 W5500 SPI 引脚
   - **4G** (标准版/全功能版): 填写 APN 和串口引脚
   - **LoRa** (全功能版): 确认串口引脚和网关在线
3. 点击 **保存**,设备重启并连接网络
4. 连接成功后,可通过路由器分配的 IP 或 `http://fastbee.local` 访问

> 精简版默认只保留 WiFi/mDNS/MQTT 和基础外设能力。如需以太网、4G 或 Modbus,请选择 `esp32` 或 `esp32s3` 标准版。

## 第三步: 配置外设

### 3.1 添加 DHT11 温湿度传感器

1. 进入 **外设配置** 页面
2. 点击 **添加外设**
3. 填写配置:
   - **外设ID**: `dht_01`
   - **名称**: `DHT11温湿度`
   - **类型**: `SENSOR` (type: 38)
   - **引脚**: `13` (根据实际接线修改)
   - **启用**: 先保持禁用,确认引脚后再启用
4. 点击 **保存**

**JSON 配置示例**:

```json
{
  "id": "dht_01",
  "name": "DHT11温湿度",
  "type": 38,
  "enabled": false,
  "pinCount": 1,
  "pins": [13, 255, 255, 255, 255, 255, 255, 255],
  "params": {}
}
```

### 3.2 添加继电器

1. 点击 **添加外设**
2. 填写配置:
   - **外设ID**: `relay_01`
   - **名称**: `高温联动继电器`
   - **类型**: `GPIO_DIGITAL_OUTPUT` (type: 12)
   - **引脚**: `25` (根据实际接线修改)
   - **初始状态**: `0` (低电平)
3. 点击 **保存**

**JSON 配置示例**:

```json
{
  "id": "relay_01",
  "name": "高温联动继电器",
  "type": 12,
  "enabled": false,
  "pinCount": 1,
  "pins": [25, 255, 255, 255, 255, 255, 255, 255],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### 3.3 启用外设

1. 确认引脚接线正确
2. 在外设列表中勾选 **启用**
3. 点击 **保存全部**
4. 串口日志应显示外设初始化成功

## 第四步: 创建联动规则

### 4.1 温度超限报警规则

1. 进入 **外设执行** 页面
2. 点击 **添加规则**
3. 填写基本信息:
   - **规则ID**: `exec_temp_relay`
   - **名称**: `温度大于30度打开继电器`
   - **启用**: 先保持禁用,测试后启用

### 4.2 配置触发器

1. 添加触发器:
   - **触发类型**: `定时触发` (triggerType: 1)
   - **触发外设**: `dht_01`
   - **时间模式**: `周期触发`
   - **间隔秒数**: `10` (每 10 秒读取一次)
2. 点击 **保存触发器**

### 4.3 配置动作

**动作1: 读取温度传感器**

1. 添加动作:
   - **目标外设**: `dht_01`
   - **动作类型**: `传感器读取` (actionType: 19)
   - **动作参数**: 
     ```json
     {
       "periphId": "dht_01",
       "sensorCategory": "dht11",
       "dataField": "temperature",
       "sensorLabel": "温度",
       "unit": "°C",
       "decimalPlaces": 1
     }
     ```
2. 点击 **保存动作**

**动作2: 打开继电器**

1. 添加动作:
   - **目标外设**: `relay_01`
   - **动作类型**: `GPIO高电平` (actionType: 0)
   - **同步延时**: `100` 毫秒
2. 点击 **保存动作**

### 4.4 保存并启用规则

1. 设置 **执行后上报**: `true`
2. 点击 **保存规则**
3. 测试规则逻辑正确后,勾选 **启用**
4. 点击 **保存全部**

**完整 JSON 配置**:

```json
{
  "id": "exec_temp_relay",
  "name": "温度大于30度打开继电器",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "dht_01",
      "timerMode": 0,
      "intervalSec": 10,
      "timePoint": "",
      "eventId": "",
      "operatorType": 0,
      "compareValue": "",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "dht_01",
      "actionType": 19,
      "actionValue": "{\"periphId\":\"dht_01\",\"sensorCategory\":\"dht11\",\"dataField\":\"temperature\",\"sensorLabel\":\"温度\",\"unit\":\"°C\",\"decimalPlaces\":1}",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    },
    {
      "targetPeriphId": "relay_01",
      "actionType": 0,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 100,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

## 第五步: 配置云端连接 (可选)

### 5.1 MQTT 配置

1. 进入 **通信协议** 页面
2. 选择 **MQTT** 标签页
3. 填写配置:
   - **服务器地址**: `mqtt.your-server.com`
   - **端口**: `1883` (非加密) 或 `8883` (TLS)
   - **客户端ID**: 自动生成或手动填写
   - **用户名/密码**: 根据服务器要求
   - **主题前缀**: `fastbee/device01`
4. 点击 **保存并连接**
5. 查看连接状态,确认成功

### 5.2 验证数据上报

1. 使用 MQTT 客户端工具订阅主题: `fastbee/device01/#`
2. 等待规则执行(每 10 秒)
3. 应收到温度数据和继电器状态消息

## 配置备份

### 导出配置

1. 进入 **设备配置 > 高级配置**
2. 点击 **导出配置**
3. 选择导出类型:
   - **外设配置**: `peripherals.json`
   - **执行规则**: `periph_exec.json`
   - **网络配置**: `network.json`
   - **全部配置**: 打包下载
4. 保存到本地备份

### 导入配置

1. 点击 **导入配置**
2. 选择本地 JSON 文件
3. 确认导入
4. 检查引脚配置,确认无误后启用外设和规则

> **重要**: 所有文档示例默认 `enabled: false`,导入后先检查引脚再启用。

## 注意事项

### 引脚分配原则
- **GPIO34-39**: 仅输入,适合 ADC/数字输入
- **GPIO6-11**: 连接 Flash,不建议使用
- **GPIO32-39**: ADC1,WiFi 开启后可用
- **GPIO4,12-15,25-27**: ADC2,WiFi 开启后不可用

### 常见注意事项
- ADC 采集建议使用 ADC1 引脚 (GPIO32-39)
- 继电器模块可能低电平有效,可使用 `ACTION_HIGH_INVERTED` (actionType: 13)
- DHT11 采样间隔建议 >= 2 秒
- I2C 设备共用 SDA/SCL,但地址不能冲突
- 每次完成配置后导出备份

## 下一步

- 查看 [示例文档](examples/README.md) 学习更多传感器配置
- 阅读 [外设配置指南](peripheral-configuration-guide.md) 了解所有外设类型
- 参考 [用户手册](user-manual.md) 掌握高级功能
- 了解 [架构设计](architecture.md) 深入理解系统原理

---

**文档版本**: v2.1  
**最后更新**: 2026-06-03  
**维护者**: FastBee团队
