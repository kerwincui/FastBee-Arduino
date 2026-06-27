[简体中文](./README.md) | [English](./README.en.md)

<h1 align="center">FastBee-Arduino</h1>

<p align="center">
  <strong>Zero-code, visual configuration — turn ESP32 into a full-featured IoT device in seconds.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32%20|%20S3%20|%20C3%20|%20C6-blue" alt="Platform">
  <img src="https://img.shields.io/badge/Arduino--ESP32-3.x%20(IDF%205.1)-orange" alt="Framework">
  <img src="https://img.shields.io/badge/PlatformIO-espressif32%207.x-orange" alt="PlatformIO">
  <img src="https://img.shields.io/badge/license-AGPL--3.0-green" alt="License">
</p>

FastBee-Arduino is a zero-code Web IoT firmware for the ESP32 full series. After flashing, configure networking, devices, protocols, peripherals, and rules entirely from a browser. Suitable for ESP32 nodes, lightweight gateways, and field data-acquisition & control terminals. Whether you're a beginner or a professional developer, FastBee-Arduino helps you develop and mass-produce IoT devices quickly and easily.

Supported chips: `ESP32`, `ESP32-S3`, `ESP32-C3`, `ESP32-C6`.

## Pre-defined Environments

| PlatformIO Environment | Edition | Chip | Flash | PSRAM | Key Capabilities |
| --- | --- | --- | --- | --- | --- |
| `esp32c3-F4R0` | Lite | ESP32-C3 | 4MB | None | WiFi, MQTT, basic peripherals, execution rules, NeoPixel |
| `esp32c6-F4R0` | Lite | ESP32-C6 | 4MB | None | Same as above, WiFi 6 support |
| `esp32-F4R0` | Standard | ESP32 | 4MB | None | Lite + command script, Modbus, I2C sensors, RFID, IR, Ethernet, 4G |
| `esp32s3-F8R0` | Standard | ESP32-S3 | 8MB | None | Same as esp32 Standard + **OTA**, dual-core higher performance |
| `esp32-F8R4` | Full | ESP32 | 8MB | 4MB | Standard + OTA, rule script, file management, logs, multi-user, BLE |
| `esp32s3-F8R4` | Full | ESP32-S3 | 8MB | 4MB | Same as above, dual-core higher performance |
| `esp32s3-F16R8` | Full | ESP32-S3 | 16MB | 8MB | Same as above, larger storage and memory headroom |

> Edition selection is primarily based on **Flash capacity** and **PSRAM availability**. Environments with Flash ≥ 8MB support OTA upgrades (4MB environments cannot due to space constraints). Modules with PSRAM (e.g., ESP32-WROVER, ESP32-S3-N8R2/N16R8) can use the Full edition.

> For detailed feature comparison and partition tables, see [Edition Comparison](https://fastbee.cn/doc/device/getting-started/edition-comparison.html).

## Quick Flash

### Online Flash (Recommended for Quick Try)

No tools required — open your browser and flash the firmware instantly to try the system: 👉 **[Online Flash Tool](https://fastbee.cn/doc/device/esp32-flasher.html)**

### Build and Flash from Source

1. Install VSCode + PlatformIO, or install PlatformIO CLI.
2. Connect the development board and confirm the serial port, e.g., `COM6`.
3. Run from the project root:

```powershell
# Windows
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32-F4R0 -Port COM6

# Linux / macOS (requires PowerShell Core: pwsh)
pwsh -File scripts/deploy.ps1 -Env esp32-F4R0 -Port /dev/ttyUSB0
```

**deploy.ps1 parameters** (build + flash):

| Parameter | Description |
|-----------|-------------|
| `-Env` | PlatformIO build environment, e.g. `esp32-F4R0`, see predefined environments above |
| `-Port` | Serial port, e.g. `COM6` |
| `-Monitor` | Open serial monitor automatically after flashing |
| `-BuildOnly` | Compile only without flashing, for CI or quick verification |
| `-SkipFs` | Skip filesystem (LittleFS) upload |
| `-SkipFirmware` | Skip firmware flash, only upload filesystem |
| `-SkipDoctor` | Skip environment diagnostics check |
| `-SkipWeb` | Skip automatic Web asset generation (has no effect if `.gz` files already exist) |

## Quick Start

The device enters AP mode on first boot or when WiFi is not configured:

| Item | Default |
| --- | --- |
| WiFi Hotspot | `FastBee-XXXX` |
| Browser Address | `http://192.168.4.1` or `http://fastbee.local` |
| Username | `admin` |
| Password | `admin123` |

```mermaid
graph LR
    A[ESP32 Hardware] -->|1. Flash Firmware| B[FastBee-Arduino Firmware]
    B -->|2. Initialize Device| C[Peripheral Config]
    B -->|3. Choose Network| D[Network Config]
    B -->|4. Connect IoT Platform| E[Communication Protocol]
    B -->|5. Rule Orchestration| F[Peripheral Execution]
    D -->|WiFi, 4G, Ethernet| G[IoT Platform]
    E --> G
```

| Step | Phase | What to Do | Page |
|------|-------|------------|------|
| 1 | **Flash Firmware** | Flash FastBee-Arduino firmware to ESP32 using PlatformIO | [Online Flash Tool](https://fastbee.cn/doc/device/esp32-flasher.html) |
| 2 | **Peripheral Config** | Select peripheral types and assign pins in the Web UI to initialize hardware | Peripheral Config |
| 3 | **Network Config** | Choose connectivity (WiFi / Ethernet / 4G), enter parameters and save | Network Config |
| 4 | **Communication Protocol** | Configure MQTT / Modbus RTU protocols to connect to IoT platform | Communication Protocol |
| 5 | **Peripheral Execution** | Set trigger conditions and actions for button-controlled lights, timed linkage, sensor-driven control, etc. | Peripheral Execution |

> Entirely code-free: Flash firmware → Open browser → Click to configure → Device is ready to use.

## Screenshots

<table>
  <tr>
    <td><img src="./images/device_01.png" alt="Dashboard"/></td>
    <td><img src="./images/device_10.png" alt="Device Control Screen"/></td>
  </tr>
  <tr>
    <td><img src="./images/device_11.png" alt="Chinese Interface"/></td>
    <td><img src="./images/device_12.png" alt="Fullscreen Display"/></td>
  </tr>
</table>

## Case Showcase

FastBee-Arduino in action across different chips and hardware platforms.

<table>
<tr>
<td align="center" width="33%">
<a href="https://fastbee.cn/doc/device/examples/esp32c6-blink-quickstart.html"><img src="./images/case-esp32c6.png" alt="ESP32-C6 Blink" width="100%"/></a>
<br/><a href="https://fastbee.cn/doc/device/examples/esp32c6-blink-quickstart.html"><b>ESP32-C6 Full-Color LED Quick Start</b></a>
<br/><sub>Flash firmware on an ESP32-C6 dev board and run an LED blink test within 5 minutes — experience the zero-code onboarding flow.</sub>
</td>
<td align="center" width="33%">
<a href="https://fastbee.cn/doc/device/examples/esp32c3-temperature-humidity.html"><img src="./images/case-esp32c3.png" alt="ESP32-C3 Temp & Humidity" width="100%"/></a>
<br/><a href="https://fastbee.cn/doc/device/examples/esp32c3-temperature-humidity.html"><b>ESP32-C3 Temperature & Humidity Monitor</b></a>
<br/><sub>Collect ambient temperature and humidity via a DHT11 sensor, display on OLED in real time, and trigger alarms on threshold breach.</sub>
</td>
<td align="center" width="33%">
<a href="https://fastbee.cn/doc/device/examples/hardware-showcase.html"><img src="./images/case-device-modbus.png" alt="ESP32 Modbus" width="100%"/></a>
<br/><a href="https://fastbee.cn/doc/device/examples/hardware-showcase.html"><b>ESP32 Modbus Monitoring & Control</b></a>
<br/><sub>ESP32 IoT terminal collects Modbus RTU slave data via RS485 for industrial sensor monitoring and remote control.</sub>
</td>
</tr>
<tr>
<td align="center" width="33%">
<a href="https://fastbee.cn/doc/device/examples/ecm50a-networking.html"><img src="./images/case-ecm50a.png" alt="ECM50-A Networking" width="100%"/></a>
<br/><a href="https://fastbee.cn/doc/device/examples/ecm50a-networking.html"><b>ECM50-A 4G/Ethernet/WiFi Networking</b></a>
<br/><sub>ESP32-S3 based ECM50-A industrial device with auto-switching between 4G cellular, W5500 Ethernet, and WiFi connectivity.</sub>
</td>
<td align="center" width="33%">
<a href="https://fastbee.cn/doc/device/examples/puzhong-esp32-test.html"><img src="./images/case-puzhong.png" alt="Puzhong Dev Board" width="100%"/></a>
<br/><a href="https://fastbee.cn/doc/device/examples/puzhong-esp32-test.html"><b>ESP32 Puzhong Board Full Feature Test</b></a>
<br/><sub>Comprehensive testing of GPIO, ADC, PWM, I2C, SPI, UART, display, and other peripherals on the Puzhong ESP32 dev board.</sub>
</td>
<td align="center" width="33%">
<a href="https://fastbee.cn/doc/device/examples/waveshare-esp32s3.html"><img src="./images/case-weixue.png" alt="WaveShare" width="100%"/></a>
<br/><a href="https://fastbee.cn/doc/device/examples/waveshare-esp32s3.html"><b>ESP32-S3 WaveShare Industrial Relay</b></a>
<br/><sub>WaveShare ESP32-S3 board adaptation covering WiFi, sensor acquisition, display driving, and MQTT reporting.</sub>
</td>
</tr>
</table>

See the [Example Tutorials](https://fastbee.cn/doc/device/examples/) directory for more cases.

## Project Structure

```text
src/              Firmware source code
include/          Headers and feature switches
data/             LittleFS default config and Web artifacts
web-src/          Web frontend source
scripts/          Build, flash, test, and release scripts
test/             PlatformIO native tests
test/browser/     Playwright browser automation (18 suites / 625 cases)
docs/             Test plans and design documents
```

## Documentation

📖 **Full online documentation**: https://fastbee.cn/doc/device/

| Document | Content |
| --- | --- |
| Quick Start | From flashing to creating your first rule |
| Deployment | Firmware, LittleFS, release packages, and device verification |
| Testing | Static checks, native, all-chip build, smoke, and stability tests |
| Edition Comparison | Lite, Standard, Full feature differences |
| Project Structure | Directory, module, and build artifact description |
| User Manual | Web page and feature operation guide |
| Supported Modules | Modules, sensors, and access methods by edition |
| Peripheral Docs | GPIO, sensors, displays, RFID, Modbus peripherals |
| Peripheral Execution | Triggers, actions, and linkage rules |
| Example Tutorials | LED, relay, sensor, display, and Modbus examples |

## License

This project is licensed under [AGPL-3.0](LICENSE).
