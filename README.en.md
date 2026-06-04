[简体中文](./README.md) | [English](./README.en.md)

<h1 align="center">FastBee-Arduino</h1>

<p align="center">
  <strong>A zero-code, visual-config IoT firmware for ESP32 full series</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32%20|%20S3%20|%20C3%20|%20C6%20|%20S2-blue" alt="Platform">
  <img src="https://img.shields.io/badge/Arduino--ESP32-3.x%20(IDF%205.1)-orange" alt="Framework">
  <img src="https://img.shields.io/badge/PlatformIO-espressif32%207.x-orange" alt="PlatformIO">
  <img src="https://img.shields.io/badge/protocol-MQTT%20%2B%20Modbus%20RTU-informational" alt="Protocols">
  <img src="https://img.shields.io/badge/license-AGPL--3.0-green" alt="License">
</p>

FastBee-Arduino is a local Web IoT firmware for the **full ESP32 chip family** (ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 / ESP32-S2), built on **Arduino-ESP32 3.x** (ESP-IDF 5.1+).

The project provides three edition tiers based on hardware resources: **Lite** (C3/C6/S2 low-cost nodes), **Standard** (ESP32/S3 general-purpose), and **Full** (S3 flagship gateway). All editions share the same codebase and configuration format — switch by changing the build target. See [Edition Comparison](docs/system/edition-comparison.md) for details.

---

## System Highlights

FastBee-Arduino combines ESP32 firmware, a local Web console, peripheral configuration, and protocol integration into one lightweight IoT system for fast hardware validation and long-running field devices.

- **Full chip family support**: ESP32, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-S2 — one codebase covers from ¥9 entry-level to flagship solutions.
- **Arduino 3.x ecosystem**: Built on Arduino-ESP32 3.x (ESP-IDF 5.1+) with ESPAsyncWebServer 3.x, NimBLE 2.x, and other actively maintained libraries.
- **Visual configuration**: configure peripherals, networking, MQTT, Modbus RTU, execution rules, and the device-control screen from the browser.
- **Multi-network connectivity**: WiFi by default; Standard/Full support Ethernet (W5500 SPI) and 4G cellular (TinyGSM), while Full also supports LoRa gateway passthrough (E22-400T22D).
- **Three-tier architecture**: Lite (C3/C6/S2) → Standard (ESP32/S3) → Full (S3), feature-scaled via compile-time flags with seamless upgrade path.

---

## Build Profiles

| Environment | Target | Edition | Notes |
|-------------|--------|---------|-------|
| `esp32` | Classic ESP32 | Standard | Default recommended, classic dual-core |
| `esp32c3` | ESP32-C3 | Lite | RISC-V low-cost (¥9-12) |
| `esp32c6` | ESP32-C6 | Lite | WiFi 6 + BLE 5.3, next-gen low-cost |
| `esp32s2` | ESP32-S2 | Lite | Xtensa single-core, WiFi only (no BLE) |
| `esp32s3` | ESP32-S3 | Standard | High-performance dual-core with I2C/RFID/IR |
| `esp32s3-full` | ESP32-S3 | Full | OTA + multi-user + BLE provisioning + all protocols |
| `native` | PC | Test | Host-side unit tests |

PlatformIO does not auto-switch build profiles from the chip detected on the serial port. The target is selected by the `-e` environment name. If `-e` is omitted, `platformio.ini` uses `default_envs = esp32`.

```bash
# Lite edition
pio run -e esp32c3        # ESP32-C3
pio run -e esp32c6        # ESP32-C6 (requires pioarduino platform)
pio run -e esp32s2        # ESP32-S2

# Standard edition
pio run -e esp32          # Classic ESP32 (default)
pio run -e esp32s3        # ESP32-S3

# Full edition
pio run -e esp32s3-full   # ESP32-S3 full features
```

> **Note**: The ESP32-C6 environment requires the [pioarduino](https://github.com/pioarduino/platform-espressif32) community platform since the official espressif32 platform does not yet support C6 with Arduino framework.

---

## Quick Start

### 1. Install Tools

Install VSCode with PlatformIO, or use PlatformIO CLI directly.

### 2. Build and Upload Web Filesystem

```powershell
cd D:\project\gitee\FastBee-Arduino
node scripts/gzip-www.js --web-slim --no-monitor
```

To build the clean staging package without uploading:

```powershell
node scripts/gzip-www.js --web-slim --no-upload --no-monitor
```

The generated publish package is placed under:

```text
.pio/fs-staging/run-*/www
```

### 3. Build and Upload Firmware

Classic ESP32:

```powershell
pio run -e esp32 --target upload
```

ESP32-S3 slim:

```powershell
pio run -e esp32s3 --target upload
```

ESP32-S3 full:

```powershell
pio run -e esp32s3-full --target upload
```

### 4. Open Serial Monitor

```powershell
pio device monitor -e esp32 -b 115200
```

### 5. Access the Device

- On first boot or without WiFi configuration, the device starts in AP mode.
- Open `http://192.168.4.1` or `http://fastbee.local`.
- Default username: `admin`
- Default password: `admin123`

---

## Core Features

### Dashboard

- System, network, runtime, and peripheral status overview.
- Lightweight first-screen requests to reduce ESP32 memory pressure.

### Device Settings

- Basic configuration, time configuration, and advanced settings.
- Cache management and `/data/config` import/export.
- Restart, factory reset, and browser-cache cleanup.

### Protocols

- MQTT connection configuration, authentication, status check, and test connection.
- MQTT supports multiple network transports by edition: WiFi on all builds, Ethernet/4G on Standard and Full, and LoRa passthrough on Full.
- Modbus RTU serial binding, master status, device mapping, and control helpers.

### Peripheral Configuration and Execution

- GPIO, sensors, displays, seven-segment display, RS485/UART, and other core peripheral types.
- Peripheral execution connects triggers to actions for lightweight automation.
- Pagination and button layouts are optimized for narrow cards and smaller screens.

### Device Control Screen

- Centralized field display and control page.
- Supports fullscreen access and deferred loading.

---

## Project Layout

```text
FastBee-Arduino/
├── include/                  # Headers
│   ├── core/                 # Framework, peripherals, execution scheduler
│   ├── network/              # WiFi, Web server, route handlers, network adapters
│   ├── protocols/            # MQTT and Modbus
│   ├── security/             # Auth, session, single-admin mode
│   └── systems/              # Health monitor, config storage, logger system
├── src/                      # C++ implementation
│   └── network/              # Contains EthernetAdapter / CellularAdapter / LoRaAdapter
├── web-src/                  # Editable Web frontend source
│   ├── css/                  # Source styles
│   ├── js/                   # Boot, state, request-governor source
│   ├── modules/              # Runtime page module source
│   └── pages/                # Page and fragment source
├── data/
│   ├── config/               # Default JSON configuration
│   └── www/                  # Web upload artifacts: compressed files plus static images
│       ├── assets/           # logo and favicon
│       ├── css/*.css.gz      # compressed styles generated from web-src/css
│       ├── js/**/*.js.gz     # compressed scripts generated from web-src/js and modules
│       └── pages/**/*.html.gz # compressed pages generated from web-src/pages
├── scripts/                  # Web build, gzip, manifest, validation scripts
├── test/                     # Unit tests and mocks
├── docs/                     # Documentation
├── platformio.ini            # Simplified PlatformIO environment matrix
└── fastbee.csv               # Partition table
```

---

## Deployment Notes

- Use `esp32` for classic ESP32 unless there is a strong reason not to.
- Use `esp32s3` (Standard) first on ESP32-S3 devices; switch to `esp32s3-full` only after confirming memory and flash headroom.
- For low-cost nodes, prefer `esp32c3` (cheapest) or `esp32c6` (WiFi 6, future-proof).
- Always flash both filesystem and firmware. Updating only firmware can leave stale Web files on the device.
- If `data/www` contains locked stale files on Windows, use the clean `.pio/fs-staging/run-*/www` package as the authoritative filesystem image.

---

## License

This project is licensed under **AGPL-3.0**. See [LICENSE](./LICENSE).
