# FastBee-Arduino 项目架构文档

> 版本：1.0.0 | 芯片：ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 | 框架：Arduino-ESP32 3.x (IDF 5.1)

## 1. 项目概述

FastBee-Arduino 是面向 ESP32 全系列的**零代码 Web 物联网固件**。烧录后通过浏览器即可完成网络、设备、协议、外设和规则配置，无需二次编程。

**核心定位**：ESP32 节点、轻量网关和现场采集控制终端。

**技术栈**：
- 固件：C++ / Arduino-ESP32 3.x / ESP-IDF 5.1 / PlatformIO
- Web 前端：原生 JavaScript（无框架依赖）/ CSS / HTML
- 通信协议：MQTT / Modbus RTU / TCP / HTTP / CoAP
- 网络接入：WiFi (AP+STA) / 以太网 (W5500) / 4G (EC801E)
- 存储：LittleFS + NVS (Preferences)
- 构建：PlatformIO + 功能编译开关（`FASTBEE_ENABLE_*`）

---

## 2. 目录结构

```
FastBee-Arduino/
├── include/                # 头文件（90 个 .h）
│   ├── core/               # 核心框架：FastBeeFramework、外设管理、执行引擎、功能开关
│   │   └── interfaces/     # 抽象接口：IAuthManager、INetworkManager、IConfigStorage 等
│   ├── network/            # 网络层：WiFi、以太网、4G、DNS、IP 管理
│   │   └── handlers/       # Web 路由处理器（14 个 RouteHandler）
│   ├── protocols/          # 协议层：MQTT、Modbus、TCP、HTTP、CoAP
│   ├── peripherals/        # 外设驱动：LCD、传感器、数码管、RFID、红外
│   │   └── drivers/        # 具体外设驱动实现
│   ├── security/           # 安全模块：认证、用户管理、加密工具
│   ├── systems/            # 系统服务：健康监控、配置存储、日志、任务调度、重启诊断
│   └── utils/              # 工具库：字符串、时间、文件、JSON、堆碎片分析
├── src/                    # 源文件实现（73 个 .cpp）
│   ├── core/               # 核心框架实现
│   ├── network/            # 网络层实现
│   │   └── handlers/       # Web 路由处理器实现
│   ├── protocols/          # 协议层实现
│   ├── peripherals/        # 外设驱动实现
│   │   └── drivers/        # 具体驱动实现
│   ├── security/           # 安全模块实现
│   ├── systems/            # 系统服务实现
│   └── utils/              # 工具库实现
├── web-src/                # Web 前端源码（66 个文件）
│   ├── css/                # 样式表（main.css ~2140 行）
│   ├── js/                 # 核心 JS 引擎（状态管理、SSE、请求治理、主题）
│   ├── i18n/               # 国际化（中文 zh-CN / 英文 en）
│   ├── modules/            # 页面模块
│   │   ├── runtime/        # 运行时模块（仪表盘、设备配置、网络、外设、协议）
│   │   └── admin/          # 管理模块（用户、文件、日志、规则脚本）
│   └── pages/              # HTML 页面模板和片段
├── test/                   # 单元测试（37 个 .cpp，native 环境运行）
├── scripts/                # 构建/部署/测试/验证脚本（43 个）
├── partitions/             # Flash 分区表（4MB / 8MB / 16MB）
├── data/                   # LittleFS 默认配置与 Web 构建产物
├── docs/                   # 项目文档
├── dist/                   # 发布固件产物
└── platformio.ini          # PlatformIO 构建配置
```

---

## 3. 系统架构

### 3.1 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                       Web 浏览器 (前端)                          │
│   登录 → 仪表盘 / 设备配置 / 网络 / 协议 / 外设 / 外设执行      │
└──────────────────────┬──────────────────────────────────────────┘
                       │ HTTP REST API + SSE 实时推送
┌──────────────────────▼──────────────────────────────────────────┐
│                    AsyncWebServer (端口 80)                       │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              WebConfigManager (瘦协调器)                      │ │
│  │  StaticHandler │ AuthHandler │ SystemHandler │ DeviceHandler │ │
│  │  PeripheralHandler │ PeriphExecHandler │ ProtocolHandler     │ │
│  │  NetworkHandler │ BatchHandler │ OTAHandler │ SSEHandler     │ │
│  └─────────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                    FastBeeFramework (单例核心)                    │
│                                                                   │
│  ┌──────────┐ ┌──────────────┐ ┌──────────────┐ ┌────────────┐ │
│  │ Network  │ │  Protocol    │ │  Peripheral  │ │  Security  │ │
│  │ Manager  │ │  Manager     │ │  Manager     │ │  Manager   │ │
│  │          │ │              │ │              │ │            │ │
│  │ WiFi AP/ │ │ MQTT Client  │ │ GPIO/UART/   │ │ Auth Mgr   │ │
│  │ STA      │ │ Modbus RTU   │ │ I2C/SPI      │ │ User Mgr   │ │
│  │ Ethernet │ │ TCP Server   │ │ LCD/Sensor   │ │ Crypto     │ │
│  │ 4G Cell  │ │ HTTP Client  │ │ NeoPixel     │ │ Session    │ │
│  └──────────┘ └──────────────┘ └──────────────┘ └────────────┘ │
│                                                                   │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────────────────┐ │
│  │ PeriphExec   │ │ Health       │ │ Task Manager             │ │
│  │ Manager      │ │ Monitor      │ │ (定时任务调度)            │ │
│  │              │ │              │ │                          │ │
│  │ 定时/事件/   │ │ 内存4级保护  │ │ 健康检查/网络检测/       │ │
│  │ MQTT/条件    │ │ DRAM专项监控 │ │ 数据同步/状态更新        │ │
│  │ 触发执行     │ │ 碎片紧凑化   │ │                          │ │
│  └──────────────┘ └──────────────┘ └──────────────────────────┘ │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────────┐│
│  │               ConfigStorage (NVS + LittleFS JSON)            ││
│  └──────────────────────────────────────────────────────────────┘│
└───────────────────────────────────────────────────────────────────┘
```

### 3.2 初始化流程

`FastBeeFramework::initialize()` 按 9 个阶段顺序执行，每阶段有独立计时日志：

| 阶段 | 方法 | 初始化内容 | 关键依赖 |
|------|------|-----------|---------|
| 1 | `initStorageAndFS()` | ConfigStorage(NVS)、LittleFS、FileUtils | 无 |
| 2 | `initLogger()` | LoggerSystem（之后统一使用 LOG 宏） | Stage 1 |
| 3 | `initWebServer()` | AsyncWebServer 实例创建 | Stage 1 |
| 4 | `initNetwork()` | FBNetworkManager（WiFi/以太网/4G） | Stage 1,2 |
| 5 | `initSecurity()` | UserManager、AuthManager | Stage 1,2 |
| 6 | `initWebConfig()` | WebConfigManager + 14 个 RouteHandler | Stage 3,4,5 |
| 7 | `initOTA()` | OTAManager | Stage 3 |
| 8 | `initSystems()` | TaskManager、HealthMonitor、外设管理 | Stage 1-6 |
| 9 | `initProtocols()` | ProtocolManager（MQTT/Modbus/TCP/HTTP/CoAP） | Stage 4,8 |

**主循环**：`FastBeeFramework::run()` 在 Arduino `loop()` 中调用，负责：
- `healthMonitor->update()` — 内存保护、碎片检测
- `protocolManager->handle()` — 协议循环处理
- `network->update()` — 网络状态更新
- `taskManager->update()` — 定时任务执行
- `checkForRestart()` — 重启条件检查

---

## 4. 核心模块详解

### 4.1 FastBeeFramework (`core/`)

**单例模式**（Meyers' Singleton），协调所有子系统。

| 类 | 文件 | 职责 |
|----|------|------|
| `FastBeeFramework` | `core/FastBeeFramework.h/cpp` (1465行) | 核心框架单例，9 阶段初始化 + 主循环 |
| `PeripheralManager` | `core/PeripheralManager.h/cpp` (3466行) | 外设 CRUD、硬件初始化、GPIO 兼容层、Modbus 委托 |
| `PeriphExecManager` | `core/PeriphExecManager.h/cpp` (2352行) | 执行规则 CRUD、事件触发、MQTT/Modbus 回调 |
| `PeriphExecExecutor` | `core/PeriphExecExecutor.h/cpp` (1807行) | 规则执行引擎（GPIO/显示/延时/Modbus/MQTT 动作） |
| `PeriphExecScheduler` | `core/PeriphExecScheduler.h/cpp` (958行) | 定时器调度、Cron 表达式、事件匹配 |
| `PeriphExecWorkerPool` | `core/PeriphExecWorkerPool.h/cpp` (165行) | 异步执行工作线程池 |
| `CommandBus` | `core/CommandBus.h/cpp` (72行) | 命令总线，模块间解耦通信 |
| `ErrorHandler` | `core/ErrorHandler.h/cpp` (105行) | 统一错误处理 |
| `RuleScriptManager` | `core/RuleScriptManager.h/cpp` (286行) | 规则脚本引擎管理 |
| `ScriptEngine` | `core/ScriptEngine.h/cpp` (519行) | 脚本解释器 |

**外设类型体系**（`PeripheralTypes.h`，368行）：
- 通信接口：UART / I2C / SPI / CAN / USB (1-10)
- GPIO 接口：数字输入/输出/PWM/中断/触摸 (11-25)
- 模拟信号：ADC / DAC (26-30)
- 专用外设：LCD / 传感器 / NeoPixel / 数码管 / RFID / DS1302 / LCD1602 / Modbus 子设备 (36-60)
- 虚拟外设：DEVICE_EVENT (60) — 无物理引脚的事件发射源

### 4.2 网络层 (`network/`)

支持 **三模网络接入**，通过 `NetworkType` 枚举切换：

| 类 | 文件 | 职责 |
|----|------|------|
| `FBNetworkManager` | `network/NetworkManager.h/cpp` | 网络总管理器，协调 WiFi/以太网/4G 适配器 |
| `WiFiManager` | `network/WiFiManager.h/cpp` (756行) | WiFi STA/AP 双模管理，多 SSID 优先级列表 |
| `EthernetAdapter` | `network/EthernetAdapter.h/cpp` (289行) | W5500 以太网 SPI 适配器 |
| `CellularAdapter` | `network/CellularAdapter.h/cpp` (248行) | EC801E 4G 蜂窝网络适配器（UART） |
| `DNSManager` | `network/DNSManager.h/cpp` (339行) | DNS 服务器（AP 模式下 captive portal） |
| `IPManager` | `network/IPManager.h/cpp` (283行) | IP 冲突检测（ARP/ICMP）和故障转移策略 |
| `OTAManager` | `network/OTAManager.h/cpp` (409行) | OTA 固件和文件系统升级 |

**网络回退策略**：
1. 主网络（以太网/4G）连接失败 → 回退 WiFi STA
2. WiFi STA 多次失败 → 回退 WiFi AP 模式
3. AP 模式下 3 次连续失败 → `ensureLastResortAP()` 清除配置重启

**网络切换回调**：`PreNetworkSwitchCallback` 在销毁适配器前调用，用于停止 MQTT 等依赖网络适配器的协议。

### 4.3 协议层 (`protocols/`)

所有协议继承 `IProtocol` 抽象基类，提供统一的 `begin()/stop()/loop()/send()` 接口：

| 协议 | 类 | 文件 | 说明 |
|------|-----|------|------|
| MQTT | `MQTTClient` | `protocols/MQTTClient.h/cpp` (1944行) | PubSubClient 封装，TLS/非TLS，自动重连，主题模板 |
| Modbus | `ModbusHandler` | `protocols/ModbusHandler.h/cpp` (2472行) | RTU 主站/从站，轮询任务，寄存器映射，JSON 模式 |
| TCP | `TCPHandler` | `protocols/TCPHandler.h/cpp` (320行) | TCP 服务器，多客户端连接 |
| HTTP | `HTTPClientWrapper` | `protocols/HTTPClientWrapper.h/cpp` (271行) | HTTP 客户端封装 |
| CoAP | `CoAPHandler` | `protocols/CoAPHandler.h/cpp` (662行) | CoAP 协议处理 |

**ProtocolManager** (`protocols/ProtocolManager.h/cpp`, 1472行)：
- 统一管理所有协议的生命周期
- 消息回调路由到 PeriphExecManager
- MQTT 延迟重启（`restartMQTTDeferred()`）避免 Web handler 阻塞
- Modbus 数据分发：LiveCallback（实时）和 PeriphExecPoll（轮询）两条路径

### 4.4 Web 层

#### 4.4.1 WebConfigManager (`network/WebConfigManager.h/cpp`, 716行)

瘦协调器模式，持有 `WebHandlerContext` 共享上下文和 14 个专职路由处理器：

| 路由处理器 | 文件 | 职责 |
|-----------|------|------|
| `StaticRouteHandler` | `handlers/StaticRouteHandler.cpp` (63行) | 静态文件服务（gzip 资源） |
| `AuthRouteHandler` | `handlers/AuthRouteHandler.cpp` (149行) | 登录/登出/会话验证 |
| `SystemRouteHandler` | `handlers/SystemRouteHandler.cpp` (1618行) | 系统信息/网络配置/重启 |
| `DeviceRouteHandler` | `handlers/DeviceRouteHandler.cpp` (424行) | 设备配置（device.json） |
| `PeripheralRouteHandler` | `handlers/PeripheralRouteHandler.cpp` (1145行) | 外设 CRUD + 硬件初始化 |
| `PeriphExecRouteHandler` | `handlers/PeriphExecRouteHandler.cpp` (1114行) | 执行规则 CRUD + 触发 |
| `ProtocolRouteHandler` | `handlers/ProtocolRouteHandler.cpp` (944行) | 协议配置（MQTT/Modbus/TCP/HTTP/CoAP） |
| `BatchRouteHandler` | `handlers/BatchRouteHandler.cpp` (472行) | 批量操作/配置导入导出 |
| `ModbusRouteHandler` | `handlers/ModbusRouteHandler.cpp` (1857行) | Modbus 主站轮询/控制/寄存器映射 |
| `MqttRouteHandler` | `handlers/MqttRouteHandler.cpp` (965行) | MQTT 连接测试/状态 |
| `SSERouteHandler` | `handlers/SSERouteHandler.cpp` (292行) | Server-Sent Events 实时推送 |
| `OTARouteHandler` | `handlers/OTARouteHandler.cpp` (162行) | OTA 固件/文件系统上传 |
| `UserRouteHandler` | `handlers/UserRouteHandler.cpp` (346行) | 多用户管理 |
| `RuleScriptRouteHandler` | `handlers/RuleScriptRouteHandler.cpp` (222行) | 规则脚本引擎 |

#### 4.4.2 Web 前端架构 (`web-src/`)

原生 JavaScript 单页应用，无框架依赖，构建后压缩部署到 LittleFS：

```
web-src/
├── index.html              # 主入口（登录页 + SPA 壳）
├── setup.html              # AP 模式初始配置页
├── css/main.css            # 全局样式（~2140 行）
├── js/                     # 核心 JS 引擎
│   ├── main.js             # 应用入口
│   ├── state.js            # 全局状态管理（737行）
│   ├── state-session.js    # 会话状态
│   ├── state-sse.js        # SSE 实时推送状态
│   ├── state-theme.js      # 主题状态
│   ├── state-ui.js         # UI 状态
│   ├── request-governor.js # HTTP 请求治理（限流/重试/队列，1041行）
│   ├── module-loader.js    # 模块动态加载
│   ├── page-loader.js      # 页面片段加载
│   ├── notification.js     # 通知组件
│   └── utils.js            # 工具函数
├── i18n/                   # 国际化
│   ├── i18n-engine.js      # i18n 引擎
│   ├── i18n-zh-CN.js       # 中文语言包（1757行）
│   └── i18n-en.js          # 英文语言包（1748行）
├── modules/                # 页面模块
│   ├── runtime/            # 运行时模块
│   │   ├── dashboard.js            # 仪表盘
│   │   ├── device-config.js        # 设备配置
│   │   ├── network.js              # 网络配置（1280行）
│   │   ├── peripherals.js          # 外设配置
│   │   ├── periph-exec.js          # 外设执行规则
│   │   ├── periph-exec-form.js     # 执行规则表单（1161行）
│   │   ├── periph-exec-modbus.js   # Modbus 执行规则
│   │   ├── device-control.js       # 设备控制入口
│   │   ├── device-control/         # 设备控制子模块
│   │   └── protocol/               # 协议配置子模块
│   │       ├── mqtt-config.js              # MQTT 配置
│   │       ├── modbus-config.js            # Modbus 配置
│   │       ├── modbus-control.js           # Modbus 控制
│   │       ├── protocol-config.js          # 协议总配置
│   │       └── protocol-lite-config.js     # 协议精简配置
│   └── admin/              # 管理模块
│       ├── users.js                # 用户管理
│       ├── files.js                # 文件管理器
│       ├── logs.js                 # 日志查看
│       └── rule-script.js          # 规则脚本
└── pages/                  # HTML 页面模板
    ├── dashboard.html      # 仪表盘页面
    ├── device.html         # 设备配置页面
    ├── network.html        # 网络配置页面（616行）
    ├── peripheral.html     # 外设配置页面
    ├── protocol.html       # 协议配置页面
    └── fragments/          # 页面片段
        ├── protocol-mqtt.html
        ├── protocol-modbus-rtu.html
        ├── protocol-tcp.html
        ├── protocol-http.html
        ├── protocol-coap.html
        └── device-ota.html
```

**构建流程**：`scripts/build-web-assets.js` + `scripts/build-web-modules.js` → 压缩 → gzip → `data/www/` → LittleFS 镜像

### 4.5 安全模块 (`security/`)

| 类 | 文件 | 职责 |
|----|------|------|
| `AuthManager` | `security/AuthManager.h/cpp` (847行) | 会话管理、Cookie 认证、登录锁定、多会话控制 |
| `UserManager` | `security/UserManager.h/cpp` (654行) | 用户 CRUD、密码哈希、角色管理 |
| `CryptoUtils` | `security/CryptoUtils.h/cpp` (734行) | SHA-256、HMAC、随机令牌生成 |

**认证机制**：
- 基于 Cookie 的会话认证（`sessionId`，HttpOnly）
- 最多 6 个活跃会话，每用户最多 3 个
- 5 次失败登录锁定 5 分钟
- 会话超时 1 小时

### 4.6 系统服务 (`systems/`)

| 类 | 文件 | 职责 |
|----|------|------|
| `ConfigStorage` | `systems/ConfigStorage.h/cpp` (520行) | NVS 键值 + LittleFS JSON 双后端存储 |
| `HealthMonitor` | `systems/HealthMonitor.h/cpp` (535行) | 四级内存保护 + DRAM 专项监控 + 碎片紧凑化 |
| `LoggerSystem` | `systems/LoggerSystem.h/cpp` (594行) | 分级日志（DEBUG/INFO/WARN/ERROR）、文件日志 |
| `TaskManager` | `systems/TaskManager.h/cpp` (363行) | 定时任务调度（最多 20 个任务） |
| `RestartDiagnostics` | `systems/RestartDiagnostics.h/cpp` (282行) | RTC_NOINIT 内存保存重启前状态快照 |

**HealthMonitor 四级内存保护**：

| 等级 | 条件 | 措施 |
|------|------|------|
| NORMAL | freeHeap ≥ 20KB 或 largestBlock ≥ 12KB | 所有功能正常 |
| WARN | 10KB ≤ freeHeap < 20KB | 降低轮询频率、降低日志级别 |
| SEVERE | 6KB ≤ freeHeap < 10KB | 暂停 Modbus 轮询、MQTT 降采样、停止日志文件 |
| CRITICAL | freeHeap < 6KB | 禁用文件日志、拒绝大响应、仅保留关键页面 |

**DRAM 专项保护**：`checkCriticalMemory()` 检测 DRAM 内部空闲（`MALLOC_CAP_INTERNAL`），连续 3 次（15秒）低于 8KB 触发 `ESP.restart()`，重启前调用 `RestartDiagnostics::savePreRestartState()`。

### 4.7 外设驱动 (`peripherals/`)

| 驱动 | 文件 | 接口 | 说明 |
|------|------|------|------|
| `LCDManager` | `peripherals/LCDManager.h/cpp` (703行) | I2C (U8g2) | OLED/LCD 显示，多页面轮播 |
| `SensorDriver` | `peripherals/SensorDriver.h/cpp` (429行) | GPIO/OneWire | DHT11/DHT22/DS18B20 |
| `SevenSegmentDriver` | `peripherals/SevenSegmentDriver.h/cpp` (274行) | GPIO (bit-bang) | TM1637 4 位数码管 |
| `BMP280Driver` | `peripherals/drivers/BMP280Driver.cpp` (149行) | I2C | 气压/温度传感器 |
| `MPU6050Driver` | `peripherals/drivers/MPU6050Driver.cpp` (175行) | I2C | 6 轴加速度/陀螺仪 |
| `SHT31Driver` | `peripherals/drivers/SHT31Driver.cpp` (108行) | I2C | 温湿度传感器 |
| `BH1750Driver` | `peripherals/drivers/BH1750Driver.cpp` (90行) | I2C | 光照强度传感器 |
| `AHT20Driver` | `peripherals/drivers/AHT20Driver.cpp` (108行) | I2C | 温湿度传感器 |
| `RFIDDriver` | `peripherals/drivers/RFIDDriver.cpp` (115行) | SPI | MFRC522 RFID 模块 |
| `IRRemoteDriver` | `peripherals/drivers/IRRemoteDriver.cpp` (108行) | GPIO | 红外遥控收发 |
| `DS1302Driver` | `peripherals/drivers/DS1302Driver.cpp` (419行) | GPIO (3线) | 实时时钟模块 |
| `LCD1602Driver` | `peripherals/drivers/LCD1602Driver.cpp` (387行) | I2C (PCF8574) | 字符液晶 |

---

## 5. 构建系统

### 5.1 预定义环境

| PlatformIO 环境 | 芯片 | Flash | PSRAM | 功能级别 | 分区表 |
|----------------|------|-------|-------|---------|--------|
| `esp32c3-F4R0` | ESP32-C3 | 4MB | 无 | Lite | fastbee.csv |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | 无 | Lite | fastbee.csv |
| `esp32-F4R0` | ESP32 | 4MB | 无 | Standard | fastbee.csv |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | 无 | Standard+OTA | fastbee-8MB.csv |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | Full | fastbee-8MB.csv |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | Full | fastbee-8MB.csv |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | Full | fastbee-16MB.csv |

### 5.2 功能编译开关

通过 `platformio.ini` 的 `build_flags` 和 `include/core/FeatureFlags.h` 控制编译裁剪。三级功能预设：

**三级功能预设对比**：

| 分类 | 功能 | Lite | Standard | Full |
|------|------|:----:|:--------:|:----:|
| **网络** | WiFi + MQTT + mDNS | ✅ | ✅ | ✅ |
| | 以太网 + 4G | ❌ | ✅ | ✅ |
| | BLE | ❌ | ❌ | ✅ |
| **协议** | MQTT | ✅ | ✅ | ✅ |
| | Modbus RTU | ❌ | ✅ | ✅ |
| | Modbus 从站 + TCP + HTTP + CoAP | ❌ | ❌ | ✅ |
| **外设** | GPIO + DHT + DS18B20 + LCD + 数码管 | ✅ | ✅ | ✅ |
| | NeoPixel + LED Screen | ✅ | ✅ | ✅ |
| | I2C 传感器 + RFID + 红外 | ❌ | ✅ | ✅ |
| | Command Script | ❌ | ✅ | ✅ |
| | Rule Script | ❌ | ❌ | ✅ |
| **Web** | 仪表盘 + 配置 + SSE + 导入导出 | ✅ | ✅ | ✅ |
| | 多用户 + 文件管理 + 日志查看 + i18n | ❌ | ❌ | ✅ |
| **系统** | 健康监控 + 任务管理 + DNS | ✅ | ✅ | ✅ |
| | OTA 升级 | ❌ | ⚠️ | ✅ |
| | 文件日志 | ❌ | ❌ | ✅ |

**主要功能开关完整列表**（详见 `include/core/FeatureFlags.h`）：

外设/显示类（重点关注 RAM 占用）：

| 开关 | 默认 | RAM 影响 | Flash 影响 |
|------|------|---------|-----------|
| `FASTBEE_ENABLE_LCD` | 0 | ~1KB | ~20KB |
| `FASTBEE_ENABLE_NEOPIXEL` | 1 | ~3B×灯珠+2KB | ~12KB |
| `FASTBEE_ENABLE_LED_SCREEN` | 0 | 动态 | ~10KB |
| `FASTBEE_ENABLE_I2C_SENSORS` | 0 | ~500B | ~15KB |
| `FASTBEE_ENABLE_RFID` | 0 | ~200B | ~12KB |
| `FASTBEE_ENABLE_BLE` | 0 | — | ~80KB |
| `FASTBEE_ENABLE_SENSOR_DRIVER` | 1 | ~200B | ~8KB |

### 5.3 分区表

| 分区文件 | Flash | App 槽位 | OTA | LittleFS |
|---------|-------|---------|-----|----------|
| `fastbee.csv` | 4MB | 2.88MB × 1 | 否 | 1MB |
| `fastbee-8MB.csv` | 8MB | 3.5MB × 2 | 是 | 960KB |
| `fastbee-16MB.csv` | 16MB | 4MB × 2 | 是 | 7.9MB |

### 5.4 源码过滤

`build_src_filter` 在 Lite/Standard 环境下排除不需要的 .cpp 文件，进一步减少 Flash 占用：

- **Lite** 排除：RuleScriptManager、OTAManager、UserRouteHandler、LogRouteHandler、TCP/HTTP/CoAP
- **Standard** 排除：UserRouteHandler、LogRouteHandler、TCP/HTTP/CoAP
- **Full** 包含所有源码

### 5.5 运行时参数

| 芯片 | TCP 最大连接 | TCP 队列 | Loop 栈 | 脚本栈 | 简单任务栈 |
|------|-------------|---------|---------|--------|-----------|
| ESP32 | 6 | 8 | 16KB | 8KB | 6KB |
| ESP32-S3 | 14 | 24 | 16KB | 8KB | 6KB |
| ESP32-C3 | 4 | 4 | 12KB | 6KB | 4KB |
| ESP32-C6 | 6 | 8 | 12KB | 6KB | 4KB |

---

## 6. 内存管理策略

### 6.1 ESP32 内存架构

ESP32 内部 DRAM 仅约 320KB，WiFi/lwIP/FreeRTOS/SSL 必须驻留内部 DRAM。PSRAM 延迟比 DRAM 高约 3 倍，但对 HTTP/JSON 响应构建无感知影响。

**PSRAM 分配策略**（`main.cpp`）：
```cpp
heap_caps_malloc_extmem_enable(512);  // ≥ 512B 的分配请求优先用 PSRAM
```
阈值设为 512 而非 4096 的原因：AsyncWebServer 每个 HTTP 请求缓冲区约 1-2KB，4096 阈值太高无法卸载到 PSRAM。

### 6.2 堆碎片治理

项目使用 `StaticPoolAllocator`（`utils/StaticPoolAllocator.h`）为 `std::map` 节点提供预分配池，根治红黑树节点的堆碎片问题：
- BlockSize 256B，BlockCount 32，冷启动预分配约 8KB DRAM
- 池耗尽优雅回退 `::malloc`

`HeapFragmentation`（`utils/HeapFragmentation.h`）提供碎片率计算，碎片率超过 80% 触发紧凑化。

### 6.3 重启诊断

`RestartDiagnostics` 在 `RTC_NOINIT` 内存保存 `PreRestartSnapshot`（约 120B），包含：
- 内存状态（freeHeap、minFreeHeap、largestFreeBlock、碎片率）
- MemGuard 等级和连续低内存计数
- 关键任务栈水位（loopTask、asyncTcp、mqttReconn）
- WiFi/MQTT/SSE 状态
- 自定义重启原因码（10 种：低内存/用户命令/OTA/异常/看门狗/栈溢出等）

启动时 `logBootDiagnostics()` 输出上次重启的完整诊断信息。

---

## 7. 数据存储

### 7.1 配置文件

所有配置存储在 LittleFS 文件系统（`/config/` 目录）：

| 文件 | 路径 | 说明 |
|------|------|------|
| `device.json` | `/config/device.json` | 设备信息（ID、名称、NTP、日志级别） |
| `network.json` | `/config/network.json` | 网络配置（WiFi SSID/密码、以太网、4G） |
| `protocol.json` | `/config/protocol.json` | 统一协议配置（MQTT/Modbus/TCP/HTTP/CoAP） |
| `peripherals.json` | `/config/peripherals.json` | 外设配置列表 |
| `exec_rules.json` | `/config/exec_rules.json` | 执行规则列表 |
| `users.json` | `/config/users.json` | 用户账户信息 |
| `auth.json` | `/config/auth.json` | 认证安全配置 |

### 7.2 双后端存储

`ConfigStorage` 提供两种存储后端：
- **NVS (Preferences)**：频繁读写的小型键值对（启动计数、固件版本）
- **LittleFS JSON**：结构化配置对象，支持分段加载（`loadProtocolSection()` 使用 ArduinoJson Filter 只反序列化需要的顶层节点）

---

## 8. API 端点

| 端点 | 方法 | 处理器 | 说明 |
|------|------|--------|------|
| `/api/login` | POST | AuthRouteHandler | 用户登录 |
| `/api/logout` | POST | AuthRouteHandler | 用户登出 |
| `/api/system/*` | GET/POST | SystemRouteHandler | 系统信息、网络配置、重启 |
| `/api/device/*` | GET/POST | DeviceRouteHandler | 设备配置 |
| `/api/network/*` | GET/POST | SystemRouteHandler | 网络状态、扫描 WiFi |
| `/api/peripheral/*` | GET/POST/DELETE | PeripheralRouteHandler | 外设 CRUD |
| `/api/periph-exec/*` | GET/POST/DELETE | PeriphExecRouteHandler | 执行规则 CRUD |
| `/api/protocol/*` | GET/POST | ProtocolRouteHandler | 协议配置 |
| `/api/mqtt/*` | GET/POST | MqttRouteHandler | MQTT 测试连接 |
| `/api/modbus/*` | GET/POST | ModbusRouteHandler | Modbus 控制 |
| `/api/batch/*` | POST | BatchRouteHandler | 批量操作、配置导入导出 |
| `/api/ota/*` | POST | OTARouteHandler | OTA 升级 |
| `/api/users/*` | GET/POST/DELETE | UserRouteHandler | 用户管理 |
| `/api/sse` | GET | SSERouteHandler | Server-Sent Events |
| `/events` | GET | SSERouteHandler | SSE 事件流 |

---

## 9. 测试体系

### 9.1 测试框架

使用 PlatformIO native 环境 + Unity 测试框架，37 个测试文件：

| 测试类别 | 文件 | 测试数量 |
|---------|------|---------|
| **核心** | `test_main.cpp` | 统一入口 |
| **外设配置** | `test_periph_config.cpp` (1827行) | 外设 CRUD、类型验证 |
| **外设执行** | `test_periph_exec.cpp` (1868行) | 执行规则、触发器、动作 |
| **MQTT 协议** | `test_mqtt_protocol.cpp` (3066行) | MQTT 连接、主题、消息 |
| **网络配置** | `test_network_config.cpp` (2181行) | WiFi/以太网/4G 配置 |
| **网络-MQTT 集成** | `test_network_mqtt_integration.cpp` (460行) | 网络切换 + MQTT 重连 |
| **E2E 场景** | `test_e2e_scenarios.cpp` (2116行) | 端到端业务场景 |
| **回归保护** | `test_regression_guard.cpp` (1998行) | 回归测试守护 |
| **健康监控** | `test_health_monitor.cpp` (802行) | 内存保护等级 |
| **系统稳定性** | `test_system_stability.cpp` (1520行) | 长时间运行稳定性 |
| **Web API** | `test_web_api.cpp` (746行) | REST API 接口 |
| **Modbus** | `test_modbus_handler.cpp` (622行) | Modbus 主站/从站 |
| **其他** | 其余 25 个测试文件 | 各模块单元测试 |

### 9.2 测试运行

```powershell
# 本地 native 测试（不访问真实设备）
pio test -e native

# 完整检查矩阵
.\scripts\test-all.ps1 -Checks static,native,build,artifacts

# 真实设备冒烟测试
.\scripts\smoke-test-device.ps1 -BaseUrl http://192.168.4.1 -Profile standard

# 长时间稳定性测试
.\scripts\soak-test-device.ps1 -BaseUrl http://设备IP -Profile full -Rounds 100
```

---

## 10. 构建与部署

### 10.1 构建脚本

| 脚本 | 说明 |
|------|------|
| `deploy.ps1` | 编译 + 上传 LittleFS + 烧录固件 |
| `build-all-artifacts.ps1` | 一次生成所有版本合并固件 |
| `doctor.ps1` | 环境诊断检查 |
| `test-all.ps1` | 完整测试矩阵 |
| `smoke-test-device.ps1` | 真实设备 API 冒烟测试 |
| `soak-test-device.ps1` | 长时间稳定性测试 |

### 10.2 Web 前端构建

```
web-src/ → scripts/build-web-assets.js → 压缩 JS/CSS
         → scripts/build-web-modules.js → 模块化打包
         → scripts/gzip-www.js → gzip 压缩
         → data/www/ → LittleFS 镜像
```

### 10.3 验证脚本

| 脚本 | 说明 |
|------|------|
| `validate-build-matrix.js` | 全芯片编译验证 |
| `validate-config-defaults.js` | 配置默认值校验 |
| `validate-i18n.js` | 国际化完整性检查 |
| `validate-doc-links.js` | 文档链接有效性 |
| `validate-test-coverage.js` | 测试覆盖率检查 |
| `web-smoke-test.js` | Web 静态资源冒烟检查 |

---

## 11. 关键设计模式

### 11.1 条件编译

所有功能模块通过 `#if FASTBEE_ENABLE_*` 宏控制编译，未启用的模块不产生任何代码：
```cpp
#if FASTBEE_ENABLE_MQTT
    MQTTClient* getMQTTClient() const { return mqttClient.get(); }
#else
    MQTTClient* getMQTTClient() const { return nullptr; }
#endif
```

### 11.2 单例模式

核心管理器使用 Meyers' Singleton（局部静态变量），C++11 线程安全：
- `FastBeeFramework::getInstance()`
- `ConfigStorage::getInstance()`
- `PeripheralManager::getInstance()`
- `PeriphExecManager::getInstance()`

### 11.3 回调解耦

模块间通过 `std::function` 回调解耦，避免循环依赖：
- `PreNetworkSwitchCallback` — 网络切换前通知协议层
- `MqttIsConnectedCallback` — PeriphExec 查询 MQTT 状态
- `ModbusCoilWriteFunc` — PeripheralManager 调用 Modbus 写线圈
- `MessageCallback` — ProtocolManager 分发协议消息

### 11.4 接口抽象

`include/core/interfaces/` 定义抽象接口，实现层与消费层解耦：
- `IAuthManager` — 认证接口
- `INetworkManager` — 网络管理接口
- `IConfigStorage` — 配置存储接口
- `IUserManager` — 用户管理接口
- `ILoggerSystem` — 日志系统接口
- `ISensorDriver` — 传感器驱动接口
- `ITaskManager` — 任务管理接口
- `IProtocol` — 协议接口

---

## 12. 依赖库

| 库 | 版本 | 用途 |
|----|------|------|
| ArduinoJson | 7.4.2 | JSON 序列化/反序列化 |
| PubSubClient | ^2.8 | MQTT 客户端 |
| ESPAsyncWebServer | ^3.6.0 | 异步 Web 服务器 |
| AsyncTCP | ^3.3.2 | 异步 TCP |
| NimBLE-Arduino | ^2.2.0 | BLE 蓝牙（Full 版） |
| U8g2 | ^2.35.9 | OLED/LCD 显示驱动 |
| DHT sensor library | ^1.4.6 | DHT 温湿度传感器 |
| OneWire | ^2.3.8 | 单总线协议 |
| DallasTemperature | ^3.9.0 | DS18B20 温度传感器 |
| Adafruit NeoPixel | ^1.12.0 | WS2812 LED 驱动 |
| TinyGSM | ^0.11.7 | 4G 蜂窝网络（Standard/Full） |
| BMP280/MPU6050/MFRC522/IRremote | 各版本 | I2C 传感器、RFID、红外 |

---

## 13. 配置文件结构

### device.json
```json
{
  "deviceId": "FB-001",
  "productName": "FastBee-Device",
  "ntpServer": "pool.ntp.org",
  "ntpOffset": 8,
  "logLevel": 2
}
```

### network.json
```json
{
  "networkType": 0,
  "wifi": {
    "mode": 0,
    "staSsid": "MyWiFi",
    "staPassword": "password",
    "apSsid": "FastBee-XXXX",
    "apPassword": "fastbee123"
  },
  "ethernet": { "spiMosi": 11, "spiMiso": 13, "spiSck": 12, "spiCs": 15, "rst": 14 },
  "cellular": { "apn": "cmnet", "pwrPin": 38, "txPin": 39, "rxPin": 40 }
}
```

### protocol.json
```json
{
  "mqtt": { "enabled": true, "server": "iot.fastbee.cn", "port": 1883, ... },
  "modbusRtu": { "enabled": false, "baudRate": 9600, ... },
  "tcp": { "enabled": false, "port": 8080, ... },
  "http": { "enabled": false, ... },
  "coap": { "enabled": false, "port": 5683, ... }
}
```
