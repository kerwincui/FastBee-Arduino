# PlatformIO 构建配置说明

> 本文档详细说明 `platformio.ini` 中各配置项的含义、功能开关的作用，以及如何根据开发/生产需求调整配置。

## 目录

- [配置结构概览](#配置结构概览)
- [公共基础配置 esp32_base](#公共基础配置-esp32_base)
- [功能版本 Flag 段](#功能版本-flag-段)
  - [lite_flags（精简版）](#lite_flags精简版)
  - [standard_flags（标准版）](#standard_flags标准版)
  - [full_flags（完整版）](#full_flags完整版)
- [运行时参数配置](#运行时参数配置)
- [源码过滤器 src_filter](#源码过滤器-src_filter)
- [构建环境 env](#构建环境-env)
- [功能开关速查表](#功能开关速查表)
- [按需编译策略](#按需编译策略)
- [日志等级配置](#日志等级配置)
- [库依赖说明](#库依赖说明)
- [常用构建命令](#常用构建命令)

---

## 配置结构概览

```
platformio.ini
├── [platformio]              ← 全局默认环境
├── [esp32_base]              ← ESP32 系列公共配置（平台、框架、库依赖）
├── [lite_flags]              ← 精简版功能开关
├── [standard_flags]          ← 标准版功能开关
├── [full_flags]              ← 完整版功能开关
├── [lite_src_filter]         ← 精简版源码过滤（排除不需要的 .cpp）
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

### lite_flags（精简版）

适用于 4MB Flash 的小容量设备（ESP32-C3/C6），关闭所有高级功能以节省固件空间。

### standard_flags（标准版）

适用于中等容量设备，保留 WiFi、MQTT、Modbus、规则脚本和常用外设，默认关闭 TCP/HTTP/CoAP、OTA、用户/角色管理、BLE、以太网和 4G，优先保证无 PSRAM ESP32 的内部 DRAM 余量。

### full_flags（完整版）

适用于 8MB+ Flash + PSRAM 的高端设备，启用 TCP/HTTP/CoAP、OTA、用户管理、以太网、4G 等完整功能。BLE 已从所有预设移除，`FASTBEE_ENABLE_BLE` 默认仍为 `0`。

**版本对比矩阵：**

| 功能 | lite | standard | full |
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
| BLE 蓝牙 | ✗ | ✗ | ✗ |
| 以太网 W5500 | ✗ | ✗ | ✓ |
| 4G 蜂窝 | ✗ | ✗ | ✓ |
| I2C 传感器 | ✗ | ✓ | ✓ |
| RFID | ✗ | ✓ | ✓ |
| 红外遥控 | ✗ | ✓ | ✗¹ |
| NeoPixel | ✓ | ✓ | ✓ |
| DS1302 RTC | ✗ | ✗ | ✓ |
| LCD1602 字符液晶 | ✗ | ✗ | ✓ |
| PSRAM | ✗ | ✗ | ✓ |
| 国际化 i18n | ✗ | ✗ | ✓ |

> ¹ ESP32-S3 的 RMT 驱动与 IRremoteESP8266 冲突，S3 环境显式禁用红外。
>
> 当前预设策略：标准版关闭以太网、4G 和 BLE；完整版保留以太网/4G，但 BLE 仍关闭。如确需恢复 BLE，需要重新加入 NimBLE 依赖并重新评估 Flash/RAM 余量。

---

## 运行时参数配置

运行时参数控制 ESP32 内部的 TCP 连接数、任务栈大小等，按芯片型号分四组：

| 参数 | ESP32 | ESP32-C3 | ESP32-C6 | ESP32-S3 | 说明 |
|------|:-----:|:--------:|:--------:|:--------:|------|
| `CONFIG_ASYNC_TCP_MAX_CONNECTIONS` | 6 | 4 | 6 | **14** | AsyncTCP 最大并发连接数 |
| `CONFIG_ASYNC_TCP_QUEUE_SIZE` | 8 | 4 | 8 | **24** | TCP 事件队列深度 |
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

`esp32-F8R4`、`esp32s3-F8R4` 和 `esp32s3-F16R8` 环境通过 `board_build.psram = enabled` 启用 PSRAM；`full_flags` 同时定义 `FASTBEE_USE_PSRAM=1` 和 `BOARD_HAS_PSRAM`：

```ini
[full_flags]
build_flags =
    -DFASTBEE_USE_PSRAM=1
    -DBOARD_HAS_PSRAM

; 在 [env:esp32-F8R4] / [env:esp32s3-F8R4] / [env:esp32s3-F16R8] 中
board_build.psram = enabled
```

无 PSRAM 的环境（`esp32s3-F8R0`、`esp32-F4R0` 等）不设置此项，`main.cpp` 中 `#ifdef BOARD_HAS_PSRAM` 分支不会编译。

---

## 源码过滤器 src_filter

通过 `build_src_filter` 排除不参与编译的 `.cpp` 文件，减小固件体积：

### lite_src_filter（精简版）

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
| `esp32c3-F4R0` | ESP32-C3 | 4MB | 无 | lite | fastbee.csv |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | 无 | standard+OTA | fastbee-8MB.csv |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | 无 | lite | fastbee.csv |
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

> 标准版中 `FASTBEE_ENABLE_ETHERNET=0`、`FASTBEE_ENABLE_CELLULAR=0`，前端能力接口会据此隐藏以太网/4G 选项；完整版保留二者为 `1`。

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
| `FASTBEE_ENABLE_DS1302` | DS1302 实时时钟（3线） |
| `FASTBEE_ENABLE_LCD1602` | LCD1602 I2C 字符液晶 |
| `FASTBEE_ENABLE_BLE` | BLE 蓝牙（NimBLE，当前预设已移除） |

> `FASTBEE_ENABLE_BLE` 当前所有预设均为 `0`，NimBLE-Arduino 已从 `lib_deps` 移除；宏保留用于兼容旧分支和私有配置。

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

## 按需编译策略

按需编译通过 `FASTBEE_ENABLE_*` 宏控制各驱动/功能是否编译进固件，实现 **Flash/RAM 资源按需分配**。

### 设计原则

1. **资源有限**：ESP32 系列 Flash 为 4-16MB，RAM 仅 320KB-8MB，不是每个设备都需要所有功能
2. **编译隔离**：禁用的功能不编译，节省 Flash 空间；对应驱动实例不创建，节省 RAM
3. **配置灵活**：在 `platformio.ini` 的 `build_flags` 中通过 `-DFASTBEE_ENABLE_XXX=1` 启用
4. **默认值分层**：基础功能（GPIO/传感器）默认启用；外设驱动按需启用

### 开关使用规则

功能开关的原则是：**设备实际不用的外设、驱动、协议和管理功能就关闭；设备确实接了硬件、业务流程确实依赖时再打开**。这样可以减少编译进固件的代码、全局对象、任务栈、协议缓冲区和第三方库依赖，直接节省 Flash 和运行时 RAM，降低长期运行时内存碎片、堆不足和任务栈紧张的风险。

| 场景 | 建议配置 | 说明 |
|------|----------|------|
| 没有接对应硬件 | `-DFASTBEE_ENABLE_XXX=0` | 不编译对应驱动，不创建运行时对象，节省 RAM/Flash |
| 硬件只是预留，当前版本不用 | `-DFASTBEE_ENABLE_XXX=0` | 预留硬件不等于软件必须启用，量产默认应关闭 |
| 已接硬件且业务要使用 | `-DFASTBEE_ENABLE_XXX=1` | 同时确认依赖库、引脚、UI/API 和配置项可用 |
| 仅开发调试使用 | 开发环境开，生产环境关 | 例如文件管理、日志查看器、详细日志、OTA 等 |
| RAM 占用高的通信模块 | 默认关闭，按项目打开 | 例如 BLE、4G、以太网、TCP/HTTP/CoAP |

建议先选择最接近硬件资源的预设：

- 4MB、无 PSRAM、功能少：优先 `lite_flags`
- ESP32 4MB/8MB、无 PSRAM、常规网关：优先 `standard_flags`
- ESP32-S3/ESP32 带 PSRAM、需要完整联网能力：优先 `full_flags`

然后只在具体 `[env:*]` 里覆盖少量差异。不要因为“以后可能会用”提前打开模块；后续需要时再把对应宏改为 `1` 重新编译即可。

### 资源配置影响分析

| 驱动 | Flash 占用 | RAM 占用 | 默认值 | 建议 |
|------|-----------|---------|--------|------|
| **MQTT** | ~15KB | ~2KB | 1 | 基础功能，保持启用 |
| **Modbus RTU** | ~20KB | ~1KB | 1 | 工业场景常用，保持启用 |
| **mDNS** | ~5KB | ~200B | 1 | 基础网络功能，保持启用 |
| **GPIO** | ~3KB | ~100B | 1 | 基础功能，保持启用 |
| **传感器驱动** | ~8KB | ~200B | 1 | 基础功能，保持启用 |
| **Web 服务器** | ~25KB | ~2KB | 1 | 基础功能，保持启用 |
| **认证/会话** | ~8KB | ~500B | 1 | 安全基础，保持启用 |
| **健康监控** | ~4KB | ~200B | 1 | 稳定性基础，保持启用 |
| NeoPixel | ~12KB | ~300B | **1**² | 可改为 0，多数设备不需要 |
| DS1302 RTC | ~2KB | ~100B | 0 | 按需启用（有 NTP 可替代） |
| LCD1602 | ~3KB | ~100B | 0 | 按需启用（有 OLED 可替代） |
| LCD/OLED | ~20KB | ~1.2KB | 0 | 按需启用（需 U8g2 库） |
| 七段数码管 | ~3KB | ~32B/实例 | 0 | 按需启用 |
| LED 点阵屏 | ~10KB | ~500B | 0 | 按需启用 |
| RFID MFRC522 | ~12KB | ~200B | 0 | 按需启用 |
| 红外遥控 | ~8KB | ~300B | 0 | 按需启用 |
| I2C 传感器 | ~15KB | ~500B | 0 | 按需启用 |
| BLE 蓝牙 | ~80KB | ~20-30KB | 0 | 已从所有预设移除，默认不要启用 |
| 以太网 W5500 | ~8KB | ~8KB | 0 | 标准版关闭；仅 Full/实际硬件需要时启用 |
| 4G 蜂窝 | ~15KB | ~10-15KB | 0 | 标准版关闭；RAM 占用高，仅 Full/实际硬件需要时启用 |
| TCP 客户端 | ~10KB | ~500B | 0 | 按需启用 |
| HTTP 客户端 | ~8KB | ~300B | 0 | 按需启用 |
| CoAP 协议 | ~12KB | ~500B | 0 | 按需启用 |
| 规则脚本 | ~8KB | ~2KB | 0 | 按需启用 |
| 命令脚本 | ~4KB | ~500B | 0 | 按需启用 |
| 用户管理 | ~4KB | ~200B | 0 | 按需启用 |
| 文件管理 | ~3KB | ~200B | 0 | 按需启用 |
| 日志查看器 | ~2KB | ~100B | 0 | 按需启用 |
| 文件日志 | ~2KB | ~200B | 0 | 按需启用 |

> 该表的“默认值”是保守基准；具体预设以 `lite_flags` / `standard_flags` / `full_flags` 为准。当前标准版关闭 Ethernet/Cellular/BLE，完整版开启 Ethernet/Cellular 但 BLE 仍关闭。
>
> ² **NEOPIXEL 优化建议**：NeoPixel LED 驱动占用 ~12KB Flash，在 lite 精简版本中也是默认启用的。
> 如果设备不使用 WS2812/NeoPixel 灯带，可在 `lite_flags` 中设置为 0 节省空间：
> ```ini
> -DFASTBEE_ENABLE_NEOPIXEL=0
> ```

### 资源节省效果

**Lite 版本（ESP32-C3/C6 4MB）** 按需编译后，仅保留基础功能：

| 类别 | 启用模块 | Flash 占用 |
|------|---------|----------|
| 协议 | MQTT | ~15KB |
| 网络 | mDNS + DNS + AP | ~10KB |
| Web | Server + Static + API | ~25KB |
| 安全 | Auth + Session | ~8KB |
| 系统 | Logger + Task + Health + Exec | ~17KB |
| 外设 | GPIO + Sensor | ~11KB |
| **合计基础** | | **~86KB** |

如启用全部可选驱动，Flash 增加约 **75KB**（NeoPixel 12KB + LCD 20KB + RFID 12KB + 红外 8KB + I2C 传感器 15KB + 其他 ~8KB）。

### 启用/关闭功能的方法

在 `platformio.ini` 对应环境段添加或覆盖 `build_flags`。启用使用 `1`，关闭使用 `0`：

```ini
[env:esp32s3-F16R8]
build_flags =
    ${esp32_base.build_flags}
    ${esp32s3_runtime_flags.build_flags}
    ${full_flags.build_flags}
    -DFASTBEE_ENABLE_DS1302=1    ; 启用 DS1302 实时时钟
    -DFASTBEE_ENABLE_LCD1602=1   ; 启用 LCD1602 字符液晶
    -DFASTBEE_ENABLE_CELLULAR=0  ; 未接 4G 模块则关闭，节省 RAM
    -DFASTBEE_ENABLE_ETHERNET=0  ; 未接 W5500 则关闭，节省 RAM
    -DFASTBEE_ENABLE_FILE_MANAGER=0 ; 生产环境不需要文件管理则关闭
```

也可以在 `[lite_flags]`、`[standard_flags]`、`[full_flags]` 中统一调整某个版本的默认值。直接修改 `FeatureFlags.h` 会影响所有未显式覆盖的环境，建议只用于定义全局兜底默认值。

> 注意：关闭宏主要影响编译和运行时对象创建；设备上已保存的历史配置数据可能仍在 LittleFS/NVS 中，但对应功能关闭后不会再创建驱动实例或执行相关逻辑。需要彻底清理配置时，再配合配置迁移或恢复出厂设置处理。

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
| U8g2 | ^2.35.9 | LCD/OLED 显示驱动 |
| Adafruit NeoPixel | ^1.12.0 | WS2812 LED 控制 |
| DHT sensor library | ^1.4.6 | DHT11/22 温湿度传感器 |
| Adafruit Unified Sensor | ^1.1.14 | 传感器统一接口 |
| OneWire | ^2.3.8 | 单总线协议（DS18B20） |
| DallasTemperature | ^3.9.0 | DS18B20 温度传感器 |

> BLE 已从所有预设移除，核心依赖中不再包含 NimBLE-Arduino。旧分支如需恢复 BLE，需要同时恢复依赖、设置 `FASTBEE_ENABLE_BLE=1`，并重新评估约 80KB Flash 和 20-30KB RAM 的占用。

### 扩展依赖（按环境声明，功能仍受宏控制）

| 库名 | 版本 | 用途 |
|------|------|------|
| Adafruit BMP280 | ^2.6.8 | 气压/温度传感器 |
| Adafruit MPU6050 | ^2.2.6 | 六轴姿态传感器 |
| MFRC522 | ^1.4.11 | RFID 读卡器 |
| IRremoteESP8266 | ^2.8.6 | 红外遥控（ESP32 原生） |
| TinyGSM | ^0.11.7 | 4G 蜂窝模块驱动；标准预设关闭 `FASTBEE_ENABLE_CELLULAR`，完整版启用 |

### 单元测试依赖（native 环境）

| 库名 | 版本 | 用途 |
|------|------|------|
| ArduinoJson | 7.4.2 | JSON 处理 |
| Unity | ^2.6.0 | 单元测试框架 |

### lib_ignore

部分环境排除不兼容或已移除的库：

| 环境 | 排除的库 | 原因 |
|------|----------|------|
| `esp32-F4R0` | NimBLE-Arduino | BLE 已移除，保留排除项防止旧依赖被拉入；4MB Flash 空间不足 |
| `esp32c3-F4R0` | NimBLE-Arduino | 同上 |
| `esp32c6-F4R0` | NimBLE-Arduino, OneWire, DallasTemperature | BLE 已移除；C6 不支持 OneWire/DS18B20 |
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
