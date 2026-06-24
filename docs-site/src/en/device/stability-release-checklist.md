---
title: Release Checklist
order: 13
---

# Version Release Checklist

> Complete verification checklist before releasing a new version.

## 1. Compilation Verification

- [ ] All target chip environments compile successfully
  - [ ] `esp32-F4R0` (Standard)
  - [ ] `esp32-F8R4` (Full)
  - [ ] `esp32c3-F4R0` (Lite)
  - [ ] `esp32c6-F4R0` (Lite)
  - [ ] `esp32s3-F8R0` (Standard+OTA)
  - [ ] `esp32s3-F8R4` (Full)
  - [ ] `esp32s3-F16R8` (Full)
- [ ] No compilation warnings (or only known exemptions)
- [ ] Flash/RAM usage within safe limits

## 2. Static Checks

- [ ] `pio check` passes
- [ ] Frontend asset integrity check passes (`web-smoke-test.js`)
- [ ] i18n completeness check passes (`validate-i18n.js`)
- [ ] Document link check passes (`validate-doc-links.js`)

## 3. Unit Tests

- [ ] `pio test -e native` all pass
- [ ] New features have corresponding test coverage
- [ ] Regression tests pass

## 4. On-Device Physical Testing

- [ ] WiFi AP mode: connectable, loggable, configurable
- [ ] WiFi STA mode: connects to router, accessible via IP
- [ ] Ethernet/4G (Full edition): connects normally
- [ ] MQTT connect/disconnect/reconnect
- [ ] Peripheral add/edit/delete
- [ ] Peripheral execution rules (Schedule/MQTT/Condition/Event)
- [ ] OTA upgrade (Full edition)
- [ ] Configuration import/export
- [ ] Long-term stability (≥ 2 hours no reboot)

## 5. Key Metrics

| Metric | Threshold | Status |
|------|------|------|
| Free heap memory | > 20KB (no PSRAM) | [ ] |
| Fragmentation rate | < 30% | [ ] |
| Longest Free Block | > 12KB | [ ] |
| TCP connections (steady state) | ≤ budget | [ ] |
| API P95 latency | < 2s | [ ] |

## 6. Documentation

- [ ] Version numbers updated in docs
- [ ] New feature documentation added
- [ ] Changelog updated
- [ ] Chinese and English docs synced

## 7. Release Artifacts

- [ ] All version `factory.bin` files generated under `dist/firmware/`
- [ ] Online flasher can load firmware normally
- [ ] GitHub Release page created

> For testing details, see [Testing](./testing.md).
