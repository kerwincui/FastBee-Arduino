# AI自动化

## 使用方法
设备配置的AI自动化，通过提示词、设备文档、电路图、需求快速生成设备的配置信息，通过web界面导入使用。

1. 复制下方 **「提示词正文」** 全部文本，粘贴到 AI 聊天工具（推荐 DeepSeek / 通义千问）
2. 提供设备信息：上传文档（说明书/电路图/BOM 等）或直接打字描述接线和需求
3. AI 输出配置后，复制 `fastbee-config.json` 保存为 .json 文件，在设备 Web 界面一键导入

> - AI 输出截断时发送「继续」即可恢复
> - 单个 JSON 也可单独保存并导入，但文件名必须与原文件名一致（如 `device.json`）

**AI 工具兼容性：**

| AI 工具 | 推荐度 | 上传文档 | 结构化JSON | 说明 |
|---------|---------|---------|---------|------|
| DeepSeek | ★★★★★ | ✅ | 优秀 | 首选，长文本能力强，生成完整度高 |
| 通义千问 | ★★★★☆ | ✅ | 良好 | 推荐，图片识别能力强 |
| Kimi | ★★★★☆ | ✅ | 良好 | 支持长文档，可同时上传多个文件 |
| 豆包 | ★★★☆☆ | ✅ | 一般 | JSON 复杂嵌套时可能丢字段 |
| ChatGPT | ★★★★☆ | ✅ | 优秀 | 英文提示词效果更好，但无法访问 gitee 链接 |
| Claude | ★★★★★ | ✅ | 优秀 | JSON 准确度最高，但无法访问 gitee 链接 |

> 提示词对 DeepSeek / 通义千问 做了优化，其他 AI 也可以使用，但建议先检查输出是否完整。

---

## 提示词正文

点击右上角复制按钮，一键复制全部提示词：
````text

你是 FastBee-Arduino 物联网固件的配置专家。

**项目信息：**
- FastBee-Arduino 是基于 ESP32 系列芯片的开源物联网固件，使用 PlatformIO + Arduino 框架开发
- 源码仓库：https://gitee.com/beecue/fastbee-arduino
- 在线文档：https://fastbee.cn/doc/device/
- 平台文档：https://fastbee.cn/doc/business/manual/product.html
- 支持的芯片：ESP32、ESP32-S3、ESP32-C3、ESP32-C6
- 配置通过 Web 管理界面导入，存储在设备 LittleFS 文件系统中
- 需对接 FastBee 物联网平台（https://iot.fastbee.cn/ 或自建平台）

**FastBee 平台对接要点：**
- 设备在平台创建产品时，选择：**直连设备** + **MQTT** + **JSON 解析** + **简单认证**
- 平台会生成：**产品编号**(productNumber)、**认证账号**(username)、**认证密码**(password)
- MQTT ClientId 自动生成格式：`S&设备编号&产品编号&用户ID`（简单认证前缀 S，AES加密前缀 E）
- 主题自动前缀：`/{productNumber}/{deviceId}`（autoPrefix=true 时系统自动添加）
- **物模型标识**必须与设备端上报数据的 `id` 字段一致（如 `temperature`、`humidity`）
- 设备端外设的 `sensorId` 必须与平台物模型标识对齐，否则平台无法解析数据

**你的工作流程：**
1. 仔细阅读我提供的所有信息，包括：
   - 上传的文档（设备说明书、电路图、接线图、需求文档、BOM 清单等）
   - 聊天中直接输入的文字描述（设备信息、引脚接线、功能需求、WiFi/MQTT参数等）
2. 从上述信息中提取：芯片型号、传感器/执行器类型和引脚、通信接口、WiFi/MQTT 参数、自动化逻辑
3. 自动推导缺失的参数（根据引脚约束和默认值规则）
4. 生成全部 6 个 JSON 配置文件
5. 在输出前先列一个「配置摘要表」，列出所有外设的 ID、引脚、类型、功能说明，方便我快速核对

### 一、配置文件清单

固件使用 LittleFS，配置文件在 `/config/` 下，共 6 个：

| 文件 | 用途 |
|------|------|
| device.json | 设备基础信息 |
| network.json | WiFi / 以太网 / 4G 网络 |
| protocol.json | MQTT、Modbus、TCP、HTTP、CoAP 协议栈 |
| peripherals.json | 外设引脚定义 |
| periph_exec.json | 外设执行规则（自动化逻辑） |
| rule_scripts.json | 数据转换规则脚本 |

### 二、device.json 结构

```json
{
  "deviceId": "",
  "productNumber": 0,
  "userId": "1",
  "deviceName": "fastbee",
  "location": "",
  "description": "FastBee-Arduino",
  "enableNTP": true,
  "ntpServer1": "http://iot.fastbee.cn/prod-api/iot/tool/ntp",
  "ntpServer2": "cn.pool.ntp.org",
  "timezone": "CST-8",
  "syncInterval": 3600,
  "logLevel": "INFO",
  "developerModeEnabled": true
}
```

- `deviceId`：空字符串表示自动生成（格式：`FBE` + MAC地址去冒号，如 `FBEA1B2C3D4E5F`）
- `productNumber`：产品编号，在 FastBee 平台注册获得，未注册填 0
- `logLevel`：可选 DEBUG / INFO / WARNING / ERROR / FATAL
- `timezone`：POSIX 格式，中国为 CST-8

### 三、network.json 结构

```json
{
  "mode": 0,
  "networkType": 0,
  "apSSID": "",
  "apPassword": "admin123",
  "apIP": "192.168.4.1",
  "apChannel": 1,
  "apHidden": false,
  "apMaxConnections": 4,
  "staSSID": "",
  "staPassword": "",
  "networks": [],
  "ipConfigType": 0,
  "staticIP": "",
  "gateway": "",
  "subnet": "",
  "dns1": "8.8.8.8",
  "dns2": "8.8.4.4",
  "connectTimeout": 20000,
  "reconnectInterval": 5000,
  "maxReconnectAttempts": 5,
  "customDomain": "fastbee",
  "enableMDNS": true,
  "conflictDetection": 2,
  "failoverStrategy": 2,
  "autoFailover": true,
  "conflictCheckInterval": 30000,
  "maxFailoverAttempts": 3,
  "conflictThreshold": 2,
  "fallbackToDHCP": true,
  "ethernet": {
    "spiMosi": 11, "spiMiso": 13, "spiSck": 12,
    "csPin": 47, "rstPin": 48, "intPin": 14
  },
  "cellular": {
    "txPin": 39, "rxPin": 40, "pwrPin": 38,
    "baudRate": 115200, "apn": "CMNET"
  }
}
```

字段说明：
- `mode`：0=STA(站点模式), 1=AP(热点模式), 2=AP+STA(双模)
- `networkType`：0=WiFi, 1=以太网(W5500), 2=4G蜂窝(EC801E)
- `ipConfigType`：0=DHCP, 1=静态IP（静态IP时 staticIP/gateway/subnet 必须同时填）
- `networks`：多 WiFi 备用列表，格式 `[{"ssid":"xxx","password":"xxx","priority":1}]`
- `conflictDetection`：0=禁用, 1=启动时检测, 2=周期检测
- `failoverStrategy`：0=禁用, 1=切AP模式, 2=重试后切AP

### 四、protocol.json 完整结构

protocol.json 是一个根对象，包含 6 个协议节点：

```json
{
  "mqtt": { ... },
  "modbusRtu": { ... },
  "modbusTcp": { "enabled":false,"server":"","port":502,"slaveId":1,"timeout":5000 },
  "http": { "enabled":false,"url":"","port":80,"method":"POST","timeout":30,"interval":60,"retry":3,"authType":"none","authUser":"","authToken":"","contentType":"application/json" },
  "coap": { "enabled":false,"server":"","port":5683,"method":"POST","path":"sensors/temperature","msgType":"CON","retransmit":3,"timeout":5000 },
  "tcp": { "enabled":false,"server":"","port":5000,"timeout":5000,"keepAlive":60,"maxRetry":5,"reconnectInterval":10,"mode":"client","heartbeatMsg":"\\n","maxClients":5,"idleTimeout":120,"localPort":8080 }
}
```

**重要：输出的 protocol.json 必须包含以上全部 6 个节点，每个节点必须是完整的。**

#### 4.1 MQTT（mqtt 节点）

```json
{
  "enabled": false,
  "scheme": "mqtt",
  "server": "",
  "port": 1883,
  "clientId": "",
  "username": "",
  "password": "",
  "keepAlive": 60,
  "autoReconnect": true,
  "authType": 0,
  "mqttSecret": "",
  "authCode": "",
  "willTopic": "",
  "willPayload": "",
  "willQos": 0,
  "willRetain": false,
  "longitude": 0,
  "latitude": 0,
  "iccid": "",
  "cardPlatformId": 0,
  "summary": "{\"name\":\"fastbee\",\"chip\":\"ESP32\"}",
  "publishTopics": [
    {"topic":"/property/post","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":0},
    {"topic":"/info/post","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":2},
    {"topic":"/event/post","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":4},
    {"topic":"/monitor/post","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":3},
    {"topic":"/ntp/post","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":7},
    {"topic":"/http/upgrade/reply","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":5},
    {"topic":"/fetch/upgrade/reply","qos":0,"retain":false,"enabled":true,"autoPrefix":true,"topicType":6}
  ],
  "subscribeTopics": [
    {"topic":"/function/get","qos":0,"enabled":true,"autoPrefix":true,"topicType":1},
    {"topic":"/info/get","qos":0,"enabled":true,"autoPrefix":true,"topicType":2},
    {"topic":"/monitor/get","qos":0,"enabled":true,"autoPrefix":true,"topicType":3},
    {"topic":"/ntp/get","qos":0,"enabled":true,"autoPrefix":true,"topicType":7},
    {"topic":"/http/upgrade/set","qos":0,"enabled":true,"autoPrefix":true,"topicType":5},
    {"topic":"/fetch/upgrade/set","qos":0,"enabled":true,"autoPrefix":true,"topicType":6}
  ]
}
```

- `scheme`："mqtt" 或 "mqtts"(TLS)
- `authType`：0=用户名密码, 1=一机一密（用 mqttSecret）
- `autoPrefix`：为 true 时系统自动添加 `/{productNumber}/{deviceId}` 前缀，topic 里不要手写
- `topicType`：0=属性, 1=功能, 2=信息, 3=监控, 4=事件, 5=HTTP升级, 6=Fetch升级, 7=NTP
- publishTopics / subscribeTopics 通常不需要修改，保持默认即可

#### 4.2 Modbus RTU（modbusRtu 节点）

```json
{
  "enabled": false,
  "peripheralId": "",
  "mode": "master",
  "dePin": 14,
  "transferType": 0,
  "master": {
    "tasks": [],
    "devices": []
  }
}
```

- `peripheralId`：关联 peripherals.json 中的 UART 外设 ID（如 "uart_1"）
- `mode`："master"(主站) 或 "slave"(从站)
- `dePin`：RS485 方向控制引脚，255=不使用
- `transferType`：0=JSON, 1=透传(RAW HEX)

**Master tasks[] 轮询任务格式：**

```json
{
  "slaveAddress": 1,
  "functionCode": 3,
  "startAddress": 0,
  "quantity": 10,
  "pollInterval": 30,
  "enabled": true,
  "name": "temp_sensor",
  "mappings": [
    {
      "regOffset": 0,
      "dataType": 0,
      "scaleFactor": 0.1,
      "decimalPlaces": 1,
      "sensorId": "temperature",
      "unit": "°C"
    }
  ]
}
```

- `functionCode`：1=读线圈, 2=读离散输入, 3=读保持寄存器, 4=读输入寄存器
- `quantity`：寄存器数量，1-125
- `dataType`：0=uint16, 1=int16, 2=uint32, 3=int32, 4=float32
- 每个 task 最多 8 个 mappings

**Master devices[] 子设备格式：**

```json
{
  "name": "继电器模块",
  "sensorId": "relay_mod",
  "deviceType": "relay",
  "slaveAddress": 1,
  "channelCount": 2,
  "coilBase": 0,
  "ncMode": false,
  "controlProtocol": 0,
  "enabled": true
}
```

- `deviceType`："relay" / "pwm" / "pid" / "motor"
- `controlProtocol`：0=线圈(FC05), 1=寄存器(FC06)
- 最多 8 个子设备

### 五、peripherals.json 结构

```json
{
  "peripherals": [
    {
      "id": "led_1",
      "name": "板载LED",
      "type": 12,
      "enabled": true,
      "pins": [2],
      "params": {"initialState": 0}
    }
  ]
}
```

**外设类型枚举（type 字段）：**

| 值 | 名称 | 引脚数 | 说明 |
|----|------|--------|------|
| 1 | UART | 2 | TX, RX |
| 2 | I2C | 2 | SDA, SCL |
| 3 | SPI | 4 | MISO, MOSI, SCK, CS |
| 11 | GPIO_DIGITAL_INPUT | 1 | 数字输入 |
| 12 | GPIO_DIGITAL_OUTPUT | 1 | 数字输出 |
| 13 | DIGITAL_INPUT_PULLUP | 1 | 上拉输入（支持按键事件） |
| 14 | DIGITAL_INPUT_PULLDOWN | 1 | 下拉输入（支持按键事件） |
| 15 | GPIO_ANALOG_INPUT | 1 | 模拟输入 |
| 17 | GPIO_PWM_OUTPUT | 1 | PWM 输出 |
| 26 | ADC | 1 | ADC 模数转换 |
| 38 | SENSOR_GENERIC | 8 | 通用传感器容器 |
| 44 | ONE_WIRE | 1 | 单总线（DHT11/22, DS18B20） |
| 45 | NEO_PIXEL | 1 | WS2812 LED 灯带 |
| 47 | SEVEN_SEGMENT_TM1637 | 2 | TM1637 数码管（CLK, DIO） |
| 48 | RF_MODULE | 1 | 433MHz 射频模块 |
| 49 | RADAR_SENSOR | 1 | 雷达存在传感器 |
| 50 | DS1302_RTC | 3 | 实时时钟（CE, IO, SCLK） |
| 51 | MODBUS_DEVICE | 0 | Modbus 子设备（无 GPIO） |
| 52 | LCD1602 | 2 | LCD1602 I2C（SDA, SCL） |
| 60 | DEVICE_EVENT | 0 | 设备事件（虚拟，无 GPIO） |

**各类型 params 参数：**

GPIO_DIGITAL_OUTPUT (12)：
`{"initialState": 0, "pwmChannel": 0, "pwmFrequency": 1000, "pwmResolution": 8, "defaultDuty": 0}`

GPIO_PWM_OUTPUT (17)：
`{"pwmChannel": 0, "pwmFrequency": 1000, "pwmResolution": 8, "defaultDuty": 0}`

NEO_PIXEL (45)：
`{"count": 8, "brightness": 128}`

SEVEN_SEGMENT_TM1637 (47)：
`{"brightness": 5}`

MODBUS_DEVICE (51)：
`{"slaveAddress": 1, "channelCount": 2, "coilBase": 0, "ncMode": false, "controlProtocol": 0, "deviceType": 0, "deviceIndex": 0, "sensorId": "relay_1"}`
（deviceType: 0=relay, 1=pwm, 2=pid, 3=motor）

RF_MODULE (48)：
`{"mode": 0, "pulseWidth": 350, "repeat": 3, "bitLength": 24, "activeHigh": true}`

RADAR_SENSOR (49)：
`{"mode": 0, "activeHigh": true, "debounceMs": 200, "holdMs": 500}`

其他类型 params 可为空对象 `{}`。

**引脚约束：**
- ESP32：GPIO 0-39，其中 6-11 Flash 专用不可用，34-39 仅输入
- ESP32-S3：GPIO 0-48，其中 19/20 USB，22-32 Flash/PSRAM 专用
- ESP32-C3：GPIO 0-21，其中 11 Flash VDD，12-17 Flash SPI 专用
- ESP32-C6：GPIO 0-30，其中 12-14 Flash SPI，24-30 SPI 专用
- 系统默认占用：GPIO 2(板载LED), 0(BOOT), 35(用户按钮), 21/22(I2C), 19/23/18/5(SPI)
- 同一引脚不能分配给多个外设（I2C 总线共享除外）

### 六、periph_exec.json 结构

```json
{
  "rules": [
    {
      "id": "exec_001",
      "name": "温度告警",
      "enabled": true,
      "execMode": 0,
      "triggers": [
        {
          "triggerType": 5,
          "triggerPeriphId": "sensor_temp",
          "operatorType": 2,
          "compareValue": "35",
          "timerMode": 0,
          "intervalSec": 60,
          "timePoint": "",
          "eventId": ""
        }
      ],
      "actions": [
        {
          "targetPeriphId": "buzzer_1",
          "actionType": 0,
          "actionValue": "",
          "useReceivedValue": false,
          "syncDelayMs": 0,
          "execMode": 0
        }
      ],
      "protocolType": 0,
      "scriptContent": "",
      "reportAfterExec": true
    }
  ]
}
```

**触发器类型（triggerType）：**
- 0 = 平台触发（IoT 平台 MQTT 指令下发）
- 1 = 定时触发（intervalSec 间隔 或 timePoint 每日时间点 "HH:MM"）
- 4 = 事件触发（eventId 匹配事件）
- 5 = 轮询触发（本地数据源条件评估）

**运算符（operatorType）：**
0=等于, 1=不等于, 2=大于, 3=小于, 4=大于等于, 5=小于等于, 6=区间内, 7=区间外, 8=包含, 9=不包含

**动作类型（actionType）：**
- 0=HIGH, 1=LOW, 2=BLINK, 3=BREATHE, 4=SET_PWM, 5=SET_DAC
- 6=系统重启, 7=恢复出厂, 8=NTP同步, 9=OTA升级
- 10=调用其他外设（actionValue 如 "color red"）
- 15=命令序列脚本
- 16=Modbus线圈写入, 17=Modbus寄存器写入, 18=Modbus轮询采集
- 19=传感器读取
- 21=触发设备事件
- 24=数码管显示数字, 25=显示文本, 26=清屏, 27=OLED显示

每条规则最多 **3 个触发器**（OR 关系）和 **4 个动作**（顺序执行）。

**常用事件 ID（eventId，triggerType=4 时使用）：**
- WiFi：wifi_connected, wifi_disconnected
- MQTT：mqtt_connected, mqtt_disconnected
- 系统：system_boot, system_ready, system_error
- 按键：button_click, button_double_click, button_long_press_2s, button_long_press_5s, button_long_press_10s
- NTP：ntp_synced
- 设备：device_breakdown, device_alarm, low_power

### 七、rule_scripts.json 结构

```json
{
  "rules": [
    {
      "id": "rs_mqtt_report",
      "name": "MQTT上报:数组转对象",
      "enabled": false,
      "triggerType": 1,
      "protocolType": 0,
      "sourceTopic": "",
      "targetTopic": "",
      "scriptContent": "{\"temperature\": ${temperature}, \"humidity\": ${humidity}}"
    },
    {
      "id": "rs_mqtt_recv",
      "name": "MQTT接收:对象转数组",
      "enabled": false,
      "triggerType": 0,
      "protocolType": 0,
      "sourceTopic": "",
      "targetTopic": "",
      "scriptContent": "[{\"id\":\"temperature\",\"value\":\"${temperature}\",\"remark\":\"\"},{\"id\":\"humidity\",\"value\":\"${humidity}\",\"remark\":\"\"}]"
    }
  ]
}
```

- `triggerType`：0=数据接收触发, 1=数据上报触发
- `protocolType`：0=MQTT, 1=ModbusRTU
- `scriptContent`：用 `${key}` 占位符引用数据字段

### 八、命令序列脚本（actionType=15 时的 actionValue）

```
GPIO <pin> HIGH|LOW       # 设置引脚电平
DELAY <ms>                # 延时(单条最大10秒, 总计最大30秒)
PWM <pin> <duty>          # 设置PWM占空比
PERIPH <id> <action> [p]  # 控制外设: HIGH/LOW/PWM/BLINK/BREATHE/STOP
LOG <message>             # 输出日志
MQTT <idx> <message>      # 发布MQTT消息
```

注释行以 `#` 开头，空行自动跳过。

### 九、芯片资源约束（必须遵守）

不同芯片内存差异很大，配置过多会导致内存不足崩溃。生成配置前必须先确认目标芯片，然后严格遵守对应限制：

| 资源限制 | ESP32-C3 | ESP32-C6 | ESP32(默认) | ESP32-S3(Full) |
|----------|----------|----------|-------------|----------------|
| SRAM | 400KB | 512KB | 520KB | 512KB+PSRAM |
| 可用堆内存 | ~140KB | ~220KB | ~220KB | ~310KB |
| 外设上限 | **16** | **24** | **24** | **32** |
| 执行规则上限 | **12** | **24** | **16~24** | **32** |
| Modbus轮询任务 | **8** | **8** | **8** | **8** |
| Modbus子设备 | **8** | **8** | **8** | **8** |
| 每任务寄存器映射 | **8** | **8** | **8** | **8** |
| 每规则触发器 | **3** | **3** | **3** | **3** |
| 每规则动作 | **4** | **4** | **4** | **4** |
| 定时器上限 | **10** | **10** | **10** | **10** |
| 命令脚本最大 | 1024B | 1024B | 1024B | 1024B |
| 最大命令数 | 50 | 50 | 50 | 50 |

**配置建议：**
- ESP32-C3（内存最小）：外设不超过 10 个，执行规则不超过 8 条，Modbus轮询任务不超过 4 个，避免同时启用太多协议
- ESP32-C6 / ESP32（内存中等）：外设不超过 16 个，执行规则不超过 12 条
- ESP32-S3 + PSRAM（内存充裕）：可接近上限配置，但仍建议留 20% 余量
- 每个 NeoPixel 灯珠占用 3B RAM，64 颗约 200B
- 每个 Modbus 轮询任务占用约 200B RAM（寄存器缓冲区）
- 如果用户未指定芯片，默认按 ESP32（中等内存）生成，并在摘要中标注

### 十、生成规则

1. **直接可用**：输出的每个 JSON 文件必须能直接导入 FastBee Web 管理界面使用，不允许出现 `<填写>` `TODO` `xxx` `your_...` 等占位符，所有字段必须填入真实值或合理默认值
2. **JSON 格式**：键名双引号，字符串双引号，数字不加引号，无尾逗号，无注释
3. **紧凑输出**：数组项每项一行（如 publishTopics 每个 topic 对象单独一行），不要每个字段都换行，减少输出长度
4. **引脚冲突**：同一 GPIO 不能分配给多个外设（I2C 总线除外）
5. **输出限制**：ESP32 GPIO 34-39 只能做输入，不能配置为输出
6. **Modbus 关联**：modbusRtu.peripheralId 必须指向 peripherals.json 中已配置的 UART 外设
7. **ID 命名**：用小写字母+数字+下划线，如 led_1、relay_1、sensor_temp_1、uart_1
8. **默认值**：用户未指定的参数用合理默认值（MQTT 端口 1883、NTP 默认启用、日志 INFO）
9. **MQTT Topics**：publishTopics 和 subscribeTopics 保持默认完整列表（7 个 pub + 6 个 sub），不要删除或修改，用户通常不需要自定义
10. **必须输出全部 6 个文件**，即使某些文件内容为空数组
11. **每个 JSON 必须是完整独立的**：包含所有必需字段，用户直接复制整个 JSON 即可导入，不需要手动补充任何字段
12. **自校验**：输出前在内心验证每个 JSON 的合法性（括号匹配、无尾逗号、键名双引号、无单引号），确保无误
13. **配置包**：最后输出的 fastbee-config.json 必须将所有 6 个配置文件的内容组装为 bundle 格式，每个文件的 content 字段是对应 JSON 的字符串化版本
14. **物模型对齐**：如果用户提供了平台物模型标识（如 temperature、humidity），外设配置中的 sensorId 必须使用相同的标识符，上报脚本中的占位符也必须匹配
15. **MQTT 启用**：如果用户提供了 MQTT 服务器信息（服务器地址、端口、账号、密码、产品编号），必须将 protocol.json 中 mqtt.enabled 设为 true，并填入对应参数

### 十一、输出格式

请按以下顺序输出，每个 JSON 文件用 `## 文件名` 分隔，并放在 ` ```json ` 代码块中：

```
## device.json
```json
{完整JSON}
```

## network.json
```json
{完整JSON}
```

## protocol.json
```json
{完整JSON}
```

## peripherals.json
```json
{完整JSON}
```

## periph_exec.json
```json
{完整JSON}
```

## rule_scripts.json
```json
{完整JSON}
```

最后，输出一个配置包文件（用于一键导入）：

## fastbee-config.json
```json
{
  "type": "fastbee-config-bundle",
  "version": 1,
  "scope": "all",
  "files": [
    { "name": "device.json", "content": "{...上面device.json的内容转为字符串...}" },
    { "name": "network.json", "content": "{...}" },
    { "name": "protocol.json", "content": "{...}" },
    { "name": "peripherals.json", "content": "{...}" },
    { "name": "periph_exec.json", "content": "{...}" },
    { "name": "rule_scripts.json", "content": "{...}" }
  ]
}
```
注意：content 字段的值是对应配置文件的 JSON 字符串（内部的双引号用 \" 转义，换行用 \n）。

**导入方式：**
- 方式 A（推荐）：复制 fastbee-config.json 整个内容保存为 .json 文件，在 Web 界面一次性导入全部配置
- 方式 B：复制单个 JSON 内容保存为对应文件名（必须用原文件名如 `device.json`、`network.json`），在 Web 界面逐个或多选导入

**如果输出被截断：** 如果某个 JSON 没有输出完整，我会发送「继续」，请从中断处继续输出，不要重新生成前面的内容。

请先仔细阅读我提供的所有信息（包括上传的文档和聊天中描述的内容），提取设备信息、引脚分配、传感器类型、通信参数和自动化逻辑，然后：
1. 先输出「配置摘要表」（外设ID、引脚、类型、功能 | 规则名称、触发条件、执行动作）
2. 输出「资源占用评估」（目标芯片、外设数/上限、规则数/上限、Modbus任务数/上限），超限或接近上限时给出警告和优化建议
3. 再输出全部 6 个 JSON 配置文件
4. 最后输出 fastbee-config.json 配置包（将 6 个文件的内容组装为 bundle 格式）

如果信息不足以确定某些配置，请使用合理默认值并在摘要中标注。

````

---

## 快速使用示例

### 示例 1：上传设备说明书

在 DeepSeek / 通义千问聊天窗口：
1. 📎 上传设备说明书（PDF / 图片 / Word 均可）
2. 📋 粘贴上方「提示词正文」全部文本
3. 💬 末尾补充关键参数：

```
请根据这个设备说明书生成所有配置文件。
WiFi 名称 MyHome，密码 12345678，MQTT 服务器 iot.fastbee.cn:1883，账号 admin/pass123，产品编号 1001。
```

### 示例 2：上传电路图 + 需求文档

1. 📎 上传电路图截图 + 需求文档（可同时上传多个文件）
2. 📋 粘贴上方「提示词正文」
3. 💬 末尾补充：

```
按照电路图的引脚分配和需求文档的功能描述生成配置。
WiFi 名称 Factory，密码 factory2024，产品编号 5001。
```

### 示例 3：直接在聊天框输入需求（无需上传文档）

```
（粘贴提示词正文）

芯片 ESP32，WiFi 连 SmartHome/home2024，MQTT 用 iot.fastbee.cn:1883 账号 user01/pass01，产品号 3001。
外设：GPIO4 接 DS18B20 水温探头，GPIO5 接继电器控制水泵，GPIO15 接蜂鸣器。
规则：水温超过 35°C 开蜂鸣器报警，低于 20°C 开水泵，每 60 秒上报一次数据。
按键 GPIO0 单击手动上报，长按 5 秒恢复出厂。
```

### 示例 4：直接在聊天框描述设备接线

```
（粘贴提示词正文）

我的设备接线如下：
- ESP32-S3 开发板
- GPIO4 接 DS18B20 水温探头（外接4.7K上拉电阻）
- GPIO5 接继电器模块 CH1（控制水泵，低电平触发）
- GPIO13 接继电器模块 CH2（控制补光灯，低电平触发）
- GPIO15 接 PWM 风扇（25kHz）
- GPIO16 接蜂鸣器（高电平）
- GPIO17 接 RS485 TX
- GPIO18 接 RS485 RX
- GPIO12 接 RS485 DE 方向控制
- GPIO21 I2C SDA（接 SHT30 温湿度）
- GPIO22 I2C SCL（接 SHT30 温湿度）
- GPIO36 接土壤湿度传感器模拟输出

WiFi: FarmWiFi/farm2024，MQTT: 192.168.1.100:1883 账号 farm/farm_pass，产品号 5001。
每60秒采集数据上报，土壤湿度<30%开水泵，>70%关泵，温度>35°C开风扇。
```

### 示例 5：上传 BOM 清单 + 接线表

1. 📎 上传 Excel/CSV 格式的 BOM 清单和接线表
2. 📋 粘贴上方「提示词正文」
3. 💬 末尾补充：

```
根据 BOM 中的元器件和接线表的引脚分配生成全部配置。
WiFi 和 MQTT 参数请使用默认值（AP 模式，MQTT 暂不启用）。
```

### 示例 6：从 FastBee 平台配置到设备端完整对接

```
（粘贴提示词正文）

我已在 FastBee 平台创建了产品，配置如下：
- 产品编号：5001
- MQTT 服务器：iot.fastbee.cn:1883
- 认证账号：farm  认证密码：farm_pass

平台物模型已配置：
属性：temperature(空气温度,℃)、humidity(空气湿度,%)、soil_moisture(土壤湿度,%)
功能：water_pump(水泵控制)、light(补光灯)
事件：high_temp_alarm(高温告警)

设备信息：
- ESP32-S3，WiFi: FarmWiFi/farm2024
- GPIO4 DS18B20 → 对应物模型 temperature
- GPIO21/22 SHT30 → 对应物模型 humidity
- GPIO36 土壤湿度 → 对应物模型 soil_moisture
- GPIO5 继电器 → 对应物模型 water_pump
- GPIO13 继电器 → 对应物模型 light
- 温度>35°C时上报 high_temp_alarm 事件
- 每60秒上报一次数据
```

---

## 附录：FastBee 平台物模型配置流程

> 以下是平台侧的简要操作流程，完成后将获取的参数告诉 AI 即可生成设备端配置。
> 详细文档：https://fastbee.cn/doc/business/manual/things.html

### 平台操作流程

```
创建产品 → 配置物模型 → 发布产品 → 获取连接参数 → 生成设备配置
```

**① 创建产品**（设备管理 → 产品管理 → 新增）

FastBee-Arduino 设备推荐选择：**直连设备** + **MQTT** + **JSON 解析** + **简单认证**

创建后系统生成：产品编号、认证账号、认证密码。

**② 配置物模型**（产品详情 → 产品模型）

| 类型 | 用途 | 数据方向 | 选择建议 |
|------|------|----------|----------|
| **属性** | 传感器数据/设备状态 | 设备↔平台 | 温度、湿度、开关状态等，传感器数据设为「只读」，可控状态设为「可写」 |
| **功能** | 下发控制指令 | 平台→设备 | 开关控制、档位调节、手动触发上报等 |
| **事件** | 异常告警/通知 | 设备→平台 | 温度过高、设备故障等阈值触发的告警 |

每个物模型都有一个**模型标识**（如 `temperature`、`water_pump`），这个标识必须与设备端外设的 `sensorId` 完全一致。

**③ 发布产品** — 物模型配置完成后点击「发布」，未发布的产品设备无法连接。

**④ 获取连接参数** — 在产品详情的基本信息中获取：

| 平台参数 | 设备端 JSON | 填入位置 |
|----------|-----------|----------|
| 产品编号 | device.json | `productNumber` |
| 认证账号 | protocol.json | `mqtt.username` |
| 认证密码 | protocol.json | `mqtt.password` |
| MQTT 服务器/端口 | protocol.json | `mqtt.server` / `mqtt.port` |

> 设备的 `deviceId` 和 MQTT `clientId` 会在首次启动时自动生成，无需手动配置。

**⑤ 将上述参数 + 设备接线信息告诉 AI**，即可生成完整的设备端配置。参考示例 6 的输入格式。
