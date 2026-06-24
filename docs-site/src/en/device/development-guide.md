---
title: Development Guide
order: 73
---

# Development Guide

> Getting started with FastBee-Arduino secondary development.

## Environment Setup

### Required Software

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO IDE Extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
- [Git](https://git-scm.com/)
- Python 3.x (required by build scripts)

### Clone the Project

```bash
git clone https://github.com/kerwincui/FastBee-Arduino.git
cd FastBee-Arduino
```

### Install Dependencies

PlatformIO automatically downloads required libraries on first compilation:

```bash
pio pkg install
```

## Development Workflow

### 1. Understand the Architecture

Read [Architecture](./architecture.md) and [Core Framework](./core-framework.md) to understand overall system design.

### 2. Modify Code

Follow the development conventions in [Code Change Guidelines](./code-change-guidelines.md).

### 3. Build & Verify

```bash
# Build target environment
pio run -e esp32-F4R0

# Run unit tests
pio test -e native
```

### 4. Flash & Test

```bash
# Flash to device
pio run -e esp32-F4R0 -t upload

# Serial monitor
pio device monitor -e esp32-F4R0
```

## Adding New Features

### Adding a New Peripheral

1. Define peripheral type in `include/core/PeripheralTypes.h`
2. Create driver files in `include/peripherals/drivers/` + `src/peripherals/drivers/`
3. Add initialization logic in `PeripheralManager`
4. Add `FASTBEE_ENABLE_*` compilation switch
5. Configure defaults in `platformio.ini`
6. Record resource usage in `docs/feature-flags-ram-guide.md`

### Adding a New Protocol

1. Implement the `IProtocol` interface
2. Register in `ProtocolManager`
3. Add compilation switch and configuration

## Common Commands

| Command | Description |
|------|------|
| `pio run -e {env}` | Build specified environment |
| `pio run -e {env} -t upload` | Flash firmware |
| `pio run -e {env} -t uploadfs` | Upload filesystem |
| `pio test -e native` | Run tests |
| `pio device monitor` | Serial monitor |
| `.\scripts\test-all.ps1 -Checks build` | Full build verification |
| `.\scripts\doctor.ps1` | Environment diagnostics |

> For complete build commands, see [Flashing & Deployment](./flashing-testing.md).
> For build configuration details, see [Build Configuration](./build-config.md).
