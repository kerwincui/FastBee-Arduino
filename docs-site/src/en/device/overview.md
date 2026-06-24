---
title: Overview
order: 2
---

# FastBee-Arduino Project Overview

> Version: 1.0.0 | Chips: ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 | Framework: Arduino-ESP32 3.x (IDF 5.1)

## Introduction

FastBee-Arduino is a **zero-code Web IoT firmware** for the full ESP32 family. After flashing, configure network, device, protocol, peripheral, and rule settings entirely via browser — no programming required.

**Core Positioning**: ESP32 nodes, lightweight gateways, and field data acquisition & control terminals.

## Key Features

- **Zero-code Configuration**: All configuration done via web browser, no programming needed
- **Multi-protocol Support**: MQTT / Modbus RTU / TCP / HTTP / CoAP
- **Multi-network Access**: WiFi (AP+STA) / Ethernet (W5500) / 4G (EC801E)
- **Rich Peripherals**: GPIO, LCD, sensors, relays, NeoPixel, RFID, and more
- **Rule Engine**: Schedule trigger, event trigger, MQTT trigger, condition trigger
- **Remote Management**: OTA firmware upgrade, real-time monitoring, config import/export
- **Security**: Multi-user management, session management, password encryption

## Tech Stack

- Firmware: C++ / Arduino-ESP32 3.x / ESP-IDF 5.1 / PlatformIO
- Web Frontend: Vanilla JavaScript (no framework) / CSS / HTML
- Communication: MQTT / Modbus RTU / TCP / HTTP / CoAP
- Network: WiFi (AP+STA) / Ethernet (W5500) / 4G (EC801E)
- Storage: LittleFS + NVS (Preferences)

## Application Scenarios

- Smart Home: Lighting control, environment monitoring, security alarm
- Industrial IoT: Equipment monitoring, data acquisition, remote control
- Smart Agriculture: Greenhouse, irrigation control, weather monitoring
- Education & Research: IoT teaching, prototype verification, graduation projects

## Supported Hardware

| Chip | Flash | PSRAM | Feature Level |
|------|-------|-------|---------|
| ESP32 | 4-16MB | 0-4MB | Standard / Full |
| ESP32-S3 | 8-16MB | 0-8MB | Standard+OTA / Full |
| ESP32-C3 | 4MB | None | Lite |
| ESP32-C6 | 4MB | None | Lite |

> For more hardware details, see [Hardware Selection](./hardware-equipment.md) and [Edition Comparison](./edition-comparison.md).

## Quick Start

1. Use the [Online Flasher](./esp32-flasher.md) or [PlatformIO](./flashing-testing.md) to flash firmware onto your ESP32
2. Connect to the device WiFi hotspot `FastBee-XXXX`, password `fastbee123`
3. Open browser and navigate to `http://192.168.4.1`
4. Configure WiFi network and MQTT server connection

> See [Quick Start](./quick-start.md) for detailed steps.

## Related Links

- GitHub Repository: [https://github.com/kerwincui/FastBee-Arduino](https://github.com/kerwincui/FastBee-Arduino)
- FastBee IoT Platform: [http://fastbee.cn/](http://fastbee.cn/)
