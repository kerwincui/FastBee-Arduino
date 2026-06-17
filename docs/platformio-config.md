# PlatformIO 构建配置说明

> 本文档详细说明 `platformio.ini` 中各配置项的含义、功能开关的作用，以及如何根据开发/生产需求调整配置。

## 目录

- [配置结构概览](#配置结构概览)
- [公共基础配置 esp32_base](#公共基础配置-esp32_base)
- [功能版本 Flag 段](#功能版本-flag-段)
  - [slim_flags（精简版）](#slim_flags精简版)
  - [standard_flags（标准版）](#standard_flags标准版)
  - [full_flags（完整版）](#full_flags完整版)
- [运行时参数配置](#运行时参数配置)
- [源码过滤器 src_filter](#源码过滤器-src_filter)
- [构建环境 env](#构建环境-env)
- [功能开关速查表](#功能开关速查表)
- [日志等级配置](#日志等级配置)
- [库依赖说明](#库依赖说明)
- [常用构建命令](#常用构建命令)

---

## 配置结构概览

```
platformio.ini
├── [platformio]              ← 全局默认环境
├── [esp32_base]              ← ESP32 系列公共配置（平台、框架、库依赖）
├── [slim_flags]              ← 精简版功能开关
├── [standard_flags]          ← 标准版功能开关
├── [full_flags]              ← 完整版功能开关
├── [slim_src_filter]         ← 精简版源码过滤（排除不需要的 .cpp）
├── [standard_src_filter]     ← 标准版源码过滤
├── [standard_ota_src_filter] ← 标准版+OTA 源码过滤
├── [esp32_runtime_flags]     ← ESP32 运行时参数（TCP/栈大小）
├── [esp32c3_runtime_flags]   ← ESP32-C3 运行时参数
├── [esp32c6_runtime_flags]   ← ESP32-C6 运行时参数
├── [esp32s3_runtime_flags]   ← ESP32-S3 运行时参数
├── [env:esp32-F4R0]          ← ESP32 4MB Flash, 无 PSRAM
├── [env:esp32-F8R4]          ← ESP32 8MB Flash, 4MB PSRAM
├── [env:esp32c3-F4R0]        ← ESP32-C3 4MB Flash
├── [env:esp32s3-F8R0]        ← ESP32-S3 8MB Flash, 无 PSRAM
├── [env:esp32c6-F4R0]        ← ESP32-C6 4MB Flash
├── [env:esp32s3-F8R4]        ← ESP32-S3 8MB Flash, 4MB PSRAM
├── [env:esp32s3-F16R8]       ← ESP32-S3 16MB Flash, 8MB PSRAM
└── [env:native]              ← 本机单元测试（不烧录硬件）
```

各 `[env:*]` 通过 `${xxx.yyy}` 语法引用公共段，实现配置复用。

---

## 公共基础配置 esp32_base

```ini
[esp32_base]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38/platform-espressif32.zip
framework = arduino
build_type = release
board_build.filesystem = littlefs
board_build.partitions = partitions/fastbee.csv
```

| 配置项 | 说明 |
|--------|------|
| `platform` | ESP-IDF 平台版本，使用 pioarduino 55.03.38 发布版 |
| `framework` | Arduino 框架 |
| `build_type` | `release` 开启编译器优化（`-Os`），改为 `debug` 可启用调试符号 |
| `board_build.filesystem` | 使用 LittleFS 作为文件系统 |
| `board_build.partitions` | 分区表，默认 4MB 使用 `fastbee.csv`，8MB/16MB 使用对应分区表 |

### 预构建/后构建脚本

```ini
extra_scripts =
    pre:scripts/kill-stale-processes.py    ; 构建前清理残留串口进程，防止文件锁
    pre:scripts/fastbee-artifacts.py       ; 构建前准备
    post:scripts/fastbee-artifacts.py      ; 构建后归档固件到 dist/
```

### 公共 build_flags

以下开关在所有版本中默认启用：

| 宏定义 | 默认值 | 说明 |
|--------|--------|------|
| `FASTBEE_ENABLE_MQTT` | 1 | MQTT 协议支持 |
| `FASTBEE_ENABLE_MDNS` | 1 | mDNS 域名服务（`fastbee.local`） |
| `FASTBEE_ENABLE_AP_MODE` | 1 | WiFi AP 热点模式 |
| `FASTBEE_ENABLE_WEB_SERVER` | 1 | AsyncWebServer Web 服务器 |
| `FASTBEE_ENABLE_WEB_STATIC` | 1 | 静态文件服务（LittleFS） |
| `FASTBEE_ENABLE_WEB_API` | 1 | REST API 路由 |
| `FASTBEE_ENABLE_AUTH` | 1 | 用户认证 |
| `FASTBEE_ENABLE_SESSION` | 1 | 会话管理 |
| `FASTBEE_ENABLE_TASK_MANAGER` | 1 | 任务调度管理 |
| `FASTBEE_ENABLE_HEALTH_MONITOR` | 1 | 系统健康监控 |
| `FASTBEE_ENABLE_LOGGER` | 1 | 日志系统 |
| `FASTBEE_ENABLE_PERIPH_EXEC` | 1 | 外设执行引擎 |
| `FASTBEE_ENABLE_GPIO` | 1 | GPIO 控制 |
| `FASTBEE_ENABLE_SENSOR_DRIVER` | 1 | 传感器驱动 |
| `FASTBEE_ENABLE_LCD` | 1 | LCD 显示 |
| `FASTBEE_ENABLE_SEVEN_SEGMENT` | 1 | 七段数码管 |
| `FASTBEE_ENABLE_DNS` | 1 | DNS 解析 |
| `FASTBEE_LOG_BUFFER_SIZE` | 256 | 单条日志缓冲区大小（字节） |
| `FASTBEE_JSON_DOC_SIZE` | 8192 | ArduinoJson 文档分配大小（字节） |
| `ASYNCWEBSERVER_REGEX` | 1 | 启用 URL 正则匹配 |
| `NDEBUG` | - | 关闭标准库 assert（release 模式） |

---

## 功能版本 Flag 段

项目提供三个功能级别，通过 `build_flags` 引用切换：

### slim_flags（精简版）

适用于 4MB Flash 的小容量设备（ESP32-C3/C6），关闭所有高级功能以节省固件空间。

### standard_flags（标准版）

适用于中等容量设备，保留 Modbus、规则脚本、以太网、4G 等实用功能，关闭用户/角色管理和 BLE。

### full_flags（完整版）

适用于 8MB+ Flash + PSRAM 的高端设备（ESP32-S3），启用全部功能。

**版本对比矩阵：**

| 功能 | slim | standard | full |
|------|:----:|:--------:|:----:|
| TCP 客户端 | ✗ | ✗ | ✓ |
| HTTP 客户端 | ✗ | ✗ | ✓ |
| CoAP 协议 | ✗ | ✗ | ✓ |
| Modbus RTU | ✗ | ✓ | ✓ |
| OTA 升级 | ✗ | ✗ | ✓ |
| 规则脚本 | ✗ | ✓ | ✓ |
| 命令脚本 | ✗ | ✓ | ✓ |
| 用户管理 | ✗ | ✗ | ✓ |
| 角色管理 | ✗ | ✗ | ✓ |
| 文件管理 | ✗ | ✗ | ✓ |
| 日志查看器 | ✗ | ✗ | ✓ |
| 文件日志 | ✗ | ✗ | ✓ |
| BLE 蓝牙 | ✗ | ✗ | ✓ |
| 以太网 W5500 | ✗ | ✓ | ✓ |
| 4G 蜂窝 | ✗ | ✓ | ✓ |
| LoRa 网关 | ✗ | ✗ | ✓ |
| I2C 传感器 | ✗ | ✓ | ✓ |
| RFID | ✗ | ✓ | ✓ |
| 红外遥控 | ✗ | ✓ | ✗¹ |
| NeoPixel | ✓ | ✓ | ✓ |
| PSRAM | ✗ | ✗ | ✓ |
| 国际化 i18n | ✗ | ✗ | ✓ |

> ¹ ESP32-S3 的 RMT 驱动与 IRremoteESP8266 冲突，S3 环境显式禁用红外。

---

## 运行时参数配置

运行时参数控制 ESP32 内部的 TCP 连接数、任务栈大小等，按芯片型号分四组：

| 参数 | ESP32 | ESP32-C3 | ESP32-C6 | ESP32-S3 | 说明 |
|------|:-----:|:--------:|:--------:|:--------:|------|
| `CONFIG_ASYNC_TCP_MAX_CONNECTIONS` | 2 | 1 | 3 | **14** | AsyncTCP 最大并发连接数 |
| `CONFIG_ASYNC_TCP_QUEUE_SIZE` | 4 | 4 | 8 | **24** | TCP 事件队列深度 |
| `ARDUINO_LOOP_STACK_SIZE` | 16384 | 12288 | 12288 | 16384 | Arduino loopTask 栈大小（字节） |
| `CONFIG_ASYNC_TCP_TASK_WDT_TIMEOUT` | 10 | 10 | 10 | 10 | AsyncTCP 任务看门狗超时（秒） |
| `SCRIPT_TASK_STACK` | 8192 | 6144 | 6144 | 8192 | 规则脚本任务栈大小 |
| `SIMPLE_TASK_STACK` | 6144 | 4096 | 4096 | 6144 | 简单定时任务栈大小 |

> **S3 参数说明**：ESP32-S3 配 8MB+ PSRAM，内部 DRAM 在 WiFi+MQTT+WebServer 全开后仅剩 ~18-35KB。
> 将 TCP 连接数提升至 14、队列深度提升至 24，可支撑多标签页并发访问（峰值 10-14 PCB）。
> 配合 PSRAM 阈值（见下文）将 HTTP 缓冲区卸载到 PSRAM，内部 DRAM 专供 lwIP TCP PCB（~172B/个，必须内部 DRAM）。

ESP32-C3/C6 还额外配置 USB 串口模式：
```ini
-DARDUINO_USB_MODE=1           ; USB-JTAG 模式
-DARDUINO_USB_CDC_ON_BOOT=1    ; 启用 USB CDC 串口
```

> **调优提示**：
> - 如遇栈溢出崩溃（`Guru Meditation Error: Stack canary watchpoint triggered`），可增大对应 `*_STACK` 值。
> - 增大 `CONFIG_ASYNC_TCP_MAX_CONNECTIONS` 可支持更多并发 Web 请求，但会占用更多 RAM。
> - S3 有 PSRAM 时建议同步降低 PSRAM 阈值（见 PSRAM 配置段），释放内部 DRAM 供 TCP PCB 使用。

---

## PSRAM 配置

### 背景

ESP32-S3 内部 DRAM 仅 ~320KB，WiFi 协议栈 + MQTT + AsyncWebServer 全开后，内部 DRAM 可用空间常仅剩 **18-35KB**。
若 HTTP 请求缓冲区、ArduinoJson 序列化 `String` 等大分配无法卸载到 PSRAM，会导致 `new` 失败 → `abort()`。

### 阈值配置

通过 `heap_caps_malloc_extmem_enable(threshold)` 控制分配策略：

```cpp
// src/main.cpp — setup() 中调用
heap_caps_malloc_extmem_enable(512);  // ≥ 512B 的分配请求优先用 PSRAM
```

| 阈值 | 效果 | 适用场景 |
|------|------|----------|
| `4096`（旧默认） | 仅 ≥ 4KB 的分配用 PSRAM | PSRAM 较小或无 HTTP 并发压力 |
| **`512`（当前）** | ≥ 512B 的分配用 PSRAM | ESP32-S3 + PSRAM，AsyncWebServer 并发场景 |

### 为什么是 512 而不是 4096？

| 分配类型 | 典型大小 | 能否用 PSRAM |
|----------|----------|-------------|
| AsyncWebServer HTTP 缓冲区 | 1-2 KB | ✅ 可以（512 阈值命中，4096 阈值无法命中） |
| ArduinoJson String 序列化 | 200B - 2KB | ✅ 可以 |
| lwIP TCP PCB 结构体 | ~172 B | ❌ 必须内部 DRAM（硬件要求） |
| FreeRTOS 任务栈 | 2-16 KB | ⚠️ 可以但延迟略高 |
| WiFi 协议栈缓冲 | 不等 | ❌ 内部 DRAM |

> **PSRAM 延迟**：PSRAM 通过 SPI 访问，延迟比内部 DRAM 高约 3x，但对 HTTP/JSON 响应构建无感知影响（瓶颈在网络 I/O）。

### 构建时启用 PSRAM

`esp32s3-F8R4` 和 `esp32s3-F16R8` 环境通过 `board_build.arduino.memory_type` 自动启用 PSRAM：

```ini
; 在 [env:esp32s3-F8R4] / [env:esp32s3-F16R8] 中
board_build.arduino.memory_type = qio_opi   ; OPI PSRAM（8线，速度更快）
```

无 PSRAM 的环境（`esp32s3-F8R0`、`esp32-F4R0` 等）不设置此项，`main.cpp` 中 `#ifdef BOARD_HAS_PSRAM` 分支不会编译。

---

## 源码过滤器 src_filter

通过 `build_src_filter` 排除不参与编译的 `.cpp` 文件，减小固件体积：

### slim_src_filter（精简版）

排除最多文件：OTA、用户/角色管理、日志、规则脚本、TCP/HTTP/CoAP 协议。

### standard_src_filter（标准版，无 OTA）

排除 OTA、用户/角色管理、日志、TCP/HTTP/CoAP，但保留 RuleScript。

### standard_ota_src_filter（标准版 + OTA）

排除用户/角色管理、日志、TCP/HTTP/CoAP，保留 OTA 和 RuleScript。

> **注意**：`build_src_filter` 仅排除 `.cpp` 源文件。对应的头文件 `.h` 仍会被包含（通过 `#if` 宏控制跳过实现），因此排除源文件时对应的功能宏必须设为 `0`。

---

## 构建环境 env

每个 `[env:*]` 代表一个具体的目标硬件配置：

### 命名规则

`env:{芯片}-{Flash大小}{PSRAM大小}`

- `F4` = 4MB Flash，`F8` = 8MB，`F16` = 16MB
- `R0` = 无 PSRAM，`R4` = 4MB PSRAM，`R8` = 8MB PSRAM

### 环境列表

| 环境 | 芯片 | Flash | PSRAM | 功能版本 | 分区表 |
|------|------|-------|-------|----------|--------|
| `esp32-F4R0` | ESP32 | 4MB | 无 | standard | fastbee.csv |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | full | fastbee-8MB.csv |
| `esp32c3-F4R0` | ESP32-C3 | 4MB | 无 | slim | fastbee.csv |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | 无 | standard+OTA | fastbee-8MB.csv |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | 无 | slim | fastbee.csv |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | full | fastbee-8MB.csv |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | full | fastbee-16MB.csv |
| `native` | 本机 | - | - | 单元测试 | - |

### native 环境

本机单元测试环境，不烧录硬件：

```ini
[env:native]
platform = native
test_framework = unity
test_build_src = no           ; 不自动编译 src/，由测试手动引入
```

---

## 功能开关速查表

所有 `FASTBEE_ENABLE_*` 宏取值为 `0`（禁用）或 `1`（启用）。

### 网络与协议

| 宏定义 | 说明 | 依赖库 |
|--------|------|--------|
| `FASTBEE_ENABLE_MQTT` | MQTT 客户端 | PubSubClient |
| `FASTBEE_ENABLE_MDNS` | mDNS 服务 | ESPmDNS |
| `FASTBEE_ENABLE_AP_MODE` | WiFi AP 热点 | - |
| `FASTBEE_ENABLE_DNS` | DNS 解析 | - |
| `FASTBEE_ENABLE_TCP` | 原生 TCP 客户端 | - |
| `FASTBEE_ENABLE_HTTP` | HTTP 客户端 | - |
| `FASTBEE_ENABLE_COAP` | CoAP 协议 | - |
| `FASTBEE_ENABLE_MODBUS` | Modbus RTU 主站 | - |
| `FASTBEE_MODBUS_SLAVE_ENABLE` | Modbus RTU 从站 | - |
| `FASTBEE_ENABLE_ETHERNET` | W5500 以太网 | - |
| `FASTBEE_ENABLE_CELLULAR` | EC801E 4G 蜂窝 | TinyGSM |
| `FASTBEE_ENABLE_LORA` | E22 LoRa 网关 | - |

### Web 服务

| 宏定义 | 说明 |
|--------|------|
| `FASTBEE_ENABLE_WEB_SERVER` | AsyncWebServer 服务器 |
| `FASTBEE_ENABLE_WEB_STATIC` | LittleFS 静态文件服务 |
| `FASTBEE_ENABLE_WEB_API` | REST API 路由注册 |
| `FASTBEE_ENABLE_AUTH` | JWT/Session 用户认证 |
| `FASTBEE_ENABLE_SESSION` | 会话管理 |
| `FASTBEE_WEB_START_EARLY` | 网络就绪前启动 Web（调试用） |

### 系统管理

| 宏定义 | 说明 |
|--------|------|
| `FASTBEE_ENABLE_OTA` | OTA 空中升级 |
| `FASTBEE_ENABLE_OTA_FS` | OTA 文件系统更新 |
| `FASTBEE_ENABLE_FILE_MANAGER` | Web 文件管理器 |
| `FASTBEE_ENABLE_CONFIG_TRANSFER` | 配置导入/导出 |
| `FASTBEE_ENABLE_LOG_VIEWER` | Web 日志查看器 |
| `FASTBEE_ENABLE_FILE_LOGGING` | 日志写入文件 |
| `FASTBEE_ENABLE_USER_ADMIN` | 多用户管理 |
| `FASTBEE_ENABLE_ROLE_ADMIN` | 角色权限管理 |
| `FASTBEE_SINGLE_ADMIN_MODE` | 单管理员模式（简化认证） |

### 外设与传感器

| 宏定义 | 说明 |
|--------|------|
| `FASTBEE_ENABLE_GPIO` | GPIO 输入/输出控制 |
| `FASTBEE_ENABLE_PERIPH_EXEC` | 外设执行引擎 |
| `FASTBEE_ENABLE_SENSOR_DRIVER` | 传感器驱动框架 |
| `FASTBEE_ENABLE_I2C_SENSORS` | I2C 传感器（BMP280/MPU6050） |
| `FASTBEE_ENABLE_DS18B20` | DS18B20 温度传感器 |
| `FASTBEE_ENABLE_RFID` | MFRC522 RFID 读卡器 |
| `FASTBEE_ENABLE_IR_REMOTE` | 红外遥控 |
| `FASTBEE_ENABLE_LCD` | LCD 显示屏 |
| `FASTBEE_ENABLE_LED_SCREEN` | LED 点阵屏 |
| `FASTBEE_ENABLE_SEVEN_SEGMENT` | 七段数码管 |
| `FASTBEE_ENABLE_NEOPIXEL` | WS2812 NeoPixel LED |
| `FASTBEE_ENABLE_BLE` | BLE 蓝牙（NimBLE） |

### 脚本引擎

| 宏定义 | 说明 |
|--------|------|
| `FASTBEE_ENABLE_RULE_SCRIPT` | 规则脚本引擎 |
| `FASTBEE_ENABLE_COMMAND_SCRIPT` | 命令脚本引擎 |

### 系统基础

| 宏定义 | 说明 |
|--------|------|
| `FASTBEE_ENABLE_LOGGER` | 日志系统 |
| `FASTBEE_ENABLE_TASK_MANAGER` | 任务调度器 |
| `FASTBEE_ENABLE_HEALTH_MONITOR` | 系统健康监控 |
| `FASTBEE_ENABLE_STORAGE_CACHE` | 存储缓存层 |
| `FASTBEE_USE_PSRAM` | 启用 PSRAM 外部 RAM |
| `FASTBEE_ENABLE_I18N` | 国际化多语言 |

---

## 日志等级配置

日志系统有**编译时**和**运行时**两层控制。

### 编译时控制

| 宏定义 | 值 | 说明 |
|--------|---|------|
| `FASTBEE_STRIP_INFO_LOGS` | `0` | 保留 INFO 日志（开发） |
| `FASTBEE_STRIP_INFO_LOGS` | `1` | **编译时移除** INFO 日志（生产，节省 Flash） |
| `FASTBEE_DEBUG_LOG` | `0` | 移除 DEBUG/VERBOSE 日志（默认） |
| `FASTBEE_DEBUG_LOG` | `1` | 启用 DEBUG/VERBOSE 日志（开发） |
| `CORE_DEBUG_LEVEL` | `0`-`5` | ESP-IDF 内核日志级别 |

`CORE_DEBUG_LEVEL` 可选值：

| 值 | 级别 | 输出内容 |
|:--:|------|----------|
| 0 | None | 无输出 |
| 1 | Error | 仅错误（默认） |
| 2 | Warn | 警告 + 错误 |
| 3 | Info | 信息 + 警告 + 错误 |
| 4 | Debug | 调试 + 以上所有 |
| 5 | Verbose | 最详细输出 |

### 日志层级关系

```
LOG_VERBOSE → LOG_DEBUG → LOG_INFO → LOG_WARNING → LOG_ERROR → LOG_FATAL
   ↑ DEBUG_LOG=1 时生效    ↑ STRIP_INFO=0 时生效     ↑ 始终输出
```

### 开发调试配置

在对应 `[xxx_flags]` 段修改：

```ini
; 开启全部应用日志
-DFASTBEE_STRIP_INFO_LOGS=0
-DFASTBEE_DEBUG_LOG=1

; 开启 ESP-IDF 内核调试日志
-DCORE_DEBUG_LEVEL=4
```

### 生产发布配置

```ini
; 移除 INFO 和 DEBUG 日志，节省 Flash
-DFASTBEE_STRIP_INFO_LOGS=1
-DFASTBEE_DEBUG_LOG=0
-DCORE_DEBUG_LEVEL=1
```

---

## 库依赖说明

### 核心依赖（所有环境）

| 库名 | 版本 | 用途 |
|------|------|------|
| ArduinoJson | 7.4.2 | JSON 序列化/反序列化 |
| PubSubClient | ^2.8 | MQTT 客户端 |
| ESPAsyncWebServer | ^3.6.0 | 异步 Web 服务器 |
| AsyncTCP | ^3.3.2 | 异步 TCP（ESPAsyncWebServer 底层） |
| NimBLE-Arduino | ^2.2.0 | BLE 低功耗蓝牙 |
| U8g2 | ^2.35.9 | LCD/OLED 显示驱动 |
| Adafruit NeoPixel | ^1.12.0 | WS2812 LED 控制 |
| DHT sensor library | ^1.4.6 | DHT11/22 温湿度传感器 |
| Adafruit Unified Sensor | ^1.1.14 | 传感器统一接口 |
| OneWire | ^2.3.8 | 单总线协议（DS18B20） |
| DallasTemperature | ^3.9.0 | DS18B20 温度传感器 |

### 扩展依赖（standard/full 环境）

| 库名 | 版本 | 用途 |
|------|------|------|
| Adafruit BMP280 | ^2.6.8 | 气压/温度传感器 |
| Adafruit MPU6050 | ^2.2.6 | 六轴姿态传感器 |
| MFRC522 | ^1.4.11 | RFID 读卡器 |
| IRremoteESP8266 | ^2.8.6 | 红外遥控（ESP32 原生） |
| TinyGSM | ^0.11.7 | 4G 蜂窝模块驱动 |

### 单元测试依赖（native 环境）

| 库名 | 版本 | 用途 |
|------|------|------|
| ArduinoJson | 7.4.2 | JSON 处理 |
| Unity | ^2.6.0 | 单元测试框架 |

### lib_ignore

部分环境排除不兼容的库：

| 环境 | 排除的库 | 原因 |
|------|----------|------|
| `esp32-F4R0` | NimBLE-Arduino | 4MB Flash 空间不足 |
| `esp32c3-F4R0` | NimBLE-Arduino | 同上 |
| `esp32c6-F4R0` | NimBLE, OneWire, DallasTemperature | C6 不支持 |
| `esp32s3-F8R4/F16R8` | IRremoteESP8266 | ESP32-S3 RMT 驱动冲突 |

---

## 常用构建命令

```bash
# 编译默认环境（esp32-F4R0）
pio run

# 编译指定环境
pio run -e esp32s3-F16R8

# 编译并烧录固件
pio run -e esp32s3-F16R8 -t upload

# 烧录文件系统（Web 静态资源）
pio run -e esp32s3-F16R8 -t uploadfs

# 编译所有环境
pio run

# 运行本机单元测试
pio test -e native

# 串口监视器
pio device monitor -e esp32s3-F16R8

# 清除编译缓存
pio run -e esp32s3-F16R8 -t clean

# 查看所有构建环境
pio project config
```

### 构建产物

编译完成后固件归档到 `dist/` 目录：

```
dist/firmware/{env}/
├── factory.bin     ← 完整固件（含 bootloader + 分区表）
├── firmware.bin    ← 应用固件
└── partitions.bin  ← 分区表
```
