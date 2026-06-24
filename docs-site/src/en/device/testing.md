---
title: Testing
order: 12
---

# Testing & Verification

> FastBee-Arduino complete testing system and command reference.

## Testing System Overview

The project uses PlatformIO native environment + Unity test framework, with 37 test files covering core functional modules.

## Running Tests

```bash
# Run local native unit tests
pio test -e native

# Run specific test cases
pio test -e native -f test_modbus

# Show detailed test output
pio test -e native -v

# Skip compilation and run tests directly
pio test -e native --no-auto-build
```

## Complete Check Matrix

```powershell
# Run all checks via test-all.ps1
.\scripts\test-all.ps1 -Checks static,native,build,artifacts

# Run specific checks only
.\scripts\test-all.ps1 -Checks static       # Static analysis
.\scripts\test-all.ps1 -Checks native       # Unit tests
.\scripts\test-all.ps1 -Checks build        # Multi-chip compilation verification
.\scripts\test-all.ps1 -Checks artifacts    # Web static asset check
```

## On-Device Online Testing

### Smoke Tests

Run API end-to-end smoke tests on flashed devices:

```powershell
# AP mode device
.\scripts\smoke-test-device.ps1 -BaseUrl http://192.168.4.1 -Profile standard

# STA mode device
.\scripts\smoke-test-device.ps1 -BaseUrl http://192.168.1.100 -Profile full
```

### Long-Term Stability Tests

```powershell
# 100-round soak test
.\scripts\soak-test-device.ps1 -BaseUrl http://device-ip -Profile full -Rounds 100

# Custom stability thresholds
.\scripts\soak-test-device.ps1 -BaseUrl http://device-ip -Rounds 60 `
  -MinHeapFreeBytes 20000 -MaxHeapFreeDropBytes 5000
```

Key metrics: heap memory decline trend, PSRAM usage trend, uptime reset count, API P95 latency.

## Test Categories

| Test Category | File | Coverage |
|---------|------|---------|
| Core | `test_main.cpp` | Unified entry point |
| Peripheral Config | `test_periph_config.cpp` | Peripheral CRUD, type validation |
| Peripheral Execution | `test_periph_exec.cpp` | Execution rules, triggers, actions |
| MQTT Protocol | `test_mqtt_protocol.cpp` | MQTT connection, topics, messages |
| Network Config | `test_network_config.cpp` | WiFi/Ethernet/4G configuration |
| End-to-End | `test_e2e_scenarios.cpp` | Multi-module collaboration scenarios |
| Regression Guard | `test_regression_guard.cpp` | Regression test guard |
| Health Monitor | `test_health_monitor.cpp` | Memory protection levels |
| System Stability | `test_system_stability.cpp` | Long-running stability |
| Web API | `test_web_api.cpp` | REST API endpoints |
| Modbus | `test_modbus_handler.cpp` | Modbus master/slave |

> For flashing and deployment commands, see [Flashing & Deployment](./flashing-testing.md).
> For pre-release checklist, see [Release Checklist](./stability-release-checklist.md).
