[简体中文](./README.md) | [English](./README.en.md)

# FastBee-Arduino

**FastBee-Arduino is a feature-complete embedded IoT device development framework built on the ESP32-Arduino platform.**

---

### Introduction
FastBee-Arduino aims to be the most comprehensive and user-friendly embedded IoT development framework on the ESP32 platform. Through modular design and enterprise-grade features, it helps enterprises quickly create product prototypes and reduce development costs. It's also an excellent tool for individual developers to learn and use. The framework provides a one-stop solution covering the entire IoT device development chain, from device management and network communication to remote maintenance.

---

### Development Progress

MQTT and Modbus RTU fully supported, HTTP, CoAP, Modbus TCP, TCP protocol infrastructure ready

██████████████████████████████████████████████████████████████████████████  **95%**

---

### Features
* **Web Configuration:** Complete user management, role-based access control (RBAC), session management, responsive SPA design, native web technologies, multi-language support (Chinese/English), dark/light theme toggle
* **Web Frontend Optimization:** HTML dynamic module splitting (skeleton + on-demand loading), i18n engine with language pack separation, Service Worker offline caching, skeleton screen loading, resource preloading, Gzip compression (81.6% compression rate)
* **Network Communication:** WiFi STA/AP/AP+STA modes, automatic fallback to AP+STA on STA failure, mDNS local domain access, network status monitoring, WiFi scanning (two-column grid layout), TCP connection optimization (12 connections/64 queue)
* **Provisioning:** AP hotspot provisioning, Bluetooth BLE provisioning (NimBLE), auto-timeout
* **Protocol Support:** MQTT (publish/subscribe topic configuration), Modbus RTU/TCP, TCP server/client, HTTP client, CoAP
* **Real-time Push:** SSE (Server-Sent Events) real-time status push, instant broadcast of device data changes to web frontend
* **Remote Maintenance:** Firmware OTA update, filesystem OTA update, online log viewer, remote restart, factory reset, real-time device status reporting
* **Data Storage:** LittleFS filesystem, NVS Preferences dual storage, JSON configuration management, Gzip compression (81.6% compression rate)
* **Peripheral Management:** RS485 serial port, digital I/O, analog input, PWM output, I2C/SPI interfaces, visual GPIO pin configuration
* **Peripheral Execution:** Rule engine supporting platform/device/timer triggers, GPIO operations, system functions, command scripts
* **Device Control:** Relay/PWM/PID device type support, free drag-and-drop layout, register mode delay control, Modbus semaphore separation optimization
* **Rule Scripts:** Custom data processing scripts supporting MQTT/Modbus/HTTP protocol data format conversion
* **Health Monitoring:** Real-time CPU/memory/storage monitoring, network connection status, task status, anomaly alerts
* **Logging System:** Multi-level logging (DEBUG/INFO/WARN/ERROR), file storage, web viewer, log rotation
* **Task Scheduling:** Scheduled tasks, async event handling, priority scheduling
* **Security:** User authentication, role-based permissions, session management, remember login

---

### Hardware
![Device](./images/device.png)

* **Chip:** ESP32-WROOM-32U
* **CPU:** 240 MHz
* **Flash:** 4MB SPI Flash
* **SRAM:** 520 kB
* **Wireless:** WiFi 802.11 b/g/n + Bluetooth 4.2 + BLE
* **Power:** DC 9-36V, external antenna, USB programming port and config button
* **Terminal Connections:**
  * A/L: RS485-A (TX), GPIO17
  * B/H: RS485-B (RX), GPIO16
  * VCC: Power positive, DC 9-36V
  * GND: Power negative
  * DGND: Digital ground (isolated)
  * EGND: Protective earth
  * IO/L: Isolated digital I/O low side，GPIO21
  * IO/H: Isolated digital I/O high side，GPIO22
* **LED Indicators:**
  * POWER: Power indicator (solid = power OK)
  * STATE: Status LED, GPIO5 (active low)
  * DATA: Communication LED (blinks on data transfer)
* **Button:** GPIO0 (long press for config mode)

---

### Development Environment
* **IDE:** VSCode + PlatformIO
* **Target Chip:** ESP32 (ESP32-WROOM-32/32U/32D)
* **Flash Requirement:** 4MB or more
* **Framework:** Arduino-ESP32 2.0.x
* **Dependencies:**
  * ArduinoJson @ 7.4.2 - JSON parsing
  * PubSubClient @ 2.8 - MQTT client
  * NimBLE-Arduino @ 1.4.1 - Bluetooth BLE provisioning
  * ESPAsyncWebServer @ 3.9.2 - Async web server (in lib/)

---

### Getting Started
1. **Setup Environment:** Install VSCode and PlatformIO extension
2. **Clone Repository:** `git clone https://gitee.com/beecue/fastbee-arduino.git`
3. **Compress Frontend:** `node scripts/gzip-www.js`
4. **Upload Filesystem:** `pio run --target uploadfs`
5. **Upload Firmware:** `pio run --target upload`
6. **Access Device:**
   - First boot enters AP provisioning mode, connect to `fastbee-ap`
   - Or access via mDNS: http://fastbee.local
   - Default credentials: `admin` / `admin123`
7. **Network Setup:** Configure WiFi connection in web interface (WiFi scanning supported)
8. **Peripheral Setup:** Configure RS485, GPIO and other hardware interfaces (visual forms)
9. **Peripheral Execution:** Configure trigger rules and execution actions
10. **Protocol Setup:** Configure MQTT/Modbus to connect IoT platform

---

### System Architecture
```
┌─────────────────────────────────────────────────┐
│              Application Layer                   │
├─────────────────────────────────────────────────┤
│  Web UI  │ Device Monitor │ Scheduler │ Config  │
├─────────────────────────────────────────────────┤
│              Service Layer                       │
│  OTA  │ Network │ Logger │ Health │ TaskManager │
├─────────────────────────────────────────────────┤
│              Protocol Layer                      │
│    MQTT    │  Modbus  │   TCP   │    HTTP       │
├─────────────────────────────────────────────────┤
│              Security Layer                      │
│  UserMgr  │  RoleMgr  │  AuthMgr  │  Crypto     │
├─────────────────────────────────────────────────┤
│              Storage Layer                       │
│    NVS Preferences    │    LittleFS (JSON)      │
├─────────────────────────────────────────────────┤
│              Hardware Layer                      │
│  GPIO │ RS485 │ I2C │ SPI │ PWM │ ADC │ WiFi/BLE│
├─────────────────────────────────────────────────┤
│              ESP32 MCU (240MHz Dual Core)        │
└─────────────────────────────────────────────────┘
```

---

### Project Structure
```
FastBee-Arduino/
├── include/                       # Header files
│   ├── core/                      # Core framework
│   │   ├── FastBeeFramework.h     # Main framework
│   │   ├── ConfigDefines.h        # Config constants
│   │   ├── SystemConstants.h      # System constants
│   │   ├── PeripheralManager.h    # Peripheral management
│   │   ├── PeriphExecManager.h    # Peripheral execution
│   │   ├── RuleScriptManager.h    # Rule script management
│   │   └── FeatureFlags.h         # Feature flags
│   ├── network/                   # Network modules
│   │   ├── NetworkManager.h       # Network manager
│   │   ├── WiFiManager.h          # WiFi manager
│   │   ├── WebConfigManager.h     # Web config service
│   │   ├── OTAManager.h           # OTA update
│   │   ├── handlers/              # Route handlers
│   │   │   ├── SSERouteHandler.h  # SSE real-time push
│   │   │   ├── ModbusRouteHandler.h
│   │   │   └── ...                # Other API handlers
│   │   └── DNSServer.h            # DNS service
│   ├── protocols/                 # Protocol handlers
│   │   ├── ProtocolManager.h      # Protocol manager
│   │   ├── MQTTClient.h           # MQTT client
│   │   ├── ModbusHandler.h        # Modbus handler
│   │   ├── TCPHandler.h           # TCP handler
│   │   └── HTTPClientWrapper.h    # HTTP client
│   ├── security/                  # Security modules
│   │   ├── UserManager.h          # User manager
│   │   ├── RoleManager.h          # Role manager
│   │   ├── AuthManager.h          # Auth manager
│   │   └── CryptoUtils.h          # Crypto utils
│   ├── systems/                   # System services
│   │   ├── LoggerSystem.h         # Logger system
│   │   ├── TaskManager.h          # Task scheduler
│   │   ├── HealthMonitor.h        # Health monitor
│   │   └── ConfigStorage.h        # Config storage
│   └── utils/                     # Utilities
│       ├── StringUtils.h
│       ├── TimeUtils.h
│       ├── FileUtils.h
│       └── JsonConverters.h
├── src/                           # Source files
│   ├── core/                      # Core implementation
│   ├── network/                   # Network implementation
│   ├── protocols/                 # Protocol implementation
│   ├── security/                  # Security implementation
│   ├── systems/                   # System implementation
│   ├── utils/                     # Utils implementation
│   └── main.cpp                   # Main entry
├── data/                          # Filesystem
│   ├── www/                       # Web frontend
│   │   ├── index.html             # SPA skeleton page
│   │   ├── sw.js                  # Service Worker offline cache
│   │   ├── pages/                 # Dynamic loading page modules
│   │   ├── css/                   # Stylesheets
│   │   ├── js/                    # JavaScript
│   │   └── assets/                # Static assets
│   ├── config/                    # Configuration files
│   │   ├── device.json            # Device config
│   │   ├── network.json           # Network config
│   │   ├── protocol.json          # Protocol config
│   │   ├── peripherals.json       # Peripheral config
│   │   ├── periph_exec.json       # Peripheral execution config
│   │   ├── rule_scripts.json      # Rule script config
│   │   ├── roles.json             # Role config
│   │   └── users.json             # User config
│   └── logs/                      # Log directory
│       └── system.log
├── web-src/                       # Web frontend source
│   └── modules/                   # JS modules (admin/runtime)
├── lib/                           # Local libraries
│   └── ESPAsyncWebServer/         # Async web server
├── scripts/                       # Build scripts
│   ├── gzip-www.js                # Frontend compression
│   ├── build-web-modules.js       # Module builder
│   └── filter_littlefs.py         # File filtering
├── docs/                          # Documentation
│   ├── modbus_usage_guide.md      # Modbus usage guide
│   ├── periph_exec_flow.md        # Peripheral execution flow
│   └── script-guide.md            # Script guide
├── test/                          # Unit tests
│   ├── mocks/                     # Mock objects
│   └── helpers/                   # Test helpers
├── tests/                         # Integration tests
├── platformio.ini                 # PlatformIO config
├── fastbee.csv                    # Partition table
└── README.md                      # Documentation
```

---

### Screenshots

<img src="./images/device_10.png"/>

<img src="./images/device_11.png"/>

<img src="./images/device_01.png"/>

<img src="./images/device_02.png"/>

<img src="./images/device_03.png"/>

<img src="./images/device_04.png"/>

<img src="./images/device_05.png"/>

<img src="./images/device_06.png"/>

<img src="./images/device_07.png"/>

<img src="./images/device_08.png"/>

<img src="./images/device_09.png"/>

---

### License
This project is licensed under the MIT License - see the [LICENSE](./LICENSE) file for details.

---

### Contributing
1. Fork the repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

---

### Contact
* Gitee: [https://gitee.com/beecue/fastbee-arduino](https://gitee.com/beecue/fastbee-arduino)
* Issues: [Submit Issue](https://gitee.com/beecue/fastbee-arduino/issues)
