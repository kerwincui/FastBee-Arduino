[![gitee投票](https://gitee.com/beecue/fastbee/raw/master/images/banner1.png)](https://gitee.com/activity/2025opensource?ident=IKKZS9)

### 环境
* 开发工具: VSCode+platformIO
* 芯片: ESP32
* flash: 4MB以上
* 依赖库:ESP32-Arduino, bblanchon/ArduinoJson@^6.21.3, links2004/WebSockets@^2.3.6, knolleary/PubSubClient@^2.8, eModbus1.7.4

### 设备图
![设备图](./device.png)

* 芯片使用ESP32-WROOM-32U
* 4MB spi flash
* Wifi + Bluetooth + Bluetooth LE
* 供电电压 9-36V，带天线，use烧录口和按键
* 接线端子、指示灯和按键说明
  * A/L：RS485-TX，GPIO17
  * B/H：RS485-RX，GPIO18
  * VCC:供电引脚，直流9-36V
  * GND：供电引脚，直流9-36V
  * DGND：接地引脚，隔离GND
  * EGND：连接大地，确保设备外壳与地点位一致
  * IO/L：隔离型输入输出
  * IO/H：隔离型输入输出
  * POWER：电源指示灯
  * STATE：状态指示灯，GPIO5（低电平点亮）
  * DATA：通讯指示灯
  * 按键：GPIO0

### 使用流程
1. 烧录文件系统
2. 上传文件系统
3. 编译固件，并上传
4. 输入域名my.fastbee.cn或硬件串口打印IP地址（默认192.168.1.5）打开页面进行配置,默认账号admin admin


### 配置存储 Preferences + SPIFFS JSON
1. Preferences: 快速读写常用配置，确保快速访问和安全（经常修改并立即生效的配置）
2. JSON文件: 集中管理大量配置，易于备份和维护
3. 分层存储: 热配置放Preferences，全量配置放JSON

### 循环依赖处理
* 使用前向声明和指针
* 基础模块使用单例模式，工具类使用静态类


### 系统架构
```
┌─────────────────────────────────────────┐  
│             应用层 (Application)         │  
├─────────────────────────────────────────┤  
│    Web界面  │ 设备监控 │ 定时任务 │ 配置  │  
├─────────────────────────────────────────┤  
│             服务层 (Services)            │  
│  OTA服务 │ 网络服务 │ 日志服务 │ 健康检查 │  
├─────────────────────────────────────────┤  
│             协议层 (Protocols)           │  
│   MQTT   │  Modbus │  TCP   │ HTTP │ CoAP │  
├─────────────────────────────────────────┤  
│             存储层 (Storage)             │  
│ Preferences │ SPIFFS/LittleFS (JSON配置) │  
├─────────────────────────────────────────┤  
│             硬件层 (Hardware)            │  
│              ESP32 微控制器              │  
└─────────────────────────────────────────┘
```  

### 系统结构
```
FastBee_IoT_Platform  
├── include/                       # 头文件目录  
│   ├── Core/                      # 核心框架头文件(系统初始化和生命周期管理,全局配置管理)  
│   │   ├── FastBeeFramework.h     # 主框架类，协调所有子系统  
│   │   ├── ConfigDefines.h        # 配置常量定义  
│   │   ├── SystemConstants.h      # 系统常量定义  
│   │   └── GpioManager.h          # GPIO引脚管理  
│   ├── Systems/                   # 子系统头文件  
│   │   ├── LoggerSystem.h         # 分级日志系统  
│   │   ├── TaskManager.h          # 多任务调度管理  
│   │   ├── HealthMonitor.h        # 系统健康检查  
│   │   ├── GpioConfig.h           # GPIO引脚定义 
│   │   └── ConfigStorage.h        # 配置存储管理  
│   ├── Network/                   # 网络相关头文件  
│   │   ├── NetworkManager.h       # 网络连接管理  
│   │   ├── WebConfigManager.h     # Web配置后台(Web服务器、REST API、认证管理)  
│   │   ├── OTAManager.h           # OTA升级管理  
│   │   └── DNSServer.h            # DNS服务器（自定义域名）  
│   ├── Protocols/                 # 协议栈头文件  
│   │   ├── ProtocolManager.h      # 协议管理器(协议统一管理、数据路由)  
│   │   ├── MQTTClient.h           # MQTT客户端  
│   │   ├── ModbusHandler.h        # Modbus协议处理  
│   │   ├── TCPHandler.h           # TCP服务器/客户端  
│   │   ├── HTTPClient.h           # HTTP客户端  
│   │   └── CoAPHandler.h          # CoAP协议处理  
│   ├── Security/                  # 安全相关头文件  
│   │   ├── UserManager.h          # 用户管理  
│   │   ├── AuthManager.h          # 认证授权管理  
│   │   └── CryptoUtils.h          # 加密工具  
│   └── Utils/                     # 工具类头文件  
│       ├── StringUtils.h  
│       ├── TimeUtils.h  
│       └── FileUtils.h  
├── src/                           # 源代码目录  
│   ├── Core/                      # 核心框架实现  
│   │   ├── FastBeeFramework.cpp  
│   │   ├── GpioManager.cpp  
│   │   └── SystemInitializer.cpp  
│   ├── Systems/                   # 子系统实现  
│   │   ├── LoggerSystem.cpp  
│   │   ├── TaskManager.cpp  
│   │   ├── HealthMonitor.cpp  
│   │   ├── GpioConfig.cpp           
│   │   └── ConfigStorage.cpp  
│   ├── Network/                   # 网络相关实现  
│   │   ├── NetworkManager.cpp  
│   │   ├── WebConfigManager.cpp  
│   │   ├── OTAManager.cpp  
│   │   └── DNSServer.cpp  
│   ├── Protocols/                 # 协议栈实现  
│   │   ├── ProtocolManager.cpp  
│   │   ├── MQTTClient.cpp  
│   │   ├── ModbusHandler.cpp  
│   │   ├── TCPHandler.cpp  
│   │   ├── HTTPClient.cpp  
│   │   └── CoAPHandler.cpp  
│   ├── Security/                  # 安全相关实现  
│   │   ├── UserManager.cpp  
│   │   ├── AuthManager.cpp  
│   │   └── CryptoUtils.cpp  
│   └── Utils/                     # 工具类实现  
│       ├── StringUtils.cpp  
│       ├── TimeUtils.cpp  
│       └── FileUtils.cpp  
├── data/                          # 文件系统数据（SPIFFS/LittleFS）  
│   ├── www/                       # 日志文件目录  
│   │   ├── index.html             # 主页面/仪表盘  
│   │   ├── login.html             # 登录页面  
│   │   ├── dashboard.html         # 设备监控仪表盘  
│   │   ├── system.html            # 系统配置页面  
│   │   ├── network.html           # 网络配置页面  
│   │   ├── users.html             # 用户管理页面  
│   │   ├── protocols.html         # 协议配置页面  
│   │   ├── ota.html               # OTA升级页面  
│   │   ├── monitor.html           # 设备监测页面  
│   │   ├── css/  
│   │   │   ├── main.css           # 主样式文件  
│   │   │   ├── dashboard.css      # 仪表盘样式  
│   │   │   ├── forms.css          # 表单样式  
│   │   │   └── responsive.css     # 响应式样式  
│   │   ├── js/   
│   │   │   ├── main.js            # 主JavaScript文件  
│   │   │   ├── auth.js            # 认证相关  
│   │   │   ├── dashboard.js       # 仪表盘功能  
│   │   │   ├── config.js          # 配置页面功能  
│   │   │   ├── network.js         # 网络配置功能  
│   │   │   ├── users.js           # 用户管理功能  
│   │   │   ├── protocols.js       # 协议配置功能  
│   │   │   ├── ota.js             # OTA升级功能  
│   │   │   ├── gpio.js            # GPIO配置  
│   │   │   └── utils.js           # 工具函数  
│   │   └── assets/   
│   │       ├── favicon.ico        # 网站图标  
│   │       └── logo.png           # 平台Logo  
│   ├── config/                    # 配置文件目录  
│   │   ├── system.json  
│   │   ├── system.json  
│   │   ├── network.json  
│   │   ├── users.json  
│   │   ├── mqtt.json  
│   │   ├── modbus.json  
│   │   ├── tcp.json  
│   │   ├── http.json  
│   │   └── coap.json  
│   ├── logs/                      # 日志文件目录  
│   │   └── system.log  
│   └── ota/                       # OTA更新文件目录  
│       └── firmware.bin  
├── lib/                           # 第三方库（如果需要）  
│   └── README.md  
├── platformio.ini                 # PlatformIO配置文件  
└── README.md                      # 项目说明文档  
```




