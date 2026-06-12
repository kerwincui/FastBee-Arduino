# FastBee-Arduino 用户操作手册

本文面向实际接线和 Web 配置使用，覆盖从首次启动到外设配置、外设执行、传感器联动、导入导出和排错的完整流程。

## Web 界面地图

FastBee 的 Web 管理界面以左侧菜单组织功能：先看仪表盘确认设备在线，再进入网络、外设、规则和协议页面完成配置。以下截图来自同一台 ESP32-S3 设备，便于对照实际页面操作。

![Web 控制台导航图](images/web-console-navigation-map.svg)

![设备监控仪表盘](images/fastbee-dashboard.png)

| 菜单 | 截图 | 主要用途 |
|---|---|---|
| 网络设置 | ![网络设置](system/images/network-settings.png) | 查看联网状态、配置 WiFi STA/AP、mDNS、静态 IP。 |
| 外设配置 | ![外设配置](system/images/peripheral-management.png) | 添加传感器、GPIO、ADC、Modbus 子设备等外设对象。 |
| 外设执行 | ![外设执行](system/images/periph-exec-management.png) | 创建定时、事件、平台触发等自动化规则。 |
| 通信协议 | ![通信协议](system/images/protocol-mqtt-config.png) | 配置 MQTT 云平台连接和 Modbus RTU 总线。 |
| 设备配置 | ![设备配置](system/images/device-config.png) | 修改设备名称、编号、NTP、开发环境开关等系统参数。 |
| 文件管理 | ![文件管理](system/images/file-management.png) | 浏览 LittleFS、导入导出配置、下载日志或备份文件。 |

如果只想先按截图熟悉页面入口，可阅读 [Web 控制台图文导览](web-console-visual-guide.md)。

## 1. 固件版本选择

本项目基于 **Arduino-ESP32 3.x**（ESP-IDF 5.1+）构建，支持 ESP32 全系列芯片，分为三个版本层级：

| 固件环境 | 版本层级 | 适用芯片 | 主要能力 |
|---|---|---|---|
| `esp32c3` | 精简版 (Lite) | ESP32-C3 | Web 管理、WiFi/MQTT、mDNS、GPIO、DHT、DS18B20、OLED、TM1637、配置导入/导出 |
| `esp32c6` | 精简版 (Lite) | ESP32-C6 | 同上，支持 WiFi 6 |
| `esp32` | 标准版 (Standard) | ESP32 | 精简版基础 + 以太网 W5500 + 4G EC801E + Modbus RTU 主站 + I2C 传感器 + RFID + 红外 + 命令脚本 |
| `esp32s3` | 标准版 (Standard) | ESP32-S3 | 同上，资源余量更好 |
| `esp32s3-full` | 全功能版 (Full) | ESP32-S3 | 标准版基础 + LoRa + BLE + OTA + 多用户 + 文件/日志管理 + RuleScript + TCP/HTTP/CoAP + 多语言 |

**选择建议**：
- 低成本批量部署：`esp32c3`（¥9-12）或 `esp32c6`（WiFi 6，¥12-15）
- 需要以太网、4G 或 Modbus：`esp32` 或 `esp32s3`
- 需要 OTA、多用户、文件/日志、RuleScript、多语言或 LoRa：`esp32s3-full`
- 详细对比见 [版本对比指南](system/edition-comparison.md)

![固件版本选型决策图](images/version-selection-guide.svg)

## 2. 快速开始

1. 使用部署脚本烧录匹配的 LittleFS 文件系统和固件。
2. 运行设备接口冒烟测试。
3. 首次启动后连接设备 AP，进入 Web 管理页面。
4. 在“网络设置”配置 WiFi。
5. 在“外设配置”添加或导入硬件外设，先保持 `enabled: false`，确认引脚后再启用。
6. 在“外设执行”创建采集、联动或控制规则。
7. 在“设备配置 / 高级配置”导出配置备份。

常用部署命令：

```powershell
# ESP32 标准版
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6

# ESP32-S3 标准版
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3 -Port COM6

# ESP32-S3 全功能版
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6
```

烧录后检查：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.4.1 -Profile standard
```

设备已连入局域网时，将 `BaseUrl` 替换为实际 IP；全功能版使用 `-Profile full`。

部署完成后，浏览器进入 Web 控制台应先看到仪表盘数据。若页面可访问但状态为空，通常是设备仍在启动或会话已过期，刷新并重新登录即可继续检查。

![部署后仪表盘检查](images/fastbee-dashboard.png)

## 3. 网络与 MQTT 平台对接

建议先确认“网络在线”，再配置 MQTT。MQTT 会使用当前活动网络传输：WiFi 场景走 `WiFiClient`，标准版/全功能版在以太网或 4G 在线时走对应适配器。

### 3.1 联网方式选择

| 联网方式 | 精简版 | 标准版 | 全功能版 | 操作要点 |
|---|---|---|---|---|
| WiFi STA/AP | ✅ | ✅ | ✅ | 首次进入 AP，配置路由器 SSID/密码后切换 STA |
| mDNS | ✅ | ✅ | ✅ | 网络正常后可尝试 `http://fastbee.local` |
| 以太网 W5500 | ❌ | ✅ | ✅ | 检查 SPI 引脚、供电和网线，保存后重启 |
| 4G EC801E | ❌ | ✅ | ✅ | 检查 UART 引脚、APN、SIM 卡、天线和信号 |
| LoRa 透传 | ❌ | ❌ | ✅ | 适合网关透传场景，需确认串口和对端在线 |

### 3.2 MQTT 配置顺序

1. 在“网络配置”页面保存并确认联网方式已连接。
2. 在“通信协议 / MQTT”填写服务器地址、端口、客户端 ID、用户名和密码。
3. 按平台要求配置发布/订阅主题。FastBee 平台或自建平台的主题和消息格式以平台产品定义为准。
4. 使用页面上的 MQTT 连接测试，确认返回 connected。
5. 创建外设执行规则时，平台触发使用 MQTT 下发，采集和执行结果通过 MQTT 上报。

更多主题和消息格式说明见 [MQTT 配置](protocols/mqtt-config.md) 和 [协议概览](protocols/README.md)。

![网络配置页面](system/images/network-settings.png)

![MQTT 配置页面](system/images/protocol-mqtt-config.png)

## 4. 外设配置基本规则

外设配置文件位于 `data/config/peripherals.json`，每个外设至少包含：

```json
{
  "id": "relay_01",
  "name": "继电器1",
  "type": 12,
  "enabled": false,
  "pinCount": 1,
  "pins": [26, 255, 255, 255, 255, 255, 255, 255],
  "params": {}
}
```

常用类型速查：

| 类型 | type | 常用模块 |
|---|---:|---|
| I2C | 2 | SHT31、AHT20、BH1750、OLED、BMP280、MPU6050 |
| GPIO 输入 | 11/13/14 | 按键、PIR、震动、干簧管、数字避障 |
| GPIO 输出 | 12 | LED、继电器、蜂鸣器、激光 |
| ADC | 15/26 | 光敏、土壤湿度、烟雾、雨滴、电压、电流 |
| SENSOR | 38 | DHT、HC-SR04、通用传感器占位 |
| ONE_WIRE | 44 | DS18B20 |
| NeoPixel | 45 | WS2812B |
| TM1637 | 47 | 四位数码管 |
| Modbus 设备 | 51 | RS485 从站控制（标准版/全功能版） |
| DEVICE_EVENT | 60 | 逻辑事件源 |

Web 页面新增外设时会按类型展开不同参数。建议先保存为禁用状态，确认引脚和供电后再启用。

![外设配置列表](system/images/peripheral-management.png)

![新增外设弹窗](system/images/peripheral-add-dialog.png)

![外设配置数据模型](images/peripheral-config-data-model.svg)

如果外设保存成功但运行不生效，按图检查页面字段、`peripherals.json`、运行时驱动和规则引用是否使用同一个外设 ID。

## 5. 外设执行基本规则

外设执行文件位于 `data/config/periph_exec.json`。一个规则由触发器和动作组成：

```json
{
  "id": "exec_temp_relay",
  "name": "温度过高打开继电器",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:dht11_01_temperature",
      "operatorType": 2,
      "compareValue": "35"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "relay_01",
      "actionType": 0,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "reportAfterExec": true
}
```

常用触发器：

| triggerType | 名称 | 用途 |
|---:|---|---|
| 0 | 平台触发 | MQTT/平台命令下发 |
| 1 | 定时触发 | 周期采集、定时控制 |
| 4 | 事件触发 | 按键、数据源阈值、系统事件 |
| 5 | 轮询触发 | 本地数据源条件判断；Modbus 轮询需标准版/全功能版 |

常用动作：

| actionType | 名称 | 用途 |
|---:|---|---|
| 0/1 | GPIO 高/低电平 | 继电器、LED、蜂鸣器 |
| 2/3 | 闪烁/呼吸 | LED、蜂鸣器提示 |
| 4/5 | PWM/DAC | 调光、调速、模拟输出 |
| 10 | 调用外设 | 步进电机、NeoPixel、UART 等复合动作 |
| 15 | 命令脚本 | 多步 GPIO/PWM/延时组合（标准版/全功能版） |
| 19 | 传感器读取 | ADC、DHT、DS18B20、HC-SR04、SHT31、AHT20、BH1750 等 |
| 21 | 触发事件 | 向平台或本地规则发出逻辑事件 |
| 24/25/26/27 | 显示动作 | TM1637、OLED 显示 |

外设执行页面重点核对规则是否启用、触发器来源、动作数量和目标外设。首次上线建议保持规则禁用，先通过“执行一次”观察日志和外设状态。

![外设执行规则列表](system/images/periph-exec-management.png)

![触发器时序对比](images/trigger-timing-comparison.svg)

![外设执行规则表单安全检查](images/rule-builder-safety-checklist.svg)

普通用户配置规则时可以先看这两张图：触发器时序图帮助选择平台、定时、事件或轮询；安全检查图帮助确认启用前是否具备日志、上报和回滚手段。

## 6. 传感器读取 actionValue

`actionType: 19` 使用 JSON 字符串描述读取方式。Web 表单会自动生成该字符串，也可以手动编辑。

```json
{
  "periphId": "sht31_i2c",
  "sensorCategory": "SHT31",
  "dataField": "temperature",
  "sensorLabel": "温度",
  "unit": "℃",
  "decimalPlaces": 1,
  "driverParams": {
    "addr": "0x44",
    "sda": 21,
    "scl": 22
  }
}
```

常用 `sensorCategory`：

| sensorCategory | 输出字段 | 资源建议 |
|---|---|---|
| `analog` | `voltage`、`value` | ESP32 可用 |
| `digital` | `value` | ESP32 可用 |
| `dht11` / `dht22` | `temperature`、`humidity` | ESP32 可用 |
| `ds18b20` | `temperature` | ESP32 可用 |
| `ultrasonic` | `distance` | ESP32 可用 |
| `current` | `current` | ESP32 可用 |
| `voltage` | `voltage` | ESP32 可用 |
| `SHT31` | `temperature`、`humidity` | ESP32 可用，轻量 I2C |
| `AHT20` | `temperature`、`humidity` | ESP32 可用，轻量 I2C |
| `BH1750` | `illuminance` | ESP32 可用，轻量 I2C |
| `BMP280` | `temperature`、`pressure`、`altitude` | 标准版/全功能版 |
| `MPU6050` | `accelX/Y/Z`、`temperature`、`gyroX/Y/Z` | 标准版/全功能版 |

## 7. 典型场景

![典型场景自动化闭环](images/scenario-automation-loop.svg)

下面的场景都可以按“采集、判断、执行、上报、复核”的闭环拆解。初次配置时先跑单传感器、单规则、单动作，确认后再组合显示、告警和平台上报。

### 温湿度超过阈值打开继电器

1. 配置 DHT11/DHT22/SHT31/AHT20。
2. 创建定时采集规则，`reportAfterExec` 设为 `true`。
3. 创建事件触发规则，`eventId` 使用 `ds:<外设ID>_<字段>`。
4. 动作选择继电器高电平或低电平，根据模块有效电平决定。

### 光照低于阈值开灯

1. 配置 BH1750 或光敏 ADC。
2. 定时读取 `illuminance` 或 `voltage`。
3. 事件触发 `operatorType: 3`，例如小于 `50`。
4. 动作打开继电器或 LED。

### 电流过载断电

1. 配置 ACS712 电流传感器 ADC。
2. 读取动作高级参数填写 `sensitivity`、`zeroOffset`、`vRef`、`adcMax`。
3. 事件触发 `current > 15`。
4. 动作关闭继电器，并可追加 `actionType: 21` 上报 `over_current` 事件。

### Modbus 传感器采集（标准版/全功能版）

1. 使用 `esp32`、`esp32s3` 或 `esp32s3-full` 固件。
2. 在“通信协议 / Modbus RTU”配置串口、波特率和从站地址。
3. 添加 Modbus 子设备或寄存器映射。
4. 在外设执行中使用轮询触发或 Modbus 动作读取数据，并通过 MQTT 上报。

## 8. 导入导出建议

- 所有文档示例默认 `enabled: false`，导入后先检查引脚再启用。
- 每次批量修改前导出 `peripherals.json` 和 `periph_exec.json`。
- 从全功能版导出的配置导入 Lite/Standard 时，高级功能字段会被忽略；Lite 不支持 Modbus、脚本、LoRa、OTA 等规则。
- I2C 模块可以共用同一组 SDA/SCL，但地址不能冲突。
- GPIO6-GPIO11 通常连接 Flash，不建议作为外设引脚。
- GPIO34-GPIO39 仅输入，适合 ADC/数字输入，不适合继电器输出。

全功能版可以直接在文件管理页查看和维护 LittleFS 文件，现场迁移前建议优先导出配置文件。

![文件管理页面](system/images/file-management.png)

配置导入导出建议按下图形成固定流程：每次修改先备份，导入前校验 JSON，导入后用仪表盘、日志和对应功能页复核。

![配置文件生命周期流程图](images/config-lifecycle-flow.svg)

## 9. 排错

| 现象 | 检查项 |
|---|---|
| Web 页面能打开但外设无动作 | 外设是否启用、引脚是否正确、继电器有效电平是否反相 |
| 传感器读数为空 | 采集规则是否启用、`periphId` 是否存在、采样间隔是否太短 |
| I2C 传感器初始化失败 | SDA/SCL 是否接反、地址是否正确、是否有上拉、电源是否为 3.3V |
| ADC 数值异常 | 是否超过 3.3V、是否需要分压、ADC 衰减和校准参数是否正确 |
| 阈值规则不触发 | 先确认采集规则已生成 `ds:<id>_<field>` 数据源，再配置事件触发 |
| MQTT 测试失败 | 先确认网络在线，再检查服务器、端口、ClientID、用户名/密码和主题规则 |
| 以太网/4G 菜单不可用 | 确认固件是否为标准版或全功能版；精简版默认关闭这些能力 |
| Modbus 页面或动作不可用 | 精简版默认关闭 Modbus，请切换 `esp32`、`esp32s3` 或 `esp32s3-full` |
| `fastbee.local` 无法访问 | 确认电脑和设备在同一局域网；必要时改用路由器分配的 IP |
| 找不到多语言切换 | Lite/Standard 默认单语言，完整多语言仅 Full 保留 |
