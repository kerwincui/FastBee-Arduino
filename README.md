
**FastBee-Arduino是一个基于ESP32-Arduino平台构建的、功能完整的嵌入式物联网设备开发框架。**

---

### 项目介绍
FastBee-Arduino致力于成为ESP32平台上最全面、最易用的嵌入式物联网开发框架，通过模块化设计和企业级功能，帮助企业快速制作产品原型，降低开发成本；同时也是个人开发者快速学习和使用的工具；为开发者提供了一站式的解决方案，涵盖了从设备管理、网络通信到远程维护等物联网设备开发的全链路需求。

---

### 开发进度

持续开发中...

████████████████████████████████████████████████░░░░░░░░░░░░░░  **70%**


---

### 功能模块
* **Web配置管理：** 完整的用户管理、角色权限管理、会话管理，响应式单页面应用设计，原生Web技术实现，支持中英文切换
* **网络通信：** WiFi STA/AP/AP+STA三种模式、自动回退机制、mDNS本地域名访问、网络状态监测、WiFi扫描与连接
* **配网功能：** AP热点配网、蓝牙BLE配网（NimBLE）、配网超时自动关闭
* **协议支持：** MQTT协议接入、Modbus RTU/TCP、TCP服务器/客户端、HTTP客户端
* **远程维护：** 固件OTA升级、文件系统OTA升级、在线日志查看、远程重启、恢复出厂设置
* **数据存储：** LittleFS文件系统、NVS Preferences双存储、JSON配置文件管理、Gzip压缩优化
* **外设管理：** RS485串口配置、数字输入/输出、模拟输入、PWM输出、I2C/SPI接口、可视化配置
* **GPIO管理：** 可视化GPIO引脚配置、状态监控、PWM控制
* **健康监测：** CPU/内存/存储实时监测、网络连接状态、任务运行状态、异常告警
* **日志系统：** 分级日志（DEBUG/INFO/WARN/ERROR）、文件存储、Web在线查看、日志轮转
* **任务调度：** 定时任务管理、异步事件处理、优先级调度
* **安全认证：** 用户登录认证、基于角色的权限控制(RBAC)、会话管理、记住登录

---

### 硬件产品
![设备图](./images/device.png)

* 芯片：ESP32-WROOM-32U
* Flash：4MB SPI Flash
* 无线：WiFi 802.11 b/g/n + Bluetooth 4.2 + BLE
* 供电电压：DC 9-36V，带外置天线，USB烧录口和配置按键
* **接线端子说明：**
  * A/L：RS485-A（TX），GPIO17
  * B/H：RS485-B（RX），GPIO18
  * VCC：供电正极，DC 9-36V
  * GND：供电负极
  * DGND：数字地（隔离GND）
  * EGND：保护地（连接设备外壳）
  * IO/L：隔离型数字输入/输出低端
  * IO/H：隔离型数字输入/输出高端
* **指示灯说明：**
  * POWER：电源指示灯（常亮表示供电正常）
  * STATE：状态指示灯，GPIO5（低电平点亮）
  * DATA：通讯指示灯（数据收发闪烁）
* **按键：** GPIO0（长按进入配置模式）

---

### 开发环境
* **开发工具：** VSCode + PlatformIO
* **目标芯片：** ESP32（ESP32-WROOM-32/32U/32D）
* **Flash要求：** 4MB及以上
* **框架版本：** Arduino-ESP32 2.0.x
* **依赖库：**
  * ArduinoJson @ 7.4.2 - JSON解析
  * WebSockets @ 2.3.6 - WebSocket通信
  * PubSubClient @ 2.8 - MQTT客户端
  * NimBLE-Arduino @ 1.4.1 - 蓝牙BLE配网
  * ESPAsyncWebServer @ 3.9.2 - 异步Web服务器（lib目录）

---

### Flash分区布局
| 分区名 | 类型 | 偏移地址 | 大小 | 说明 |
|--------|------|----------|------|------|
| nvs | data | 0x9000 | 20KB | NVS键值存储 |
| otadata | data | 0xe000 | 8KB | OTA数据 |
| app0 | app | 0x10000 | 2.25MB | 应用程序 |
| spiffs | data | 0x260000 | 1.625MB | LittleFS文件系统 |

---

### 使用流程
1. **环境准备**：安装VSCode和PlatformIO插件
2. **克隆项目**：`git clone https://gitee.com/beecue/fastbee-arduino.git`
3. **压缩前端资源**：`node scripts/gzip-www.js`
4. **上传文件系统**：PlatformIO → Upload Filesystem Image
5. **编译上传固件**：PlatformIO → Upload
6. **访问设备**：
   - 首次启动自动进入AP配网模式，连接热点 `fastbee-ap`
   - 或通过mDNS访问：http://fastbee.local
   - 默认账号：`admin` / `admin123`
7. **网络配置**：在Web界面配置WiFi连接信息
8. **外设配置**：配置RS485、GPIO等硬件接口
9. **协议对接**：配置MQTT/Modbus等协议连接物联网平台

---

### 系统架构
```
┌─────────────────────────────────────────────────┐
│              应用层 (Application)                │
├─────────────────────────────────────────────────┤
│  Web管理界面 │ 设备监控 │ 定时任务 │ 配置管理    │
├─────────────────────────────────────────────────┤
│              服务层 (Services)                   │
│  OTA升级 │ 网络管理 │ 日志服务 │ 健康监测 │任务调度│
├─────────────────────────────────────────────────┤
│              协议层 (Protocols)                  │
│    MQTT    │  Modbus  │   TCP   │    HTTP       │
├─────────────────────────────────────────────────┤
│              安全层 (Security)                   │
│  用户管理  │  角色权限  │  会话管理  │  加密工具  │
├─────────────────────────────────────────────────┤
│              存储层 (Storage)                    │
│    NVS Preferences    │    LittleFS (JSON)      │
├─────────────────────────────────────────────────┤
│              硬件层 (Hardware)                   │
│  GPIO │ RS485 │ I2C │ SPI │ PWM │ ADC │ WiFi/BLE│
├─────────────────────────────────────────────────┤
│              ESP32 微控制器 (240MHz双核)         │
└─────────────────────────────────────────────────┘
```

---

### 项目结构
```
FastBee-Arduino/
├── include/                       # 头文件目录
│   ├── core/                      # 核心框架
│   │   ├── FastBeeFramework.h     # 主框架类
│   │   ├── ConfigDefines.h        # 配置常量
│   │   ├── SystemConstants.h      # 系统常量
│   │   ├── GPIOManager.h          # GPIO管理
│   │   ├── PeripheralManager.h    # 外设管理
│   │   └── FeatureFlags.h         # 功能开关
│   ├── network/                   # 网络模块
│   │   ├── NetworkManager.h       # 网络连接管理
│   │   ├── WiFiManager.h          # WiFi管理
│   │   ├── WebConfigManager.h     # Web配置服务
│   │   ├── OTAManager.h           # OTA升级
│   │   └── DNSServer.h            # DNS服务
│   ├── protocols/                 # 协议模块
│   │   ├── ProtocolManager.h      # 协议管理器
│   │   ├── MQTTClient.h           # MQTT客户端
│   │   ├── ModbusHandler.h        # Modbus处理
│   │   ├── TCPHandler.h           # TCP处理
│   │   └── HTTPClientWrapper.h    # HTTP客户端
│   ├── security/                  # 安全模块
│   │   ├── UserManager.h          # 用户管理
│   │   ├── RoleManager.h          # 角色管理
│   │   ├── AuthManager.h          # 认证授权
│   │   └── CryptoUtils.h          # 加密工具
│   ├── systems/                   # 系统服务
│   │   ├── LoggerSystem.h         # 日志系统
│   │   ├── TaskManager.h          # 任务调度
│   │   ├── HealthMonitor.h        # 健康监测
│   │   └── ConfigStorage.h        # 配置存储
│   └── utils/                     # 工具类
│       ├── StringUtils.h
│       ├── TimeUtils.h
│       ├── FileUtils.h
│       └── JsonConverters.h
├── src/                           # 源代码目录
│   ├── core/                      # 核心实现
│   ├── network/                   # 网络实现
│   ├── protocols/                 # 协议实现
│   ├── security/                  # 安全实现
│   ├── systems/                   # 系统实现
│   ├── utils/                     # 工具实现
│   └── main.cpp                   # 主入口
├── data/                          # 文件系统
│   ├── www/                       # Web前端资源
│   │   ├── index.html             # 单页面应用
│   │   ├── css/                   # 样式文件
│   │   ├── js/                    # JavaScript
│   │   └── assets/                # 静态资源
│   ├── config/                    # 配置文件
│   │   ├── device.json            # 设备配置
│   │   ├── network.json           # 网络配置
│   │   ├── protocol.json          # 协议配置
│   │   ├── peripherals.json       # 外设配置
│   │   ├── gpio.json              # GPIO配置
│   │   └── users.json             # 用户配置
│   └── logs/                      # 日志目录
│       └── system.log
├── lib/                           # 本地库
│   └── ESPAsyncWebServer/         # 异步Web服务器
├── scripts/                       # 构建脚本
│   ├── gzip-www.js                # 前端压缩
│   └── filter_littlefs.py         # 文件过滤
├── platformio.ini                 # PlatformIO配置
├── fastbee.csv                    # 分区表
└── README.md                      # 项目说明
```

---

### 部分截图

<img src="./images/device_01.png"/>

<img src="./images/device_02.png"/>

<img src="./images/device_03.png"/>

<img src="./images/device_04.png"/>

---

### 许可证
本项目采用 MIT 许可证，详见 [LICENSE](./LICENSE) 文件。

---

