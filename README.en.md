[简体中文](./README.md) | [English](./README.en.md)

<h1 align="center">FastBee-Arduino</h1>

<p align="center">
  <strong>A zero-code, visual-config IoT firmware for ESP32 full series</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32%20|%20S3%20|%20C3%20|%20C6-blue" alt="Platform">
  <img src="https://img.shields.io/badge/Arduino--ESP32-3.x%20(IDF%205.1)-orange" alt="Framework">
  <img src="https://img.shields.io/badge/PlatformIO-espressif32%207.x-orange" alt="PlatformIO">
  <img src="https://img.shields.io/badge/protocol-MQTT%20%2B%20Modbus%20RTU-informational" alt="Protocols">
  <img src="https://img.shields.io/badge/license-AGPL--3.0-green" alt="License">
</p>

FastBee-Arduino is a local Web IoT firmware for the **full ESP32 chip family** (ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6), built on **Arduino-ESP32 3.x** (ESP-IDF 5.1+).

The project provides three edition tiers based on hardware resources: **Lite** (C3/C6 low-cost nodes), **Standard** (ESP32/S3 general-purpose), and **Full** (S3 flagship gateway). All editions share the same codebase and configuration format — switch by changing the build target. See [Edition Comparison](docs/system/edition-comparison.md) for details.

---

## System Highlights

FastBee-Arduino combines ESP32 firmware, a local Web console, peripheral configuration, and protocol integration into one lightweight IoT system for fast hardware validation and long-running field devices.

- **Full chip family support**: ESP32, ESP32-S3, ESP32-C3, ESP32-C6 — one codebase covers from ¥9 entry-level to flagship solutions.
- **Arduino 3.x ecosystem**: Built on Arduino-ESP32 3.x (ESP-IDF 5.1+) with ESPAsyncWebServer 3.x, NimBLE 2.x, and other actively maintained libraries.
- **Visual configuration**: configure peripherals, networking, MQTT, Modbus RTU, execution rules, and the device-control screen from the browser.
- **Multi-network connectivity**: WiFi by default; Standard/Full support Ethernet (W5500 SPI) and 4G cellular (TinyGSM), while Full also supports LoRa gateway passthrough (E22-400T22D).
- **Three-tier architecture**: Lite (C3/C6) → Standard (ESP32/S3) → Full (S3), feature-scaled via compile-time flags with seamless upgrade path.

---

## Build Profiles

| Environment | Target Hardware | Default Flash | Default PSRAM | Edition | Max Peripherals / Rules | Notes |
|-------------|-----------------|---------------|---------------|---------|------------------------|-------|
| `esp32` | ESP32-WROOM / ESP32-WROVER | 4 MB | None | Standard | 24 / 16 (soft) | Default recommended, classic dual-core |
| `esp32c3` | ESP32-C3 | 4 MB | None | Lite | 16 / 12 (soft) | RISC-V low-cost (¥9-12) |
| `esp32c6` | ESP32-C6 | 4 MB | None | Lite | 16 / 12 (soft) | WiFi 6 + BLE 5.3, next-gen low-cost |
| `esp32s3` | ESP32-S3 | 8 MB | None | Standard | 24 / 16 (soft) | High-performance dual-core with I2C/RFID/IR |
| `esp32s3-full` | ESP32-S3 N16R8 | 16 MB | 8 MB | Full | 32 / 32 (soft) | OTA + multi-user + BLE provisioning + all protocols |
| `native` | PC | — | — | Test | — | Host-side unit tests |

PlatformIO does not auto-switch build profiles from the chip detected on the serial port. The target is selected by the `-e` environment name. If `-e` is omitted, `platformio.ini` uses `default_envs = esp32`.

```bash
# Lite edition
pio run -e esp32c3        # ESP32-C3
pio run -e esp32c6        # ESP32-C6 (requires pioarduino platform)

# Standard edition
pio run -e esp32          # Classic ESP32 (default)
pio run -e esp32s3        # ESP32-S3

# Full edition
pio run -e esp32s3-full   # ESP32-S3 full features
```

> **Note**: The ESP32-C6 environment requires the [pioarduino](https://github.com/pioarduino/platform-espressif32) community platform since the official espressif32 platform does not yet support C6 with Arduino framework.

---

## Build Instructions

### First-Time Compilation

On the first build, PlatformIO will automatically download and install the toolchain. Ensure a stable network connection and sufficient disk space:

| Component | Size | Applicable Environments | Description |
|-----------|------|------------------------|-------------|
| pioarduino platform | ~50 MB | All | Community espressif32 platform |
| Xtensa toolchain | ~500 MB | esp32, esp32s3 | ESP-IDF 5.x compiler |
| RISC-V toolchain | ~705 MB | esp32c3, esp32c6 | ESP-IDF 5.x compiler |
| Library dependencies | ~100 MB | All | ArduinoJson, ESPAsyncWebServer, etc. |

**Total**: The first build may require downloading **1~1.5 GB**. Subsequent builds do not need to re-download.

### Network Requirements

- PlatformIO downloads toolchains from GitHub and Espressif official servers
- If the network is slow, downloads may time out and auto-retry
- Recommended to build in a stable network environment
- If download fails, retry multiple times; PlatformIO supports resumable downloads

### ESP32-C6 Specific Notes

The ESP32-C6 environment is based on the slim lite edition and **disables DS18B20 temperature sensor by default** (OneWire library is not yet compatible with ESP32-C6's ESP-IDF 5.x GPIO register structures).

- DHT11/DHT22 humidity/temperature sensors: **Available**
- DS18B20 (OneWire) temperature sensor: **Unavailable**
- Other sensors (ultrasonic, current, voltage, etc.): **Available**

To use DS18B20 on ESP32-C6, refer to the OneWire library's [ESP32-C6 adaptation PR](https://github.com/PaulStoffregen/OneWire/pulls) or use alternative temperature sensors.

### Build Time Reference

| Environment | First Build | Incremental Build |
|-------------|-------------|-------------------|
| esp32 | 5~8 min | 30~60 sec |
| esp32c3 | 5~8 min | 30~60 sec |
| esp32s3 | 5~8 min | 30~60 sec |
| esp32s3-full | 8~12 min | 60~90 sec |

> Build times depend on CPU performance and disk speed; values above are for reference.

### FAQ

**Q: What if toolchain download fails?**

A: PlatformIO will auto-retry. You can run `pio run -e <env>` multiple times. Alternatively, delete the `.pio` directory and rebuild.

**Q: How to skip toolchain download?**

A: If you already have toolchains from another ESP32 project, copy `~/.platformio/packages/toolchain-*` to the corresponding location on your machine.

**Q: What if I encounter memory issues during compilation?**

A: ESP32-S3 full edition consumes more resources. Recommended:
- Use `esp32s3` standard edition instead of `esp32s3-full`
- Disable unnecessary features (see build flags in `platformio.ini`)
- Refer to [Edition Comparison Guide](docs/system/edition-comparison.md) to choose the appropriate edition

---

## Quick Start

### 1. Install Tools

Install VSCode with PlatformIO, or use PlatformIO CLI directly.

### 2. Build and Flash in One Command

```powershell
cd D:\project\gitee\FastBee-Arduino
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6
```

The command builds and uploads the matching LittleFS Web filesystem first, then builds and uploads the firmware.

Common examples:

```powershell
# Classic ESP32 Standard
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6

# ESP32-S3 Standard
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3 -Port COM6

# ESP32-S3 N16R8 Full
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6
```

`deploy.ps1` uses the environment matrix in `platformio.ini`, keeping the Web filesystem and firmware edition aligned. `buildfs/uploadfs` automatically selects a filesystem profile for the target environment:

| Filesystem Profile | Environments | Notes |
|--------------------|--------------|-------|
| `lite` | `esp32c3`, `esp32c6` | Compact Web/default config without Full admin pages or default Modbus resources |
| `standard` | `esp32`, `esp32s3` | Keeps MQTT/Modbus/common peripherals, removes logs/files/users/roles/OTA/RuleScript admin pages |
| `full` | `esp32s3-full` | Keeps the complete Web UI, admin pages, multilingual assets, and advanced features |

Peripheral templates are included by profile, but real GPIO/UART/display hardware is not enabled by default. After wiring a device, confirm the target chip supports the selected pins before enabling it in the Web UI.

Build without uploading:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -BuildOnly
```

Upload only the filesystem or only the firmware:

```powershell
# Upload LittleFS Web filesystem only
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6 -SkipFirmware

# Upload firmware only
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6 -SkipFs
```

Generated staging packages, images, and artifacts are placed under:

```text
.pio/fs-staging/<profile>-<env>-*
.pio/build/<env>/littlefs.bin
dist/firmware/<env>/
```

### 3. Post-Flash Smoke Test

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.4.1 -Profile standard
```

If the device has joined your LAN, use its assigned IP:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full
```

The smoke test logs in with the default credentials and checks auth, system, network, MQTT, peripheral configuration, peripheral execution, and batch APIs. The `full` profile also checks filesystem, logs, users, and roles.
When the device has just reconnected or the network is noisy, append `-RetryCount 2 -DelayMs 600` so transient 503, timeout, and low-memory guard responses are retried gently.

For longer API stability checks, run the soak test. It repeats the critical endpoints by round and can write a CSV report:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -Rounds 60 -RetryCount 2 -DelayMs 500 -ReportPath .pio\test-results\soak-full.csv
```

For pre-commit and release validation, use the unified test entry:

```powershell
# Static checks + all firmware builds
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static,build

# Complete local matrix without a real device
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static,native,build,artifacts
```

See [Testing and Version Validation](docs/testing.md) for the full matrix, and [Project Structure](docs/project-structure.md) for directory and file ownership.

### 4. Advanced PlatformIO Commands

```powershell
pio run -e esp32s3-full --target uploadfs --upload-port COM6
pio run -e esp32s3-full --target upload --upload-port COM6
```

After each successful build, the latest artifacts for that environment are copied to:

```text
dist/firmware/<env>/
```

`factory.bin` is a merged image containing the bootloader, partition table, OTA data when generated, and the app firmware. After `buildfs/uploadfs`, the directory also contains `littlefs.bin`, `<profile>-littlefs.bin`, and `factory-with-fs.bin`.

### 5. Build All Release Artifacts

To generate firmware and filesystem images for all chip environments in one run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1
```

The script builds `esp32`, `esp32c3`, `esp32s3`, `esp32c6`, and `esp32s3-full` in sequence. For each environment it builds the firmware first, then the matching LittleFS image, and finally copies every version into:

```text
dist/firmware/all-latest/
```

Release files use `fastbee-{chip}n{Flash}r{PSRAM}-{edition}.bin` names:

| File | Environment |
|------|-------------|
| `fastbee-esp32n4r0-std.bin` | `esp32` |
| `fastbee-esp32c3n4r0-lite.bin` | `esp32c3` |
| `fastbee-esp32c6n8r0-lite.bin` | `esp32c6` |
| `fastbee-esp32s3n8r0-std.bin` | `esp32s3` |
| `fastbee-esp32s3n16r8-full.bin` | `esp32s3-full` |

The directory also contains `manifest.json` with the environment, filesystem profile, hardware target, release file name, and suggested deploy command.

The script overwrites same-name files by default. Add `-CleanOutput` if the output directory should be cleaned first.

To choose a release directory:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -OutputDir dist\firmware\release-20260607
```

To only collect existing `dist/firmware/<env>/` artifacts without rebuilding:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -SkipBuild
```

### 6. Open Serial Monitor

```powershell
pio device monitor -e esp32 -b 115200
```

### 7. Access the Device

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
- If `data/www` contains locked stale files on Windows, use the clean `.pio/fs-staging/<profile>-<env>-*` package as the authoritative filesystem image.

---

## License

This project is licensed under **AGPL-3.0**. See [LICENSE](./LICENSE).
