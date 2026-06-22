# FastBee-Arduino 外设/协议/功能 资源占用与配置指南

> 本文档全面介绍各功能模块的 RAM / Flash 资源占用情况，以及如何通过 `platformio.ini` 启用或禁用它们。

---

## 1. ESP32 内存背景

在配置功能开关之前，需要理解 ESP32 的内存约束：

| 资源 | ESP32 | ESP32-S3 | ESP32-C3 | ESP32-C6 |
|------|-------|----------|----------|----------|
| 内部 SRAM | ~320KB | ~512KB | ~400KB | ~512KB |
| 可用堆（启动后） | ~220KB | ~310KB | ~140KB | ~220KB |
| PSRAM（可选） | 4-8MB | 2-8MB | 无 | 无 |
| Flash | 4-16MB | 8-16MB | 4MB | 4MB |

**关键约束**：
- WiFi 驱动、lwIP TCP/IP 栈、FreeRTOS 内核、TLS/SSL 握手**必须驻留内部 DRAM**，无法卸载到 PSRAM
- 蓝牙控制器启动时额外占用 ~30KB DRAM（默认已释放）
- PSRAM 延迟比 DRAM 高 ~3 倍，适合 HTTP/JSON 缓冲区，不适合实时性高的场景
- 碎片化是真正的内存杀手：总空闲 35KB 但最大连续块可能仅 8KB

**基线内存占用**（WiFi + WebServer + 健康监控已启用）：

| 组件 | 估算 DRAM 占用 |
|------|---------------|
| WiFi 驱动 + lwIP | ~60-80KB |
| FreeRTOS 内核 + 任务栈 | ~30-40KB |
| AsyncWebServer + AsyncTCP | ~20-30KB |
| PubSubClient (MQTT) | ~4-8KB |
| ArduinoJson 运行时 | ~4-8KB |
| 系统服务（日志/配置/健康监控） | ~10-15KB |
| **基线合计** | **~130-180KB** |

这意味着在 4MB no-PSRAM 的 ESP32 上，基线功能已吃掉大半内存，**每 KB 的节省都很重要**。

---

## 2. 功能开关配置方法

### 2.1 配置位置

功能开关通过 `platformio.ini` 的 `build_flags` 配置，格式为：

```ini
build_flags =
    -DFASTBEE_ENABLE_XXX=1    ; 启用
    -DFASTBEE_ENABLE_XXX=0    ; 禁用
```

默认值定义在 `include/core/FeatureFlags.h` 中，`platformio.ini` 中的值会覆盖默认值。

### 2.2 三级预设

项目预定义了三套功能级别，通过 `platformio.ini` 的 `[lite_flags]` / `[standard_flags]` / `[full_flags]` section 引用：

```ini
; Lite 精简版 — 4MB Flash 芯片 (C3/C6)
build_flags = ${lite_flags.build_flags}

; Standard 标准版 — 4-8MB Flash 芯片
build_flags = ${standard_flags.build_flags}

; Full 全功能版 — 8MB+ Flash + PSRAM
build_flags = ${full_flags.build_flags}
```

---

## 3. 外设/显示模块（RAM 占用重点）

以下模块是 **RAM 占用大户**，在内存受限芯片上应优先考虑裁剪。

### 3.1 OLED/LCD 显示屏 — `FASTBEE_ENABLE_LCD`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_LCD` |
| **RAM 占用** | **~1.2KB**（1KB 帧缓冲区 + ~200B 管理器开销） |
| Flash 占用 | ~20KB（U8g2 库 ~15KB + 管理器 ~5KB） |
| 硬件需求 | SSD1306/SH1106 等 I2C/SPI OLED/LCD |
| 依赖库 | U8g2 |
| 默认值 | Lite: 1, Standard: 1, Full: 1 |

```ini
; 禁用 LCD 显示屏（节省 ~1.2KB RAM + ~20KB Flash）
-DFASTBEE_ENABLE_LCD=0
```

> **建议**：如果硬件上没有接 OLED/LCD 屏幕，务必禁用以释放 ~1.2KB 宝贵的 DRAM。

### 3.2 NeoPixel LED 灯带 — `FASTBEE_ENABLE_NEOPIXEL`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_NEOPIXEL` |
| **RAM 占用** | **3B × 灯珠数 + ~2KB 库开销**（64 颗 ≈ 212B + 2KB） |
| Flash 占用 | ~12KB |
| 硬件需求 | WS2812B LED 灯带/灯珠 |
| 依赖库 | Adafruit NeoPixel |
| 默认值 | Lite: 1, Standard: 1, Full: 1 |

```ini
; 禁用 NeoPixel（节省 ~2KB RAM + ~12KB Flash）
-DFASTBEE_ENABLE_NEOPIXEL=0

; 或减少最大灯珠数（节省 RAM）
-DFASTBEE_NEOPIXEL_MAX_LEDS=32    ; 从默认 64 减半
-DFASTBEE_NEOPIXEL_MAX_LEDS=16    ; 减到 16 颗
```

> **建议**：不使用 LED 灯带时禁用。如果只需少量灯珠，通过 `FASTBEE_NEOPIXEL_MAX_LEDS` 减少上限即可。

### 3.3 WS2812B LED 点阵屏 — `FASTBEE_ENABLE_LED_SCREEN`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_LED_SCREEN` |
| **RAM 占用** | **动态**（24 × 灯珠数 × sizeof(rmt_item32_t)，发送时临时分配） |
| Flash 占用 | ~10KB |
| 硬件需求 | WS2812B 点阵屏矩阵 |
| 默认值 | Lite: 0, Standard: 1, Full: 1 |

```ini
; 禁用 LED 点阵屏（节省动态 RAM + ~10KB Flash）
-DFASTBEE_ENABLE_LED_SCREEN=0
```

> **注意**：与 `FASTBEE_ENABLE_NEOPIXEL` 是独立模块。LED_SCREEN 是点阵矩阵显示，NEOPIXEL 是灯带控制。

### 3.4 I2C 高级传感器 — `FASTBEE_ENABLE_I2C_SENSORS`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_I2C_SENSORS` |
| **RAM 占用** | **~500B**（驱动实例 + 数据缓存） |
| Flash 占用 | ~15KB（BMP280 ~5KB + MPU6050 ~10KB） |
| 支持传感器 | BMP280（气压/温度）、MPU6050（6轴）、SHT31（温湿度）、BH1750（光照）、AHT20（温湿度） |
| 依赖库 | Adafruit BMP280, Adafruit MPU6050 |
| 默认值 | Lite: 0, Standard: 1, Full: 1 |

```ini
; 禁用 I2C 高级传感器（节省 ~500B RAM + ~15KB Flash）
-DFASTBEE_ENABLE_I2C_SENSORS=0
```

> **建议**：不需要 BMP280/MPU6050 等高级传感器时禁用。基础传感器 DHT11/DHT22/DS18B20 由 `FASTBEE_ENABLE_SENSOR_DRIVER` 独立控制。

### 3.5 RFID 射频卡模块 — `FASTBEE_ENABLE_RFID`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_RFID` |
| **RAM 占用** | **~200B** |
| Flash 占用 | ~12KB |
| 硬件需求 | MFRC522 模块（SPI 接口） |
| 依赖库 | MFRC522 |
| 默认值 | Lite: 0, Standard: 1, Full: 1 |

```ini
; 禁用 RFID（节省 ~200B RAM + ~12KB Flash）
-DFASTBEE_ENABLE_RFID=0
```

### 3.6 红外遥控 — `FASTBEE_ENABLE_IR_REMOTE`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_IR_REMOTE` |
| **RAM 占用** | **~300B**（接收缓冲区） |
| Flash 占用 | ~8KB |
| 硬件需求 | 红外接收管 + 发射管 |
| 依赖库 | IRremoteESP8266 |
| 默认值 | Lite: 0, Standard: 1, Full: 1 |

```ini
; 禁用红外遥控（节省 ~300B RAM + ~8KB Flash）
-DFASTBEE_ENABLE_IR_REMOTE=0
```

### 3.7 基础传感器驱动 — `FASTBEE_ENABLE_SENSOR_DRIVER`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_SENSOR_DRIVER` |
| **RAM 占用** | **~200B**（驱动实例 + 缓存） |
| Flash 占用 | ~8KB（DHT ~3KB + OneWire ~2KB + DallasTemperature ~3KB） |
| 支持传感器 | DHT11、DHT22、DS18B20 |
| 依赖库 | Adafruit DHT, OneWire, DallasTemperature |
| 默认值 | 所有版本: 1 |

```ini
; 禁用基础传感器（节省 ~200B RAM + ~8KB Flash）
-DFASTBEE_ENABLE_SENSOR_DRIVER=0
```

### 3.8 TM1637 数码管 — `FASTBEE_ENABLE_SEVEN_SEGMENT`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_SEVEN_SEGMENT` |
| **RAM 占用** | **~32B × 实例数** |
| Flash 占用 | ~3KB（自写 bit-bang 驱动，无外部库） |
| 默认值 | Lite: 1, Standard: 1, Full: 1 |

```ini
; 禁用数码管（节省 ~32B RAM + ~3KB Flash）
-DFASTBEE_ENABLE_SEVEN_SEGMENT=0
```

### 3.9 DS1302 实时时钟 — `FASTBEE_ENABLE_DS1302`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_DS1302` |
| **RAM 占用** | **~100B** |
| Flash 占用 | ~2KB |
| 硬件需求 | DS1302 模块（3线接口：CE/IO/SCLK） |
| 默认值 | 所有版本: 0 |

```ini
; 启用 DS1302 实时时钟（增加 ~100B RAM + ~2KB Flash）
-DFASTBEE_ENABLE_DS1302=1
```

### 3.10 LCD1602 字符液晶 — `FASTBEE_ENABLE_LCD1602`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_LCD1602` |
| **RAM 占用** | **~100B** |
| Flash 占用 | ~3KB |
| 硬件需求 | LCD1602/LCD2004 + PCF8574 I2C 扩展板 |
| 默认值 | 所有版本: 0 |

```ini
; 启用 LCD1602（增加 ~100B RAM + ~3KB Flash）
-DFASTBEE_ENABLE_LCD1602=1
```

---

## 4. 网络模块

### 4.1 以太网 (W5500) — `FASTBEE_ENABLE_ETHERNET`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_ETHERNET` |
| **RAM 占用** | **~8KB**（W5500 驱动缓冲区 + SPI 事务缓冲） |
| Flash 占用 | ~8KB |
| 硬件需求 | W5500 模块（SPI2_HOST 接口） |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用以太网（节省 ~8KB RAM + ~8KB Flash）
-DFASTBEE_ENABLE_ETHERNET=0
```

### 4.2 4G 蜂窝网络 (EC801E) — `FASTBEE_ENABLE_CELLULAR`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_CELLULAR` |
| **RAM 占用** | **~10-15KB**（TinyGSM 库 + UART 缓冲 + AT 指令缓冲 1KB） |
| Flash 占用 | ~15KB |
| 硬件需求 | EC801E-CN 模块（UART2，默认引脚 pwrPin=38, txPin=39, rxPin=40） |
| 依赖库 | TinyGSM |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用 4G 蜂窝网络（节省 ~10-15KB RAM + ~15KB Flash）
-DFASTBEE_ENABLE_CELLULAR=0
```

> **注意**：4G 模块是 RAM 占用最大的网络组件之一，不使用务必禁用。

### 4.3 BLE 蓝牙 — `FASTBEE_ENABLE_BLE`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_BLE` |
| **RAM 占用** | **~20-30KB**（NimBLE 协议栈运行时） |
| Flash 占用 | **~80KB**（NimBLE 库，Flash 大户！） |
| 依赖库 | NimBLE-Arduino |
| 默认值 | Lite: 0, Standard: 0, Full: 0（已从所有预设移除） |

```ini
; 禁用 BLE（节省 ~20-30KB RAM + ~80KB Flash）
-DFASTBEE_ENABLE_BLE=0
```

如果旧配置或私有分支仍引入 NimBLE 库，需要同步清理依赖或在 `lib_ignore` 中排除：
```ini
lib_ignore = NimBLE-Arduino
```

> **重要**：BLE 是所有功能开关中 Flash 占用最大的（~80KB），RAM 占用也很高。当前所有预设均禁用 BLE，宏仅保留用于兼容旧配置；如需重新启用，需要同时恢复 NimBLE 依赖并重新评估 RAM 余量。

### 4.4 mDNS — `FASTBEE_ENABLE_MDNS`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_MDNS` |
| **RAM 占用** | **~2KB**（mDNS 查询缓存） |
| Flash 占用 | ~5KB |
| 默认值 | 所有版本: 1 |

```ini
; 禁用 mDNS（节省 ~2KB RAM + ~5KB Flash）
-DFASTBEE_ENABLE_MDNS=0
```

> **注意**：禁用后将无法通过 `fastbee.local` 域名访问设备，只能通过 IP 地址。

### 4.5 DNS 服务器 — `FASTBEE_ENABLE_DNS`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_DNS` |
| **RAM 占用** | **~1KB** |
| Flash 占用 | ~3KB |
| 默认值 | 所有版本: 1 |

```ini
; 禁用 DNS 服务器（节省 ~1KB RAM + ~3KB Flash）
-DFASTBEE_ENABLE_DNS=0
```

### 4.6 AP 模式 — `FASTBEE_ENABLE_AP_MODE`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_AP_MODE` |
| **RAM 占用** | **~1-2KB**（AP 连接客户端信息） |
| Flash 占用 | ~2KB |
| 默认值 | 所有版本: 1 |

```ini
; 禁用 AP 模式（节省 ~1-2KB RAM + ~2KB Flash）
-DFASTBEE_ENABLE_AP_MODE=0
```

> **警告**：禁用后设备将无法在首次启动时提供配置热点，需要预先配置好 WiFi 或通过其他方式配置。

---

## 5. 协议模块

### 5.1 MQTT — `FASTBEE_ENABLE_MQTT`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_MQTT` |
| **RAM 占用** | **~4-8KB**（PubSubClient 缓冲 1KB + 8 个上报槽 × 512B = 4KB + TLS ~3KB） |
| Flash 占用 | ~15KB |
| 默认值 | 所有版本: 1 |

```ini
; 禁用 MQTT（节省 ~4-8KB RAM + ~15KB Flash）
-DFASTBEE_ENABLE_MQTT=0
```

> **注意**：MQTT 是 FastBee 的核心协议，禁用后设备将无法与物联网平台通信。仅在纯本地使用场景下禁用。

**MQTT TLS/SSL 额外开销**：使用 MQTTS（加密连接）时，TLS 握手需要额外 ~6-10KB DRAM（证书验证缓冲区）。`MQTTClient` 提供 `setInsecure()` 方法跳过证书验证以降低内存需求。

**上报槽配置**：默认 8 个固定槽位（`MQTT_REPORT_SLOTS = 8`），每槽 512B，共 4KB，零堆分配设计。

### 5.2 Modbus RTU — `FASTBEE_ENABLE_MODBUS`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_MODBUS` |
| **RAM 占用** | **~3-5KB**（轮询任务缓存 + 寄存器映射 + 串口缓冲） |
| Flash 占用 | ~20KB |
| 默认值 | Lite: 0, Standard: 1, Full: 1 |

```ini
; 禁用 Modbus（节省 ~3-5KB RAM + ~20KB Flash）
-DFASTBEE_ENABLE_MODBUS=0
```

### 5.3 Modbus 从站模式 — `FASTBEE_MODBUS_SLAVE_ENABLE`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_MODBUS_SLAVE_ENABLE` |
| **RAM 占用** | **~440B**（从站寄存器表 + 响应缓冲） |
| Flash 占用 | ~10KB |
| 前置条件 | `FASTBEE_ENABLE_MODBUS=1` |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用 Modbus 从站模式（节省 ~440B RAM + ~10KB Flash）
-DFASTBEE_MODBUS_SLAVE_ENABLE=0
```

### 5.4 TCP 服务器 — `FASTBEE_ENABLE_TCP`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_TCP` |
| **RAM 占用** | **~2-3KB**（多客户端连接缓冲） |
| Flash 占用 | ~10KB |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用 TCP 服务器（节省 ~2-3KB RAM + ~10KB Flash）
-DFASTBEE_ENABLE_TCP=0
```

### 5.5 HTTP 客户端 — `FASTBEE_ENABLE_HTTP`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_HTTP` |
| **RAM 占用** | **~2KB**（HTTP 请求/响应缓冲 2KB） |
| Flash 占用 | ~8KB |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用 HTTP 客户端（节省 ~2KB RAM + ~8KB Flash）
-DFASTBEE_ENABLE_HTTP=0
```

### 5.6 CoAP — `FASTBEE_ENABLE_COAP`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_COAP` |
| **RAM 占用** | **~1KB** |
| Flash 占用 | ~12KB |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用 CoAP（节省 ~1KB RAM + ~12KB Flash）
-DFASTBEE_ENABLE_COAP=0
```

---

## 6. 系统服务模块

### 6.1 健康监控 — `FASTBEE_ENABLE_HEALTH_MONITOR`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_HEALTH_MONITOR` |
| **RAM 占用** | **~200B**（SystemHealth 结构体 + 状态变量） |
| Flash 占用 | ~4KB |
| 默认值 | 所有版本: 1 |

```ini
; 禁用健康监控（节省 ~200B RAM + ~4KB Flash）
-DFASTBEE_ENABLE_HEALTH_MONITOR=0
```

> **强烈不建议禁用**：健康监控是设备稳定运行的保障，提供四级内存保护和碎片紧凑化。

### 6.2 任务调度器 — `FASTBEE_ENABLE_TASK_MANAGER`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_TASK_MANAGER` |
| **RAM 占用** | **~500B**（任务列表 + 队列） |
| Flash 占用 | ~5KB |
| 默认值 | 所有版本: 1 |

### 6.3 日志系统 — `FASTBEE_ENABLE_LOGGER`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_LOGGER` |
| **RAM 占用** | **~300B**（日志缓冲区，由 `FASTBEE_LOG_BUFFER_SIZE` 控制） |
| Flash 占用 | ~6KB |
| 默认值 | 所有版本: 1 |

```ini
; 减小日志缓冲区（节省 RAM）
-DFASTBEE_LOG_BUFFER_SIZE=128     ; 默认 256B，减到 128B

; 剥离 INFO 级别日志字符串（节省 ~2-5KB Flash）
-DFASTBEE_STRIP_INFO_LOGS=1
```

### 6.4 文件日志 — `FASTBEE_ENABLE_FILE_LOGGING`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_FILE_LOGGING` |
| **RAM 占用** | **~500B**（文件写缓冲） |
| Flash 占用 | ~3KB |
| 副作用 | 持续写 Flash 会缩短闪存寿命 |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 禁用文件日志（节省 ~500B RAM + ~3KB Flash，延长 Flash 寿命）
-DFASTBEE_ENABLE_FILE_LOGGING=0
```

### 6.5 外设执行规则 — `FASTBEE_ENABLE_PERIPH_EXEC`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_PERIPH_EXEC` |
| **RAM 占用** | **~2-4KB**（规则列表 + 调度器 + 工作线程池） |
| Flash 占用 | ~8KB |
| 默认值 | 所有版本: 1 |

```ini
; 禁用外设执行规则（节省 ~2-4KB RAM + ~8KB Flash）
-DFASTBEE_ENABLE_PERIPH_EXEC=0
```

> **注意**：禁用后定时控制、按键联动、传感器联控、MQTT 触发等自动化功能全部不可用。

### 6.6 规则脚本引擎 — `FASTBEE_ENABLE_RULE_SCRIPT`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_RULE_SCRIPT` |
| **RAM 占用** | **~4-8KB**（脚本解释器栈 + 模板引擎） |
| Flash 占用 | ~8KB |
| 默认值 | Lite: 0, Standard: 1, Full: 1 |

```ini
; 禁用规则脚本引擎（节省 ~4-8KB RAM + ~8KB Flash）
-DFASTBEE_ENABLE_RULE_SCRIPT=0
```

### 6.7 命令脚本 — `FASTBEE_ENABLE_COMMAND_SCRIPT`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_COMMAND_SCRIPT` |
| **RAM 占用** | **~1-2KB** |
| Flash 占用 | ~3KB |
| 默认值 | 继承 `FASTBEE_ENABLE_RULE_SCRIPT` |

```ini
; 禁用命令脚本（节省 ~1-2KB RAM + ~3KB Flash）
-DFASTBEE_ENABLE_COMMAND_SCRIPT=0
```

### 6.8 OTA 升级 — `FASTBEE_ENABLE_OTA`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_OTA` / `FASTBEE_ENABLE_OTA_FS` |
| **RAM 占用** | **~4-6KB**（OTA 传输缓冲 4KB + 状态管理） |
| Flash 占用 | ~10KB |
| 硬件要求 | Flash ≥ 8MB（需要双 App 分区） |
| 默认值 | Lite: 0, Standard: ⚠️, Full: 1 |

```ini
; 禁用 OTA（节省 ~4-6KB RAM + ~10KB Flash）
-DFASTBEE_ENABLE_OTA=0
-DFASTBEE_ENABLE_OTA_FS=0
```

### 6.9 配置存储缓存 — `FASTBEE_ENABLE_STORAGE_CACHE`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_STORAGE_CACHE` |
| **RAM 占用** | **~2-4KB**（LRU 缓存 + 原始 JSON 文本缓存） |
| Flash 占用 | ~1KB |
| 默认值 | Lite: 0, Standard: 0, Full: 0 |

```ini
; 启用配置缓存（增加 ~2-4KB RAM，减少文件 I/O）
-DFASTBEE_ENABLE_STORAGE_CACHE=1
```

> **说明**：超阈值大文件（> 4KB，如 protocol.json、peripherals.json）自动绕过缓存，避免堆峰值翻倍。

---

## 7. Web 服务模块

### 7.1 Web 服务器 — `FASTBEE_ENABLE_WEB_SERVER`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_WEB_SERVER` |
| **RAM 占用** | **~20-30KB**（AsyncWebServer + AsyncTCP 连接池） |
| Flash 占用 | ~25KB（不含静态资源） |
| 默认值 | 所有版本: 1 |

```ini
; 禁用 Web 服务器（节省 ~20-30KB RAM + ~25KB Flash）
-DFASTBEE_ENABLE_WEB_SERVER=0
```

> **警告**：禁用后无法通过浏览器管理设备，仅能通过 MQTT/串口配置。

### 7.2 用户管理 — `FASTBEE_ENABLE_USER_ADMIN`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_USER_ADMIN` |
| **RAM 占用** | **~500B**（多用户列表） |
| Flash 占用 | ~5KB |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

### 7.3 文件管理器 — `FASTBEE_ENABLE_FILE_MANAGER`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_FILE_MANAGER` |
| **RAM 占用** | **~500B** |
| Flash 占用 | ~3KB |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

### 7.4 国际化 — `FASTBEE_ENABLE_I18N`

| 项目 | 数值 |
|------|------|
| 开关宏 | `FASTBEE_ENABLE_I18N` |
| **RAM 占用** | **~0**（语言包在 LittleFS 中，按需加载） |
| Flash 占用（LittleFS） | ~10KB（英文语言包） |
| 默认值 | Lite: 0, Standard: 0, Full: 1 |

```ini
; 启用国际化（增加 ~10KB LittleFS 空间）
-DFASTBEE_ENABLE_I18N=1
```

---

## 8. 性能调优参数

以下参数不控制功能启用/禁用，但影响内存使用行为：

### 8.1 PSRAM 使用 — `FASTBEE_USE_PSRAM`

```ini
; 启用 PSRAM（≥512B 分配优先使用 PSRAM）
-DFASTBEE_USE_PSRAM=1
-DBOARD_HAS_PSRAM
```

> 仅对有 PSRAM 的硬件生效，无 PSRAM 硬件设置此值无效。

### 8.2 JSON 文档大小

```ini
; 默认 JSON 文档大小（栈上分配，影响栈使用）
-DFASTBEE_JSON_DOC_SIZE=4096       ; 默认 4096B
-DFASTBEE_JSON_DOC_SIZE_LARGE=8192 ; 大型配置 8192B

; 减小可节省栈空间
-DFASTBEE_JSON_DOC_SIZE=2048
```

### 8.3 AsyncTCP 连接参数

```ini
; TCP 最大并发连接数（每个连接 ~12KB DRAM）
-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=6    ; ESP32 默认
-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=4    ; C3 节省内存
-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=14   ; S3 充裕

; TCP 队列深度
-DCONFIG_ASYNC_TCP_QUEUE_SIZE=8         ; ESP32 默认
-DCONFIG_ASYNC_TCP_QUEUE_SIZE=4         ; C3 节省
```

### 8.4 任务栈大小

```ini
; 主循环栈大小（Modbus 注册子设备时调用链深，需 ≥16KB）
-DARDUINO_LOOP_STACK_SIZE=16384     ; ESP32/S3
-DARDUINO_LOOP_STACK_SIZE=12288     ; C3/C6

; 脚本任务栈
-DSCRIPT_TASK_STACK=8192            ; 全功能
-DSCRIPT_TASK_STACK=6144            ; 精简
```

### 8.5 日志精简

```ini
; 剥离 INFO 级别日志字符串（节省 ~2-5KB Flash）
-DFASTBEE_STRIP_INFO_LOGS=1

; 启用调试日志（增加 Flash，仅调试时使用）
-DFASTBEE_DEBUG_LOG=0

; 详细错误信息（增加 Flash）
-DFASTBEE_VERBOSE_ERROR=0

; ESP-IDF 内核日志级别（0=none, 1=error, 2=warn, 3=info, 4=debug）
-DCORE_DEBUG_LEVEL=1
```

---

## 9. 资源占用汇总表

### 9.1 外设/显示类

| 开关宏 | RAM | Flash | Lite | Std | Full | 说明 |
|--------|-----|-------|:----:|:---:|:----:|------|
| `FASTBEE_ENABLE_LCD` | **~1.2KB** | ~20KB | 1 | 1 | 1 | OLED/LCD 显示屏 (U8g2) |
| `FASTBEE_ENABLE_NEOPIXEL` | **~2KB + 3B/颗** | ~12KB | 1 | 1 | 1 | WS2812 LED 灯带 |
| `FASTBEE_ENABLE_LED_SCREEN` | **动态** | ~10KB | 0 | 1 | 1 | WS2812B 点阵屏 |
| `FASTBEE_ENABLE_I2C_SENSORS` | **~500B** | ~15KB | 0 | 1 | 1 | BMP280/MPU6050 等 |
| `FASTBEE_ENABLE_RFID` | **~200B** | ~12KB | 0 | 1 | 1 | MFRC522 RFID |
| `FASTBEE_ENABLE_IR_REMOTE` | **~300B** | ~8KB | 0 | 1 | 1 | 红外遥控 |
| `FASTBEE_ENABLE_SENSOR_DRIVER` | **~200B** | ~8KB | 1 | 1 | 1 | DHT/DS18B20 |
| `FASTBEE_ENABLE_SEVEN_SEGMENT` | **~32B** | ~3KB | 1 | 1 | 1 | TM1637 数码管 |
| `FASTBEE_ENABLE_DS1302` | **~100B** | ~2KB | 0 | 0 | 0 | DS1302 实时时钟 |
| `FASTBEE_ENABLE_LCD1602` | **~100B** | ~3KB | 0 | 0 | 0 | LCD1602 I2C 液晶 |

### 9.2 网络类

| 开关宏 | RAM | Flash | Lite | Std | Full | 说明 |
|--------|-----|-------|:----:|:---:|:----:|------|
| `FASTBEE_ENABLE_BLE` | **~20-30KB** | **~80KB** | 0 | 0 | 0 | BLE 蓝牙（已从预设移除） |
| `FASTBEE_ENABLE_CELLULAR` | **~10-15KB** | ~15KB | 0 | 0 | 1 | 4G EC801E |
| `FASTBEE_ENABLE_ETHERNET` | **~8KB** | ~8KB | 0 | 0 | 1 | 以太网 W5500 |
| `FASTBEE_ENABLE_MDNS` | **~2KB** | ~5KB | 1 | 1 | 1 | mDNS 本地域名 |
| `FASTBEE_ENABLE_DNS` | **~1KB** | ~3KB | 1 | 1 | 1 | DNS 服务器 |
| `FASTBEE_ENABLE_AP_MODE` | **~1-2KB** | ~2KB | 1 | 1 | 1 | AP 热点模式 |

### 9.3 协议类

| 开关宏 | RAM | Flash | Lite | Std | Full | 说明 |
|--------|-----|-------|:----:|:---:|:----:|------|
| `FASTBEE_ENABLE_MQTT` | **~4-8KB** | ~15KB | 1 | 1 | 1 | MQTT 协议（核心） |
| `FASTBEE_ENABLE_MODBUS` | **~3-5KB** | ~20KB | 0 | 1 | 1 | Modbus RTU 主站 |
| `FASTBEE_MODBUS_SLAVE_ENABLE` | **~440B** | ~10KB | 0 | 0 | 1 | Modbus 从站 |
| `FASTBEE_ENABLE_TCP` | **~2-3KB** | ~10KB | 0 | 0 | 1 | TCP 服务器 |
| `FASTBEE_ENABLE_HTTP` | **~2KB** | ~8KB | 0 | 0 | 1 | HTTP 客户端 |
| `FASTBEE_ENABLE_COAP` | **~1KB** | ~12KB | 0 | 0 | 1 | CoAP 协议 |

### 9.4 系统/Web/服务类

| 开关宏 | RAM | Flash | Lite | Std | Full | 说明 |
|--------|-----|-------|:----:|:---:|:----:|------|
| `FASTBEE_ENABLE_WEB_SERVER` | **~20-30KB** | ~25KB | 1 | 1 | 1 | Web 服务（核心） |
| `FASTBEE_ENABLE_PERIPH_EXEC` | **~2-4KB** | ~8KB | 1 | 1 | 1 | 外设执行规则 |
| `FASTBEE_ENABLE_RULE_SCRIPT` | **~4-8KB** | ~8KB | 0 | 1 | 1 | 规则脚本引擎 |
| `FASTBEE_ENABLE_COMMAND_SCRIPT` | **~1-2KB** | ~3KB | 0 | 1 | 1 | 命令脚本 |
| `FASTBEE_ENABLE_OTA` | **~4-6KB** | ~10KB | 0 | ⚠️ | 1 | OTA 升级 |
| `FASTBEE_ENABLE_STORAGE_CACHE` | **~2-4KB** | ~1KB | 0 | 0 | 0 | 配置缓存 |
| `FASTBEE_ENABLE_FILE_LOGGING` | **~500B** | ~3KB | 0 | 0 | 1 | 文件日志 |
| `FASTBEE_ENABLE_USER_ADMIN` | **~500B** | ~5KB | 0 | 0 | 1 | 多用户管理 |
| `FASTBEE_ENABLE_FILE_MANAGER` | **~500B** | ~3KB | 0 | 0 | 1 | 文件管理器 |
| `FASTBEE_ENABLE_HEALTH_MONITOR` | **~200B** | ~4KB | 1 | 1 | 1 | 健康监控 |
| `FASTBEE_ENABLE_LOGGER` | **~300B** | ~6KB | 1 | 1 | 1 | 日志系统 |

---

## 10. 典型配置方案

### 10.1 极致精简（ESP32-C3, 4MB, 无 PSRAM）

目标：最小 RAM 占用，仅保留核心功能。

```ini
[env:esp32c3-F4R0]
build_flags =
    ${esp32_base.build_flags}
    ${esp32c3_runtime_flags.build_flags}
    ; --- 功能开关 ---
    -DFASTBEE_USE_PSRAM=0
    -DFASTBEE_ENABLE_MQTT=1              ; 核心：MQTT 必须
    -DFASTBEE_ENABLE_MODBUS=0            ; 裁剪：不需要 Modbus
    -DFASTBEE_ENABLE_TCP=0
    -DFASTBEE_ENABLE_HTTP=0
    -DFASTBEE_ENABLE_COAP=0
    -DFASTBEE_ENABLE_ETHERNET=0          ; 裁剪：仅 WiFi
    -DFASTBEE_ENABLE_CELLULAR=0          ; 裁剪：无 4G 模块
    -DFASTBEE_ENABLE_BLE=0               ; BLE 已从所有预设移除
    -DFASTBEE_ENABLE_LCD=0              ; 裁剪：无 LCD 屏幕
    -DFASTBEE_ENABLE_LED_SCREEN=0
    -DFASTBEE_ENABLE_NEOPIXEL=0          ; 裁剪：无 LED 灯带
    -DFASTBEE_ENABLE_I2C_SENSORS=0       ; 裁剪：无高级传感器
    -DFASTBEE_ENABLE_RFID=0
    -DFASTBEE_ENABLE_IR_REMOTE=0
    -DFASTBEE_ENABLE_SENSOR_DRIVER=1     ; 保留：DHT/DS18B20
    -DFASTBEE_ENABLE_OTA=0              ; 裁剪：4MB 无空间 OTA
    -DFASTBEE_ENABLE_RULE_SCRIPT=0
    -DFASTBEE_ENABLE_COMMAND_SCRIPT=0
    -DFASTBEE_ENABLE_FILE_LOGGING=0
    -DFASTBEE_ENABLE_USER_ADMIN=0
    -DFASTBEE_ENABLE_FILE_MANAGER=0
    ; --- 性能调优 ---
    -DFASTBEE_STRIP_INFO_LOGS=1          ; 精简日志
    -DFASTBEE_LOG_BUFFER_SIZE=128        ; 减小日志缓冲
    -DFASTBEE_JSON_DOC_SIZE=2048         ; 减小 JSON 文档
    -DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=4 ; 减少 TCP 连接
    -DCORE_DEBUG_LEVEL=1
```

**预估节省**：相比默认 Lite，额外释放约 **~5KB RAM + ~60KB Flash**。

### 10.2 标准版（ESP32, 4MB, 无 PSRAM）

目标：保留 WiFi + MQTT + Modbus + 基础外设。标准预设默认关闭 4G、以太网和 BLE，给 no-PSRAM ESP32 留出更多内部 DRAM 余量。

```ini
[env:esp32-F4R0]
build_flags =
    ${esp32_base.build_flags}
    ${esp32_runtime_flags.build_flags}
    ${standard_flags.build_flags}        ; 标准预设
    -DFASTBEE_ENABLE_OTA=0              ; 4MB 不支持 OTA
    -DFASTBEE_ENABLE_OTA_FS=0
    -DFASTBEE_ENABLE_ETHERNET=0         ; 标准版默认关闭 → 省 ~8KB RAM
    -DFASTBEE_ENABLE_CELLULAR=0         ; 标准版默认关闭 → 省 ~10-15KB RAM
    -DFASTBEE_ENABLE_BLE=0              ; 所有版本默认关闭/移除
    ; --- 按硬件裁剪（无接的外设禁用） ---
    -DFASTBEE_ENABLE_LCD=0              ; 没接 LCD → 省 1.2KB RAM
    -DFASTBEE_ENABLE_RFID=0             ; 没接 RFID → 省 200B RAM
    -DFASTBEE_ENABLE_IR_REMOTE=0        ; 没接红外 → 省 300B RAM
```

### 10.3 全功能版（ESP32-S3, 16MB, 8MB PSRAM）

目标：所有功能全开，充分利用 PSRAM。

```ini
[env:esp32s3-F16R8]
build_flags =
    ${esp32_base.build_flags}
    ${esp32s3_runtime_flags.build_flags}
    ${full_flags.build_flags}            ; 全功能预设
    -DFASTBEE_USE_PSRAM=1
    -DBOARD_HAS_PSRAM
    -DFASTBEE_ENABLE_BLE=0               ; BLE 已从所有预设移除
    ; PSRAM 充裕，可加大 TCP 连接数
    -DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=8
    -DCONFIG_ASYNC_TCP_QUEUE_SIZE=8
```

---

## 11. 资源上限参考

各芯片型号的固件层资源上限（由 `ResourceProfile.h` 定义）：

| 参数 | ESP32-C3 (Lite) | ESP32 标准 (Std) | ESP32-S3 (Full) |
|------|:---:|:---:|:---:|
| 最大外设数 | 16 | 24 | 32 |
| 推荐执行规则数 | 12 | 16 | 32 |
| 传感器缓存条目 | 16 | 24 | 32 |
| TCP 并发预算 | 4 | 6 | 8 |
| SSE 连接预算 | 1 | 1 | 2 |
| HTTP 连接预算 | 3 | 5 | 6 |
| TCP 耗尽阈值 | 10 | 12 | 14 |

> **TCP 耗尽阈值**：当活跃 TCP 连接数超过此值时，系统会主动断开旧的 SSE 连接，防止 lwIP PCB 池耗尽（硬上限 16）。

---

## 12. 配置检查清单

在修改 `platformio.ini` 之前，按以下清单确认：

- [ ] 确认硬件上实际连接了哪些外设模块（LCD、传感器、RFID 等）
- [ ] 确认需要的联网方式（WiFi / 以太网 / 4G）
- [ ] 确认需要的协议（MQTT 是核心，Modbus/TCP/HTTP/CoAP 按需）
- [ ] 确认 Flash 大小是否支持 OTA（≥ 8MB 才支持）
- [ ] 确认是否有 PSRAM（有则启用 `FASTBEE_USE_PSRAM=1`）
- [ ] 未连接的外设模块务必禁用对应开关
- [ ] BLE 已从所有预设移除；若旧配置/旧分支仍引入 NimBLE，需同步清理依赖或加入 `lib_ignore`
- [ ] 修改后执行 `pio run -e <环境名>` 验证编译通过
- [ ] 检查编译输出的 RAM/Flash 使用量是否在安全范围
