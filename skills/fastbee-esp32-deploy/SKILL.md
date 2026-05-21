---
name: fastbee-esp32-deploy
description: Stable deployment workflow for FastBee-Arduino ESP32, ESP32-C3, and ESP32-S3 PlatformIO projects. Use when Codex needs to deploy, flash firmware, upload LittleFS/Web filesystem images, generate slim or full Web staging packages, verify device access, or diagnose stale data/www versus .pio/fs-staging artifacts.
---

# FastBee ESP32 Deploy

## Overview

Use this skill to deploy FastBee-Arduino without mixing stale Web assets, firmware, and LittleFS images. Keep the editable Web source in `web-src/`, treat `data/www/` as generated compressed publish output, and upload the clean `.pio/fs-staging/run-*/www` image to the board.

## Deployment Rules

- Work from the repository root.
- For classic ESP32, default to the `esp32` environment.
- For ESP32-C3, use `esp32c3`; for ESP32-S3 slim builds, use `esp32s3`; use `esp32s3-full` only for full-feature S3 validation.
- Flash filesystem and firmware together unless the user explicitly asks for only one. Updating only one side commonly causes stale JS/CSS, missing APIs, blank pages, or login errors.
- Do not edit `data/www/` by hand. It contains generated publish artifacts; raw generated `.html`, `.js`, and `.css` files are ignored, while committed artifacts should be `.gz` files plus small static assets such as logo/favicon.
- Use `.pio/fs-staging/run-*/` as `PLATFORMIO_DATA_DIR` for LittleFS upload. The `www` folder under that run directory is the actual filesystem payload.
- Do not delete user configs or format LittleFS unless the user explicitly asks.

## Choose The Target

Use this mapping unless the user specifies otherwise:

- `esp32`: classic ESP32 production/slim target.
- `esp32c3`: ESP32-C3 slim target.
- `esp32s3`: ESP32-S3 slim target.
- `esp32s3-full`: ESP32-S3 full-feature validation target.

## Full Stable Deployment

1. Generate a clean slim staging image:

```powershell
node scripts/gzip-www.js --web-slim --no-upload --no-monitor
```

2. Read the output path:

```text
Staging package ready: D:\project\gitee\FastBee-Arduino\.pio\fs-staging\run-*\www
```

3. Upload LittleFS using the parent run directory, not `data/www`:

```powershell
$env:PLATFORMIO_DATA_DIR='D:\project\gitee\FastBee-Arduino\.pio\fs-staging\run-xxxx'
pio run -e esp32 --target uploadfs
```

4. Flash firmware for the same environment:

```powershell
pio run -e esp32 --target upload
```

5. Verify the Web boot markers and system API. Replace the IP when needed:

```powershell
curl.exe --noproxy "*" --max-time 20 --compressed -s http://192.168.5.85/ | Select-String -Pattern "__fastbeeBootReady|__fastbeeStartChunks|login-form"
curl.exe --noproxy "*" --max-time 10 -s http://192.168.5.85/api/system/status
```

## Common Shortcuts

Generate and upload only the slim filesystem:

```powershell
node scripts/gzip-www.js --web-slim --no-monitor
```

Build Web publish artifacts without uploading:

```powershell
node scripts/build-web-assets.js
node scripts/web-asset-report.js
```

Compile only:

```powershell
pio run -e esp32
```

Monitor serial output:

```powershell
pio device monitor -e esp32 -b 115200
```

## Troubleshooting

| Symptom | First check |
| --- | --- |
| Page style is broken or blank after login | Rebuild slim staging and upload both filesystem and firmware. |
| Old UI remains after deploy | Confirm `PLATFORMIO_DATA_DIR` points to `.pio/fs-staging/run-xxxx`, not `data`. |
| `data/www` has stale files or locked folders | Ignore raw leftovers; rebuild staging and use the generated run directory. |
| COM port is busy | Close serial monitor, browser serial tools, and other PlatformIO processes. |
| Web requests time out or reset | Check serial logs for reboot/low memory, then verify slim staging was uploaded. |
| `esp32s3-full` fails on classic ESP32 | Use `esp32`; full is only for resource-rich ESP32-S3 validation. |

## Report Back

When finishing a deployment task, include the environment, whether filesystem and firmware were both flashed, the staging path used, the key verification result, and any RAM/Flash usage reported by PlatformIO.
