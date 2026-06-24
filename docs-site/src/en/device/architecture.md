---
title: Architecture Design
order: 70
---

# FastBee-Arduino Architecture Design

> System architecture and module design overview. For complete architecture documentation, see the repository [`docs/architecture.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/architecture.md).

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                   Web Browser (Frontend)                  │
│  Login → Dashboard / Device / Network / Protocol /       │
│  Peripherals / Rules                                     │
└──────────────────┬──────────────────────────────────────┘
                   │ HTTP REST + SSE real-time push
┌──────────────────▼──────────────────────────────────────┐
│                AsyncWebServer (Port 80)                   │
│  ┌───────────────────────────────────────────────────┐  │
│  │          WebConfigManager (Thin Coordinator)       │  │
│  │  StaticHandler │ AuthHandler │ DeviceHandler ...   │  │
│  └───────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────┤
│              FastBeeFramework (Singleton Core)            │
│                                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ Network  │ │ Protocol │ │Peripheral│ │ Security │  │
│  │ Manager  │ │ Manager  │ │ Manager  │ │ Manager  │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
│                                                          │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐    │
│  │ PeriphExec   │ │   Health     │ │  Task        │    │
│  │ Manager      │ │   Monitor    │ │  Manager     │    │
│  └──────────────┘ └──────────────┘ └──────────────┘    │
└──────────────────────────────────────────────────────────┘
```

## Core Modules

### Initialization Flow

`FastBeeFramework::initialize()` executes in 9 sequential phases:

| Phase | Content | Output |
|------|------|------|
| 1 | Storage & Filesystem | ConfigStorage (NVS), LittleFS |
| 2 | Logging System | LOG macros available |
| 3 | Web Server | AsyncWebServer instance |
| 4 | Network Management | WiFi/Ethernet/4G |
| 5 | Security Module | Authentication, session management |
| 6 | Web Configuration | 14 RouteHandler registrations |
| 7 | OTA Management | Firmware/filesystem upgrades |
| 8 | System Services | Task scheduling, health monitor, peripherals |
| 9 | Protocol Layer | MQTT/Modbus/TCP/HTTP/CoAP |

### Memory Management

**HealthMonitor 4-Level Memory Protection**:

| Level | Condition | Measures |
|------|------|------|
| NORMAL | freeHeap ≥ 20KB | All features normal |
| WARN | 10KB ≤ freeHeap < 20KB | Reduce polling frequency |
| SEVERE | 6KB ≤ freeHeap < 10KB | Suspend Modbus, MQTT downsampling |
| CRITICAL | freeHeap < 6KB | Reject large responses, keep only critical pages |

> For detailed MEMGUARD mechanisms and parameters, see [Resource Tuning](./resource-tuning.md).

### Key Design Patterns

- **Singleton Pattern**: Core managers use Meyers' Singleton
- **Callback Decoupling**: Modules decoupled via `std::function` callbacks
- **Interface Abstraction**: `include/core/interfaces/` defines abstract interfaces
- **Conditional Compilation**: `#if FASTBEE_ENABLE_*` controls feature trimming

## Data Storage

All configuration is stored in the LittleFS `/config/` directory:

| File | Description |
|------|------|
| `device.json` | Device information |
| `network.json` | Network configuration |
| `protocol.json` | Protocol configuration |
| `peripherals.json` | Peripheral configuration |
| `exec_rules.json` | Execution rules |
| `users.json` | User accounts |

## API Endpoints

| Endpoint | Description |
|------|------|
| `/api/login` | User login |
| `/api/system/*` | System information |
| `/api/peripheral/*` | Peripheral CRUD |
| `/api/periph-exec/*` | Execution rule CRUD |
| `/api/protocol/*` | Protocol configuration |

## Related Documents

- [Core Framework](./core-framework.md) — Class relationships and key APIs
- [Project Structure](./project-structure.md) — Directory structure
- [Build Configuration](./build-config.md) — PlatformIO build configuration
- [Resource Tuning](./resource-tuning.md) — Memory management and feature trimming
- [Commercial License](./commercial-license.md) — Licensing terms
