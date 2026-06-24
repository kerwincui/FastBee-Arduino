---
title: Edition Comparison
order: 4
---

# Edition Selection & Feature Comparison

> Choose the most suitable firmware edition based on your hardware and application needs.

## Edition Overview

FastBee-Arduino offers three feature levels, selected via PlatformIO environment names:

| Edition | Target Chips | Flash Required | PSRAM | Positioning |
|------|---------|-----------|-------|------|
| **Lite** | ESP32-C3, ESP32-C6 | 4MB | None | Lightweight Node |
| **Standard** | ESP32, ESP32-S3 | 4-8MB | None | Standard Gateway |
| **Full** | ESP32, ESP32-S3 | 8MB+ | 4MB+ | Full-Featured Terminal |

## Feature Comparison Matrix

### Network Access

| Feature | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| WiFi STA + AP | ✅ | ✅ | ✅ |
| mDNS | ✅ | ✅ | ✅ |
| Ethernet (W5500) | ❌ | ❌ | ✅ |
| 4G Cellular (EC801E) | ❌ | ❌ | ✅ |
| BLE | ❌ | ❌ | ❌ |

### Communication Protocols

| Feature | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| MQTT | ✅ | ✅ | ✅ |
| Modbus RTU Master | ❌ | ✅ | ✅ |
| Modbus Slave | ❌ | ❌ | ✅ |
| TCP Server | ❌ | ❌ | ✅ |
| HTTP Client | ❌ | ❌ | ✅ |
| CoAP | ❌ | ❌ | ✅ |

### Peripheral Support

| Feature | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| GPIO Input/Output | ✅ | ✅ | ✅ |
| DHT11/DHT22 Sensors | ✅ | ✅ | ✅ |
| DS18B20 Temperature | ✅ | ✅ | ✅ |
| OLED/LCD Display | ✅ | ✅ | ✅ |
| NeoPixel LED | ✅ | ✅ | ✅ |
| 7-Segment Display | ✅ | ✅ | ✅ |
| I2C Advanced Sensors | ❌ | ✅ | ✅ |
| RFID Reader | ❌ | ✅ | ✅ |
| IR Remote | ❌ | ✅ | ✅ |
| LED Matrix | ❌ | ✅ | ✅ |
| DS1302 RTC | ❌ | ❌ | ✅ |
| LCD1602 Character LCD | ❌ | ❌ | ✅ |

### System Features

| Feature | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| Web Management UI | ✅ | ✅ | ✅ |
| Peripheral Execution Rules | ✅ | ✅ | ✅ |
| Rule Script Engine | ❌ | ✅ | ✅ |
| Command Scripts | ❌ | ✅ | ✅ |
| OTA Firmware Upgrade | ❌ | ⚠️ | ✅ |
| Multi-User Management | ❌ | ❌ | ✅ |
| File Manager | ❌ | ❌ | ✅ |
| Log Viewer | ❌ | ❌ | ✅ |
| File Logging | ❌ | ❌ | ✅ |
| i18n Internationalization | ❌ | ❌ | ✅ |

## Build Environment Mapping

| PlatformIO Environment | Chip | Flash | PSRAM | Feature Level | Partition Table |
|----------------|------|-------|-------|---------|--------|
| `esp32c3-F4R0` | ESP32-C3 | 4MB | None | Lite | `fastbee.csv` |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | None | Lite | `fastbee.csv` |
| `esp32-F4R0` | ESP32 | 4MB | None | Standard | `fastbee.csv` |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | None | Standard+OTA | `fastbee-8MB.csv` |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | Full | `fastbee-8MB.csv` |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | Full | `fastbee-8MB.csv` |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | Full | `fastbee-16MB.csv` |

### Partition Table Description

| Partition File | Flash | App Slots | OTA | LittleFS |
|---------|-------|---------|-----|----------|
| `fastbee.csv` | 4MB | 2.88MB × 1 | No | 1MB |
| `fastbee-8MB.csv` | 8MB | 3.5MB × 2 | Yes | 960KB |
| `fastbee-16MB.csv` | 16MB | 4MB × 2 | Yes | 7.9MB |

## How to Choose

1. **Check your hardware first**: Verify your ESP32 module's Flash and PSRAM size
2. **Confirm feature requirements**: Whether you need Modbus, OTA, Ethernet, 4G, etc.
3. **Refer to the table above**: Match the corresponding PlatformIO environment name

> For flashing and deployment commands, see [Flashing & Deployment](./flashing-testing.md).
