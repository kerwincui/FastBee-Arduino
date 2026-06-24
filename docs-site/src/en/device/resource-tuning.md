---
title: Resource Usage & Feature Trimming
order: 76
---

# Resource Usage & Feature Trimming

> This document is the **authoritative source** for MEMGUARD (memory protection) detailed parameters and feature trimming configuration.

## ESP32 Memory Background

| Resource | ESP32 | ESP32-S3 | ESP32-C3 | ESP32-C6 |
|------|-------|----------|----------|----------|
| Internal SRAM | ~320KB | ~512KB | ~400KB | ~512KB |
| Available Heap (after boot) | ~220KB | ~310KB | ~140KB | ~220KB |
| PSRAM | 4-8MB | 2-8MB | None | None |

**Baseline memory usage** (WiFi + WebServer + MQTT enabled): **~130-180KB**

## HealthMonitor 4-Level Memory Protection (MEMGUARD)

| Level | Condition | Measures | Recovery Condition |
|------|------|------|---------|
| **NORMAL** | freeHeap ≥ 20KB and largestBlock ≥ 12KB | All features operating normally | — |
| **WARN** | 10KB ≤ freeHeap < 20KB | Reduce polling frequency, lower log level, suspend non-critical tasks | freeHeap recovers to > 22KB for 30s |
| **SEVERE** | 6KB ≤ freeHeap < 10KB | Suspend Modbus polling, MQTT downsampling, stop file logging, release sensor caches | freeHeap > 14KB for 60s |
| **CRITICAL** | freeHeap < 6KB | Disable file logging, reject large responses (>4KB), keep only critical pages, close SSE connections | freeHeap > 10KB for 120s |

### Dedicated DRAM Protection

- Check `MALLOC_CAP_INTERNAL` free every 5 seconds
- 3 consecutive times (15 seconds) below 8KB → trigger `ESP.restart()`
- Save state snapshot via `RestartDiagnostics` before restart

## PSRAM Allocation Strategy

```cpp
heap_caps_malloc_extmem_enable(512);  // Allocations ≥ 512B prefer PSRAM
```

Threshold set to 512 (not 4096) because: AsyncWebServer HTTP request buffers are approximately 1-2KB; a 4096 threshold would not catch them.

## Feature Trimming

### Trimming Principles

**Disable unused peripherals and protocols on the device.** Don't compile unused code, don't create unused runtime objects.

For details, see the feature switch description in [Build Configuration](./build-config.md).

### Feature Edition Presets

| Preset | Applicable | Characteristics |
|------|------|------|
| Lite | C3/C6 4MB | Minimal: MQTT + basic peripherals |
| Standard | ESP32 4-8MB | Standard: MQTT + Modbus + rule scripts |
| Full | S3/ESP32 8MB+ + PSRAM | Full: TCP/HTTP/CoAP/OTA/Ethernet/4G |

> For complete edition comparison matrix, see [Edition Comparison](./edition-comparison.md).

## Typical Configuration Schemes

### Ultra Minimal (C3 4MB No PSRAM)

Disable: Modbus, LCD, NeoPixel, I2C sensors, RFID, IR, BLE
Keep: MQTT, basic sensors (DHT/DS18B20), Web, GPIO

Estimated recovery: ~5KB RAM + ~60KB Flash

### Standard (ESP32 4MB No PSRAM)

Disable: 4G, Ethernet, BLE, unused peripherals (LCD/RFID/IR)
Keep: MQTT + Modbus + basic peripherals + Web

> For complete feature switches and RAM usage table, see [`docs/feature-flags-ram-guide.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/feature-flags-ram-guide.md).
