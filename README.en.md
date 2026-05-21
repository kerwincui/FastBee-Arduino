[简体中文](./README.md) | [English](./README.en.md)

<h1 align="center">FastBee-Arduino</h1>

<p align="center">
  <strong>A lightweight local Web IoT firmware for ESP32 / ESP32-S3</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32%20%7C%20ESP32--S3-blue" alt="Platform">
  <img src="https://img.shields.io/badge/framework-Arduino%20%2B%20PlatformIO-orange" alt="Framework">
  <img src="https://img.shields.io/badge/web%20image-%E2%89%88217KB-success" alt="Web Image">
  <img src="https://img.shields.io/badge/protocol-MQTT%20%2B%20Modbus%20RTU-informational" alt="Protocols">
  <img src="https://img.shields.io/badge/license-AGPL--3.0-green" alt="License">
</p>

FastBee-Arduino is a local Web configuration firmware designed for resource-constrained ESP32 devices. The current default build focuses on Web accessibility, login reliability, low-memory stability, and predictable behavior under concurrent browser requests.

The slim default profile keeps the core IoT workflow: dashboard, network setup, device settings, MQTT, Modbus RTU, peripheral configuration, peripheral execution, and device-control screen. High-memory and low-frequency features have been removed from the default ESP32 build to reduce boot, login, and page-loading pressure.

---

## System Highlights

FastBee-Arduino combines ESP32 firmware, a local Web console, peripheral configuration, and protocol integration into one lightweight IoT system for fast hardware validation and long-running field devices.

- **Visual configuration**: configure peripherals, networking, MQTT, Modbus RTU, execution rules, and the device-control screen from the browser.
- **Resource-aware builds**: slim builds target classic ESP32 / ESP32-C3 / ESP32-S3 for core workflows and low-memory stability; `esp32s3-full` enables full-feature validation on larger ESP32-S3 boards.

---

## Build Profiles

| Environment | Target | Profile | Notes |
|-------------|--------|---------|-------|
| `esp32` | Classic ESP32 | Slim production | Default recommended profile for 4MB Flash ESP32 boards |
| `esp32c3` | ESP32-C3 | Slim production | Conservative network and task-stack profile for smaller boards |
| `esp32s3` | ESP32-S3 | Slim production | Same feature set with more network headroom |
| `esp32s3-full` | ESP32-S3 | Full | For larger boards and feature validation |
| `native` | PC | Test | Host-side unit tests |

PlatformIO does not auto-switch build profiles from the chip detected on the serial port. The target is selected by the `-e` environment name: `pio run -e esp32` builds for classic ESP32, `pio run -e esp32c3` builds for ESP32-C3, `pio run -e esp32s3` builds the ESP32-S3 slim profile, and `pio run -e esp32s3-full` builds the ESP32-S3 full profile. If `-e` is omitted, `platformio.ini` uses `default_envs = esp32`; the chip name printed by `esptool` during upload is only a connection/write verification result and does not change the build target.

Slim builds keep dashboard, device control, network settings, device settings, protocol settings, peripheral configuration, peripheral execution, MQTT, Modbus RTU master, and core peripheral support. The full profile is mainly for ESP32-S3 validation of OTA, files/logs, multi-user features, RuleScript, and additional protocols.

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
│   ├── network/              # WiFi, Web server, route handlers
│   ├── protocols/            # MQTT and Modbus
│   ├── security/             # Auth, session, single-admin mode
│   └── systems/              # Health monitor, config storage, logger system
├── src/                      # C++ implementation
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
- Use `esp32s3` first on ESP32-S3 devices; switch to `esp32s3-full` only after confirming memory and flash headroom.
- Always flash both filesystem and firmware. Updating only firmware can leave stale Web files on the device.
- If `data/www` contains locked stale files on Windows, use the clean `.pio/fs-staging/run-*/www` package as the authoritative filesystem image.

---

## License

This project is licensed under **AGPL-3.0**. See [LICENSE](./LICENSE).
