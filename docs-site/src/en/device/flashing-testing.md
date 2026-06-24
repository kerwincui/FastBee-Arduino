---
title: Flashing & Deployment
order: 11
---

# Flashing & Deployment

> Complete PlatformIO command-line reference for flashing, packaging, and deployment.

## Build Environment Selection

First, select the appropriate PlatformIO environment for your hardware. See [Edition Comparison](./edition-comparison.md) for the complete environment mapping table.

## Flash Firmware

```bash
# Build and flash (ESP32 example)
pio run -e esp32-F4R0 -t upload

# Specify serial port
pio run -e esp32-F4R0 -t upload --upload-port COM3

# Build + flash + serial monitor
pio run -e esp32-F4R0 -t upload -t monitor
```

## Upload Filesystem

```bash
# Build and upload LittleFS image
pio run -e esp32-F4R0 -t uploadfs

# Specify serial port
pio run -e esp32-F4R0 -t uploadfs --upload-port COM3
```

## Batch Build

```bash
# Build multiple environments sequentially
pio run -e esp32-F4R0
pio run -e esp32c3-F4R0
pio run -e esp32s3-F8R4
pio run -e esp32s3-F16R8
```

## One-Click Deployment Script

The project provides PowerShell deployment scripts:

```powershell
# Full deployment (compile + upload LittleFS + flash firmware)
.\scripts\deploy.ps1 -Environment esp32s3-F8R4 -Port COM3

# Build only
.\scripts\deploy.ps1 -Environment esp32s3-F8R4 -BuildOnly

# Generate all-version merged firmware
.\scripts\build-all-artifacts.ps1
```

## Build Artifacts

After compilation, firmware archives are in `dist/firmware/{env}/`:

```
dist/firmware/{env}/
├── factory.bin     # Full firmware (bootloader + partition table)
├── firmware.bin    # Application firmware
└── partitions.bin  # Partition table
```

## Environment Diagnostics

```bash
# Run environment diagnostics
.\scripts\doctor.ps1

# View project configuration
pio project config

# View environment build flags
pio run -e esp32-F4R0 --target envdump
```

## Verification Scripts

| Script | Description |
|------|------|
| `validate-build-matrix.js` | Full-chip compilation verification |
| `validate-config-defaults.js` | Configuration default validation |
| `validate-i18n.js` | i18n completeness check |
| `validate-doc-links.js` | Document link validation |
| `validate-test-coverage.js` | Test coverage check |
| `web-smoke-test.js` | Web static asset smoke check |

> For testing commands, see [Testing](./testing.md).
