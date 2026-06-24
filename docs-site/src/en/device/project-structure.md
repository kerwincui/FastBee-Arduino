---
title: Project Structure
order: 72
---

# Project Structure

> FastBee-Arduino code repository directory structure.

```
FastBee-Arduino/
├── include/                # Header files (90 .h)
│   ├── core/               # Core framework: FastBeeFramework, peripheral manager, execution engine
│   │   └── interfaces/     # Abstract interfaces: IAuthManager, INetworkManager, etc.
│   ├── network/            # Network layer: WiFi, Ethernet, 4G, DNS, IP management
│   │   └── handlers/       # Web route handlers (14 RouteHandlers)
│   ├── protocols/          # Protocol layer: MQTT, Modbus, TCP, HTTP, CoAP
│   ├── peripherals/        # Peripheral drivers: LCD, sensors, 7-segment display, RFID
│   │   └── drivers/        # Specific peripheral driver implementations
│   ├── security/           # Security module: authentication, user management, encryption
│   ├── systems/            # System services: health monitor, config storage, logging
│   └── utils/              # Utility library: strings, time, files, JSON
├── src/                    # Source files (73 .cpp)
│   ├── core/               # Core framework implementation
│   ├── network/            # Network + handlers implementation
│   ├── protocols/          # Protocol layer implementation
│   ├── peripherals/        # Peripheral drivers + driver implementations
│   ├── security/           # Security module implementation
│   ├── systems/            # System services implementation
│   └── utils/              # Utility library implementation
├── web-src/                # Web frontend source (66 files)
│   ├── css/                # Stylesheets
│   ├── js/                 # Core JS engine
│   ├── i18n/               # Internationalization
│   ├── modules/            # Page modules
│   └── pages/              # HTML page templates
├── test/                   # Unit tests (37 .cpp)
├── scripts/                # Build/deployment/test scripts (43)
├── partitions/             # Flash partition tables
├── data/                   # LittleFS default configuration
├── docs/                   # Project documentation
├── dist/                   # Release firmware artifacts
└── platformio.ini          # PlatformIO build configuration
```

## Key Files

| File | Description |
|------|------|
| `platformio.ini` | Build configuration, feature switches, environment definitions |
| `include/core/FeatureFlags.h` | Feature compilation switch defaults |
| `include/core/PeripheralTypes.h` | Peripheral type enum definitions |
| `include/core/ResourceProfile.h` | Resource limit definitions |
| `src/main.cpp` | Program entry point, PSRAM configuration |

> See [Architecture](./architecture.md) for architecture details.
> See [Build Configuration](./build-config.md) for build configuration details.
