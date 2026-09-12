[Simplified Chinese](./README.md) | [English](./README.en.md)

# FastBee-Arduino

> Zero-code Web IoT firmware for the ESP32 family. After flashing, configure networking, peripherals, protocols, and automation rules from a browser.

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL%203.0-blue.svg)](LICENSE)

Source repository: [gitee.com/beecue/fastbee-arduino](https://gitee.com/beecue/fastbee-arduino)
Automated checks: [`.github/workflows/ci.yml`](.github/workflows/ci.yml)

## Capabilities

- **Browser management**: local Web console for device status, networking, peripherals, protocols, logs, and configuration backup.
- **Networking**: every release target supports WiFi AP/STA. Selected ESP32 and ESP32-S3 targets also support W5500 Ethernet and EC801E-CN cellular when the hardware is present and the feature is enabled.
- **Peripherals**: GPIO, ADC/PWM, DHT, DS18B20, AHT20, SHT31, BH1750, BMP280, MPU6050, OLED, LCD1602, TM1637, NeoPixel, servos, stepper motors, RFID, infrared, 433 MHz RF, and Modbus devices.
- **Local automation**: platform, timer, event, and polling triggers with GPIO, display, motor, Modbus, script, and reporting actions.
- **Maintenance**: authentication, health monitoring, low-memory protection, SSE updates, configuration transfer, and OTA on A/B-partition targets.

The release firmware exposes **MQTT/MQTTS and Modbus RTU master** as device communication protocols. The management Web server, HTTP NTP, and HTTP OTA are not a generic HTTP telemetry protocol; raw TCP, Modbus TCP, CoAP, and a generic outbound HTTP client are not release capabilities.

## Runtime Model

Release targets share the `unified_flags` feature baseline. Hardware-dependent capabilities such as OTA, Ethernet, cellular, and infrared can still be overridden or compiled out per environment. Runtime configuration and memory protection then decide whether a compiled feature is active.

## Release Environments

Build environments are defined by [`platformio.ini`](platformio.ini); the merged-image list is maintained in FastBee-doc at `src/device/fim/manifest.json`:

| Environment | Chip | Flash | PSRAM | OTA |
| --- | --- | ---: | ---: | :---: |
| `esp32-F4R0` | ESP32 | 4MB | None | No |
| `esp32-F8R0` | ESP32 | 8MB | None | Yes |
| `esp32-F8R8` | ESP32 | 8MB | 8MB | Yes |
| `esp32c3-F4R0` | ESP32-C3 | 4MB | None | No |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | None | No |
| `esp32c6-F8R0` | ESP32-C6 | 8MB | None | Yes |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | None | Yes |
| `esp32s3-F8R8` | ESP32-S3 | 8MB | 8MB | Yes |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | Yes |

`F` and `R` identify Flash and PSRAM capacity. `R0` means no PSRAM. The standard release matrix has nine environments and no longer uses `F8R4`.

The 4MB environments use a single application partition and cannot perform OTA. The 8MB and 16MB environments use A/B application partitions. Ethernet and cellular are compiled out of `esp32-F4R0`, `esp32c3-F4R0`, `esp32c6-F4R0`, and `esp32c6-F8R0`; other targets still require compatible hardware and runtime activation.

## Quick Start

### Online Flashing

Open the [online flasher](https://fastbee.cn/doc/device/deploy/esp32-flasher.html), select a merged image that exactly matches the chip, Flash, and PSRAM, and flash it at address `0x0`.

### Build and Flash from Source

Install VS Code with PlatformIO, connect the board, and run from the repository root:

```powershell
# Build Web assets, upload LittleFS, and flash firmware
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env <env> -Port <serial-port>

# Build only
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env <env> -BuildOnly
```

Useful switches include `-Monitor`, `-SkipFs`, `-SkipFirmware`, and `-SkipWeb`. A release deployment must keep firmware and LittleFS assets in sync.

### First-Boot Configuration

1. On first boot, the device starts an AP. When `apSSID` is empty, the SSID is `<device-name>-<first-6-chip-id>`; a factory reset uses `fastbee-ap`. The default AP password is `admin123`.
2. Connect to the AP and open `http://192.168.4.1`.
3. Sign in with `admin / admin123` and change the password immediately.
4. Enter local WiFi credentials on **Network Settings**, save, and reconnect through the assigned STA address or `http://fastbee.local`.
5. Enable only the required features, then configure peripherals, MQTT/Modbus, and execution rules.

Do not commit real WiFi passwords, broker credentials, tokens, or production network settings. Inject local values during deployment.

## Feature Activation

`features.json` stores the complete user-requested feature state. Web service and mDNS are enabled by default; all optional features start disabled. A legacy sparse file is preserved and expanded once on first load. A feature is active only when the relevant conditions remain true:

```text
compiled
  AND userEnabled
  AND budgetPassed
  AND NOT runtimeDisabled
```

Features that require networking or another feature must also wait for network readiness and satisfy their dependency. MQTTS depends on MQTT, command scripts depend on rule scripts, and the GB2312 font depends on LCD support.

Core navigation such as **Device Config** remains visible. Optional entries including Device Control, Rule Scripts, Device Logs, and User Management are shown only while their corresponding feature is `active`. Inspect `/api/features/status` for compile/user state and `/api/system/status` for effective runtime state.

## Configuration Files

Runtime configuration lives under LittleFS `/config/`:

| File | Purpose |
| --- | --- |
| `device.json` | Device identity, NTP, timezone, and log level |
| `network.json` | WiFi AP/STA, static IP, an mDNS compatibility mirror, Ethernet, and cellular settings |
| `protocol.json` | MQTT, MQTTS, and Modbus RTU settings |
| `peripherals.json` | Peripheral types, pins, and parameters |
| `periph_exec.json` | Triggers, conditions, and actions |
| `features.json` | Complete requested runtime feature state and authoritative mDNS switch |
| `rule_scripts.json` | Optional rule scripts |
| `users.json` / `auth.json` | Users, password hashes, and authentication policy |

Prefer the Web configuration export/import workflow for backups.

The five sanitized bench examples, supported source formats, and deployment rules are maintained in [`docs/README.md`](docs/README.md) (Chinese). Do not interchange device configurations or publish the Git-ignored private backups.

`data/config/` is reserved for the factory template: no device identity, no STA/MQTT credentials, and no peripherals or rules enabled by default. It must not be used as a live-device backup; `npm test` enforces this boundary.

## Validation

```powershell
# Static checks, native tests, all 9 environments, and firmware artifacts
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks static,native,build,artifacts"

# Device API smoke and paced checks of 16 public pages plus 4 critical scripts
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-sta-ip> -Profile unified -RequireNetworkConnected
powershell -ExecutionPolicy Bypass -File scripts\device-web-smoke.ps1 -BaseUrl http://<device-sta-ip> -Rounds 2 -DelayMs 1000 -TimeoutSec 15 -MaxElapsedMs 5000

# Five-device bench: copy the example and replace its lab addresses and ports first
powershell -ExecutionPolicy Bypass -File scripts\run-device-fleet-tests.ps1 -DeviceConfig <local-fleet-config.json> -BrowserTests:$false
```

The default `test-all.ps1` run performs static/host checks, native tests, and all nine firmware builds. Device checks require an explicit STA URL and the `unified` profile. Host and artifact validation does not prove the LittleFS/configuration content installed on hardware, so verify filesystem data, migration, and reboot recovery separately.

## Project Structure

```text
src/              Firmware source
web-src/          Web frontend source
data/config/      LittleFS default configuration
data/www/         Generated gzip-only Web publish assets (do not edit directly)
partitions/       4MB, 8MB, and 16MB partition tables
scripts/          Build, deployment, and test tools
test/             Native, device API, and browser tests
```

## Documentation

- [Device documentation](https://fastbee.cn/doc/device/)
- [Quick start](https://fastbee.cn/doc/device/getting-started/quick-start.html)
- [User manual](https://fastbee.cn/doc/device/getting-started/user-manual.html)
- [Peripheral reference](https://fastbee.cn/doc/device/peripherals/)
- [MQTT and Modbus](https://fastbee.cn/doc/device/protocols/)
- [OTA upgrades](https://fastbee.cn/doc/device/deploy/ota-upgrade.html)
- [Build configuration](https://fastbee.cn/doc/device/dev/build-config.html)
- [Testing and release validation](https://fastbee.cn/doc/device/deploy/testing.html)

## License

FastBee-Arduino is licensed under [AGPL-3.0](LICENSE).
