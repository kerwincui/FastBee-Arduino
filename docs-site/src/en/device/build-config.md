---
title: Build Configuration
order: 75
---

# PlatformIO Build Configuration

> For complete build configuration, see the repository [`docs/platformio-config.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/platformio-config.md).

## Configuration Structure

```
platformio.ini
├── [esp32_base]           ← Common config (platform, framework, libraries)
├── [lite_flags]            ← Lite edition feature switches
├── [standard_flags]        ← Standard edition feature switches
├── [full_flags]            ← Full edition feature switches
├── [lite_src_filter]       ← Source filtering
├── [xxx_runtime_flags]     ← Runtime parameters (TCP/stack size)
└── [env:xxx]               ← Per-chip environment config
```

## Build Environments

| Environment | Chip | Flash | PSRAM | Edition |
|------|------|-------|-------|------|
| `esp32-F4R0` | ESP32 | 4MB | None | Standard |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | Full |
| `esp32c3-F4R0` | ESP32-C3 | 4MB | None | Lite |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | None | Standard+OTA |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | None | Lite |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | Full |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | Full |

## Runtime Parameters

| Parameter | ESP32 | C3 | C6 | S3 |
|------|:-----:|:--:|:--:|:--:|
| TCP Max Connections | 6 | 4 | 6 | 14 |
| Loop Stack | 16KB | 12KB | 12KB | 16KB |
| Script Stack | 8KB | 6KB | 6KB | 8KB |

> For TCP connection configuration details, see [TCP Connection Budget](./tcp-connection-budget.md).

## Feature Switches

All features are controlled via `-DFASTBEE_ENABLE_XXX=1/0`. For edition feature comparison, see [Edition Comparison](./edition-comparison.md).

## Source Filtering

`build_src_filter` excludes unnecessary .cpp files by edition:
- **Lite** excludes: OTA, user/role, log viewer, TCP/HTTP/CoAP
- **Standard** excludes: user/role, log viewer, TCP/HTTP/CoAP

> For complete feature switches and resource configuration, see [Resource Tuning](./resource-tuning.md).
