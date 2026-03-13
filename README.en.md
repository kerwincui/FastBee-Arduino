# FastBee-Arduino

**FastBee-Arduino is a feature-complete embedded IoT device development framework built on the ESP32-Arduino platform.**

---

### Introduction
FastBee-Arduino aims to be the most comprehensive and user-friendly embedded IoT development framework on the ESP32 platform. Through modular design and enterprise-grade features, it helps enterprises quickly create product prototypes and reduce development costs. It's also an excellent tool for individual developers to learn and use. The framework provides a one-stop solution covering the entire IoT device development chain, from device management and network communication to remote maintenance.

---

### Development Progress

Under active development...

████████████████████████████████████████████████░░░░░░░░░░░░░░  **80%**

---

### Features
* **Web Configuration:** Complete user management, role-based access control (RBAC), session management, responsive SPA design, native web technologies, multi-language support (Chinese/English)
* **Network Communication:** WiFi STA/AP/AP+STA modes, automatic fallback mechanism, mDNS local domain access, network status monitoring, WiFi scanning
* **Provisioning:** AP hotspot provisioning, Bluetooth BLE provisioning (NimBLE), auto-timeout
* **Protocol Support:** MQTT, Modbus RTU/TCP, TCP server/client, HTTP client
* **Remote Maintenance:** Firmware OTA update, filesystem OTA update, online log viewer, remote restart, factory reset
* **Data Storage:** LittleFS filesystem, NVS Preferences dual storage, JSON configuration management, Gzip compression
* **Peripheral Management:** RS485 serial port, digital I/O, analog input, PWM output, I2C/SPI interfaces, visual GPIO pin configuration
* **Health Monitoring:** Real-time CPU/memory/storage monitoring, network connection status, task status, anomaly alerts
* **Logging System:** Multi-level logging (DEBUG/INFO/WARN/ERROR), file storage, web viewer, log rotation
* **Task Scheduling:** Scheduled tasks, async event handling, priority scheduling
* **Security:** User authentication, role-based permissions, session management, remember login

---

### Hardware
![Device](./images/device.png)

* **Chip:** ESP32-WROOM-32U
* **Flash:** 4MB SPI Flash
* **SRAM:** 520 kB
* **Wireless:** WiFi 802.11 b/g/n + Bluetooth 4.2 + BLE
* **Power:** DC 9-36V, external antenna, USB programming port and config button
* **Terminal Connections:**
  * A/L: RS485-A (TX), GPIO17
  * B/H: RS485-B (RX), GPIO18
  * VCC: Power positive, DC 9-36V
  * GND: Power negative
  * DGND: Digital ground (isolated)
  * EGND: Protective earth
  * IO/L: Isolated digital I/O low side
  * IO/H: Isolated digital I/O high side
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
  * WebSockets @ 2.3.6 - WebSocket communication
  * PubSubClient @ 2.8 - MQTT client
  * NimBLE-Arduino @ 1.4.1 - Bluetooth BLE provisioning
  * ESPAsyncWebServer @ 3.9.2 - Async web server (in lib/)

---

### Flash Partition Layout
| Partition | Type | Offset | Size | Description |
|-----------|------|--------|------|-------------|
| nvs | data | 0x9000 | 20KB | NVS key-value storage |
| otadata | data | 0xe000 | 8KB | OTA data |
| app0 | app | 0x10000 | 2.25MB | Application |
| spiffs | data | 0x260000 | 1.625MB | LittleFS filesystem |

---

### Getting Started
1. **Setup Environment:** Install VSCode and PlatformIO extension
2. **Clone Repository:** `git clone https://gitee.com/beecue/fastbee-arduino.git`
3. **Compress Frontend:** `node scripts/gzip-www.js`
4. **Upload Filesystem:** PlatformIO → Upload Filesystem Image
5. **Upload Firmware:** PlatformIO → Upload
6. **Access Device:**
   - First boot enters AP provisioning mode, connect to `fastbee-ap`
   - Or access via mDNS: http://fastbee.local
   - Default credentials: `admin` / `admin123`
7. **Network Setup:** Configure WiFi connection in web interface
8. **Peripheral Setup:** Configure RS485, GPIO and other hardware interfaces
9. **Protocol Setup:** Configure MQTT/Modbus to connect IoT platform

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
│   ├── network/                   # Network modules
│   ├── protocols/                 # Protocol handlers
│   ├── security/                  # Security modules
│   ├── systems/                   # System services
│   └── utils/                     # Utilities
├── src/                           # Source files
├── data/                          # Filesystem
│   ├── www/                       # Web frontend
│   ├── config/                    # Configuration files
│   └── logs/                      # Log files
├── lib/                           # Local libraries
├── scripts/                       # Build scripts
├── platformio.ini                 # PlatformIO config
├── fastbee.csv                    # Partition table
└── README.md                      # Documentation
```

---

### Screenshots

<img src="./images/device_01.png"/>

<img src="./images/device_02.png"/>

<img src="./images/device_03.png"/>

<img src="./images/device_04.png"/>

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
