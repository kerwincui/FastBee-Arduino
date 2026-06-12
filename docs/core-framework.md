# 核心框架

本文档详细描述 FastBee-Arduino 的核心框架组件、关键类结构和重要方法,为开发者提供深入的实现参考。

## FastBeeFramework 主框架

FastBeeFramework 是系统的核心控制器,负责初始化所有子模块并协调它们的工作。

### 初始化流程

```cpp
FastBeeFramework::getInstance()
  ├── loadConfig()              // 从 LittleFS 加载所有 JSON 配置
  ├── initPeripherals()         // 初始化所有启用的外设
  ├── initNetwork()             // 启动 WiFi/网络服务和 Web 服务器
  ├── initProtocols()           // 初始化 MQTT/Modbus 等协议
  ├── initPeriphExec()          // 启动规则引擎和异步执行
  └── startHealthMonitor()      // 启动健康监控和内存门控
```

### 关键方法

| 方法 | 说明 |
|------|------|
| `setup()` | 系统启动入口,按序调用所有初始化方法 |
| `loop()` | 主循环,处理定时任务和事件分发 |
| `getPeripheralManager()` | 获取外设管理器实例 |
| `getPeriphExecManager()` | 获取规则引擎实例 |
| `getProtocolManager()` | 获取协议管理器实例 |
| `getNetworkManager()` | 获取网络管理器实例 |

### 生命周期管理

```
启动 → setup() → 加载配置 → 初始化模块 → 进入 loop()
                                    ↓
                              处理定时任务
                              处理网络事件
                              处理协议消息
                              健康监控检查
                                    ↓
                              持续运行 (7x24)
```

## PeriphExecManager 规则引擎

PeriphExecManager 是实现"当条件满足时执行动作"的核心模块。

### 规则数据模型

```json
{
  "id": "exec_rule_01",
  "name": "规则名称",
  "enabled": true,
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
      "targetPeriphId": "relay_01",
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
```

### 触发器类型详解

| 类型 | 值 | 名称 | 说明 | 关键字段 |
|------|-----|------|------|---------|
| 平台触发 | 0 | PLATFORM_TRIGGER | MQTT下发数据触发 | operatorType, compareValue |
| 定时触发 | 1 | TIMER_TRIGGER | 周期/定点时间触发 | timerMode, intervalSec, timePoint |
| 事件触发 | 4 | EVENT_TRIGGER | 系统事件/数据源阈值 | eventId, operatorType, compareValue |
| 轮询触发 | 5 | POLL_TRIGGER | Modbus/本地数据条件 | triggerPeriphId, poll配置 |

**触发器关系**: 同一规则的多个触发器是 **OR** 关系,任一匹配即触发。

### 动作类型详解

| 值 | 枚举名称 | 分类 | actionValue 含义 |
|-----|---------|------|-----------------|
| 0 | ACTION_HIGH | GPIO | 忽略,设置目标引脚高电平 |
| 1 | ACTION_LOW | GPIO | 忽略,设置目标引脚低电平 |
| 2 | ACTION_BLINK | GPIO | 闪烁间隔毫秒(默认 500) |
| 3 | ACTION_BREATHE | GPIO | 呼吸灯周期毫秒(默认 2000) |
| 4 | ACTION_SET_PWM | GPIO | PWM 占空比 0-255 |
| 5 | ACTION_SET_DAC | GPIO | DAC 输出值 0-255 |
| 6 | ACTION_SYS_RESTART | 系统 | 忽略,延时 500ms 后重启设备 |
| 7 | ACTION_SYS_FACTORY_RESET | 系统 | 忽略,格式化 LittleFS 后重启 |
| 8 | ACTION_SYS_NTP_SYNC | 系统 | 忽略,触发 NTP 时间同步 |
| 10 | ACTION_CALL_PERIPHERAL | 外设 | 调用外设专用方法(JSON) |
| 13 | ACTION_HIGH_INVERTED | GPIO | 忽略,反相高电平(低有效) |
| 15 | ACTION_SCRIPT | 脚本 | 命令脚本内容 |
| 19 | ACTION_SENSOR_READ | 传感器 | 读取参数(JSON) |
| 21 | ACTION_TRIGGER_EVENT | 事件 | 触发逻辑事件 |
| 24 | ACTION_DISPLAY_TM1637 | 显示 | TM1637 显示参数 |
| 25 | ACTION_DISPLAY_OLED | 显示 | OLED 显示参数 |
| 26 | ACTION_DISPLAY_LCD | 显示 | LCD 显示参数 |
| 27 | ACTION_DISPLAY_ROTATE | 显示 | 轮换显示参数 |

**动作关系**: 同一规则的多个动作按数组顺序 **顺序执行**。

### 传感器读取 actionValue 格式

`actionType: 19` 使用 JSON 字符串描述读取方式:

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

**常用 sensorCategory**:

| sensorCategory | 输出字段 | 资源建议 |
|---------------|---------|---------|
| `analog` | voltage, value | ESP32 可用 |
| `digital` | value | ESP32 可用 |
| `dht11` / `dht22` | temperature, humidity | ESP32 可用 |
| `ds18b20` | temperature | ESP32 可用 |
| `ultrasonic` | distance | ESP32 可用 |
| `current` | current | ESP32 可用 |
| `voltage` | voltage | ESP32 可用 |
| `SHT31` | temperature, humidity | ESP32 可用,轻量 I2C |
| `AHT20` | temperature, humidity | ESP32 可用,轻量 I2C |
| `BH1750` | illuminance | ESP32 可用,轻量 I2C |
| `BMP280` | temperature, pressure, altitude | 建议 esp32s3-full |
| `MPU6050` | accelX/Y/Z, temperature, gyroX/Y/Z | 建议 esp32s3-full |

### 异步执行引擎

**Worker池架构**:
- 预创建 3 个异步任务 (避免运行时分配)
- 最小堆内存门控: 30KB
- 任务栈大小: 4KB (普通) / 8KB (脚本)
- 任务优先级: 0 (最低)

**执行流程**:
```
规则触发 → 检查堆内存 (>30KB?)
       → 是: 派发异步任务到 Worker 池
       → 否: 记录 WARN 日志,跳过执行
       → Worker 执行动作序列
       → 上报执行结果
```

**关键方法**:
- `evaluateCondition()`: 评估触发器条件
- `dispatchAction()`: 分发动作到执行器
- `executeRuleAsync()`: 异步执行规则
- `onSensorData()`: 处理传感器数据事件

## PeripheralManager 外设管理

PeripheralManager 负责所有外设的生命周期管理。

### 外设配置模型

```json
{
  "id": "sensor_01",
  "name": "传感器名称",
  "type": 38,
  "enabled": true,
  "pinCount": 1,
  "pins": [13, 255, 255, 255, 255, 255, 255, 255],
  "params": {}
}
```

**字段说明**:
- `id`: 唯一标识,用于规则引用
- `name`: 显示名称
- `type`: 外设类型枚举值
- `enabled`: 是否启用
- `pinCount`: 使用引脚数量
- `pins`: 引脚数组 (255 表示未使用)
- `params`: 类型特定参数

### 常用外设类型

| type | 名称 | 说明 | 引脚数 |
|------|------|------|-------|
| 1 | UART | 串口 / Modbus RTU | 2 (TX/RX) |
| 2 | I2C | I2C 总线 | 2 (SDA/SCL) |
| 11 | GPIO_DIGITAL_INPUT | 数字输入 | 1 |
| 12 | GPIO_DIGITAL_OUTPUT | 数字输出 | 1 |
| 13 | GPIO_DIGITAL_INPUT_PULLUP | 上拉输入(按键) | 1 |
| 14 | GPIO_DIGITAL_INPUT_PULLDOWN | 下拉输入 | 1 |
| 15 | GPIO_ANALOG_INPUT | ADC 模拟输入 | 1 |
| 17 | GPIO_PWM_OUTPUT | PWM 输出 | 1 |
| 18-20 | GPIO_INTERRUPT | 中断输入 (上升/下降/双沿) | 1 |
| 21 | GPIO_TOUCH | 触摸输入 | 1 |
| 26 | ADC_INPUT | ADC 输入(别名) | 1 |
| 36 | LCD | 显示屏(SSD1306/SH1106) | 2 (I2C) |
| 37 | SDIO | SD 卡接口(占位) | 4-6 |
| 38 | SENSOR | 传感器(DHT/DS18B20/超声波等) | 1-2 |
| 41 | PWM_SERVO | 舵机 | 1 |
| 42 | STEPPER_MOTOR | 步进电机 | 4-6 |
| 43 | ENCODER | 旋转编码器 | 2-3 |
| 44 | ONE_WIRE | 单总线(DS18B20) | 1 |
| 45 | NEO_PIXEL | WS2812B RGB 灯带 | 1 |
| 47 | SEVEN_SEGMENT_TM1637 | TM1637 数码管 | 2 (CLK/DIO) |
| 51 | MODBUS_DEVICE | Modbus 从站设备 | 0 (UART) |
| 60 | DEVICE_EVENT | 逻辑事件源 | 0 |

### 硬件初始化流程

```
1. 验证引脚合法性
   - 避开 GPIO6-11 (Flash 引脚)
   - GPIO34-39 仅输入
   - WiFi 开启后 ADC2 不可用

2. 检查引脚冲突
   - 同一引脚不能重复分配
   - I2C 设备可共用 SDA/SCL

3. 配置引脚模式
   - pinMode(pin, INPUT/OUTPUT/INPUT_PULLUP)
   - ledcSetup() for PWM
   - adcAttachPin() for ADC

4. 初始化外设驱动
   - 根据 type 创建对应 Driver
   - 调用 driver->begin(pins, params)

5. 注册到外设列表
   - 添加到 m_peripherals map
   - 记录引脚占用表
```

**关键方法**:
- `addPeripheral()`: 添加外设
- `initHardware()`: 初始化硬件
- `setupHardware()`: 配置引脚
- `removePeripheral()`: 删除外设
- `getPeripheral()`: 获取外设实例
- `performMaintenance()`: 定期维护（处理 ISR 中断队列）
- `processInterruptQueue()`: 从 FreeRTOS 队列取出中断事件并分发

**中断处理机制**:

GPIO 中断通过 FreeRTOS 队列从 ISR 上下文安全地传递到主循环：

```
ISR中断 → xQueueSendFromISR(pin) → 主循环 performMaintenance() → processInterruptQueue() → handleInterrupt(pin)
```

### 传感器数据缓存

传感器读取动作会把最近一次读数写入本地缓存:

```cpp
// 缓存键格式
"sensor_cache:<periphId>:<dataField>"

// 示例
"sensor_cache:dht_01:temperature" → 28.5
"sensor_cache:dht_01:humidity" → 65.2
```

缓存数据用于:
- 事件触发器条件评估
- Web 界面实时显示
- MQTT 数据上报

## ConfigStorage 配置存储

ConfigStorage 管理所有配置的持久化。

### 配置文件结构

```
/config/
  ├── device.json       # 设备配置(名称/型号/序列号)
  ├── network.json      # 网络配置(WiFi/以太网/4G/LoRa)
  ├── peripherals.json  # 外设配置(所有外设列表)
  ├── periph_exec.json  # 执行规则(触发器+动作)
  ├── protocol.json     # 协议配置(MQTT/Modbus)
  ├── roles.json        # 角色配置(full版)
  └── users.json        # 用户配置(full版)
```

### 存储策略

**LittleFS 文件系统**:
- 嵌入式 Flash 文件系统
- 支持 wear leveling (磨损均衡)
- 掉电安全 (原子写入)

**JSON 序列化**:
- 使用 ArduinoJson 库
- 紧凑模式 (减少空间占用)
- 流式读写 (避免全量加载)

**写入保护**:
- 先写临时文件 `.tmp`
- 验证 JSON 格式
- 原子替换原文件
- 失败时保留原配置

### 配置导入导出

**导出 API**:
```
GET /api/system/export-config?type=peripherals
GET /api/system/export-config?type=periph_exec
GET /api/system/export-config?type=all
```

**导入 API**:
```
POST /api/system/import-config
Body: multipart/form-data (JSON文件)
```

**关键方法**:
- `loadConfig()`: 加载配置
- `saveConfig()`: 保存配置
- `exportConfig()`: 导出配置
- `importConfig()`: 导入配置

## HealthMonitor 健康监控

HealthMonitor 持续监控系统健康状态。

### 监控指标

| 指标 | 检查间隔 | 说明 |
|------|---------|------|
| 空闲堆内存 | 5秒 | ESP.getFreeHeap() |
| 最大可分配块 | 5秒 | ESP.getMaxAllocHeap() |
| WiFi连接状态 | 1秒 | WiFi.status() |
| MQTT连接状态 | 1秒 | MQTTClient.connected() |
| 系统运行时间 | 持续 | millis() |

### 告警机制

**内存阈值**:

| 堆内存 | 动作 | 原因 |
|-------|------|------|
| < 20KB | WARN 日志 | 提醒内存不足 |
| < 10KB | 关闭 SSE | SSE 连接占用 ~8KB |
| < 5KB | 降低日志级别 | 减少字符串分配 |

**恢复策略**:
- 不自动恢复 (避免频繁抖动)
- 需外部干预 (重启/关闭功能)
- 记录告警日志供排查

### WDT 看门狗

**配置**:
```cpp
esp_task_wdt_init(10, true);  // 10秒超时
```

**原因**:
- async_tcp 文件 I/O 可能阻塞
- 大型 JSON 序列化耗时
- 防止系统死锁

**喂养**:
- loop() 主循环自动喂养
- 异步任务需手动喂养

### 启动诊断

**输出信息**:
```
[BOOT] Chip: ESP32-WROOM-32U Rev3
[BOOT] Flash: 4096KB, PSRAM: 0KB
[BOOT] Cores: 2, Features: WiFi/BT/BLE
[BOOT] Free heap: 285432 bytes
[BOOT] Max alloc: 114688 bytes
[BOOT] PSRAM: disabled (no-PSRAM build)
```

**用途**:
- 快速判断 PSRAM 是否正常
- 确认可用内存容量
- 排查启动失败问题

## 关键类关系

```
FastBeeFramework
  ├── PeripheralManager* m_peripheralManager
  ├── PeriphExecManager* m_periphExecManager
  ├── NetworkManager* m_networkManager
  ├── ProtocolManager* m_protocolManager
  ├── ConfigStorage* m_configStorage
  └── HealthMonitor* m_healthMonitor

PeripheralManager
  ├── map<string, Peripheral*> m_peripherals
  ├── map<int, bool> m_pinOccupancy
  └── vector<Driver*> m_drivers

PeriphExecManager
  ├── map<string, ExecRule*> m_rules
  ├── WorkerPool* m_workerPool
  ├── map<string, string> m_sensorCache
  └── ButtonEventSubsystem* m_buttonEvents

NetworkManager
  ├── WiFiManager* m_wifiManager
  ├── WebConfigManager* m_webManager
  └── NetworkAdapter* m_activeAdapter

ProtocolManager
  ├── MQTTClient* m_mqttClient
  ├── ModbusHandler* m_modbusHandler
  └── map<string, ProtocolHandler*> m_protocols
```

## 相关文档

- [架构设计](architecture.md) - 整体架构和模块关系
- [外设执行流程](periph_exec_flow.md) - 规则引擎完整业务逻辑
- [外设配置指南](peripheral-configuration-guide.md) - 所有外设类型详解
- [开发指南](development-guide.md) - 添加新外设和协议

---

**文档版本**: v1.0  
**最后更新**: 2026-06-03  
**维护者**: FastBee团队
