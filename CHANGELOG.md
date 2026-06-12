# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed
- Fixed PlatformIO environment names in `docs/development-guide.md` to match actual `platformio.ini` definitions.
- Fixed inconsistent `FASTBEE_ENABLE_IR_REMOTE` flag: Full edition now enables IR by default; ESP32-S3 environments explicitly disable it due to RMT driver conflict.
- Fixed date inconsistency in `docs/README.md`.

### Documentation
- Added missing test files to `test/README.md` directory structure (10 additional files).
- Updated test coverage table in `docs/development-guide.md` to include all 18 native test files.
- Added Doxygen file headers to core modules (`PeriphExecManager.cpp`, `PeriphExecExecutor.cpp`, `PeriphExecScheduler.cpp`).
- Updated `FastBeeFramework.cpp` header to standard Doxygen format.

### Code Quality
- Implemented hardware release logic for GPIO outputs and PWM/Servo peripherals in `PeripheralManager.cpp`, replacing the previous TODO placeholder.

## [4.0.0] - 2026-06-04

### Added
- Complete documentation system: 6 core docs + 50 examples + 25 peripheral manuals + 18 execution docs.
- Stability and release checklist (`docs/stability-release-checklist.md`).
- Unified test framework with 18 native test files covering Web API, MQTT, protocol handlers, security auth, system stability, and more.
- Device smoke and soak test scripts (`scripts/smoke-test-device.ps1`, `scripts/soak-test-device.ps1`).
- `test-all.ps1` unified test orchestrator with static checks, native tests, builds, artifacts, and device tests.
- i18n support (Chinese and English) for Web frontend.
- Rule script engine (`RuleScriptManager`, `ScriptEngine`) for Full edition.
- Peripheral execution engine with trigger-action model (platform/timer/event/poll triggers, 27 action types).
- Command script support for Standard and Full editions.
- OTA firmware and filesystem upgrade support (8MB+ Flash).
- Multi-user and role-based access control for Full edition.
- File manager and log viewer for Full edition.
- LittleFS-based configuration persistence with JSON format.
- Health monitor and memory guard with automatic degradation.
- Task manager for background operations.
- Restart diagnostics module for crash analysis.

### Changed
- Refactored monolithic `state.js` into 11 modular JavaScript modules.
- Web frontend assets compressed with Gzip and embedded into LittleFS.
- Modularized peripheral driver architecture with `DriverRegistry`.

### Fixed
- Multiple pagination boundary issues in API handlers.
- Memory leak in ArduinoJson v7 static `JsonDocument` usage; switched to local instances.
- WiFi/AP dual-mode switching and network fallback stability.
- MQTT reconnection logic with exponential backoff.

## [3.x.x] - 2025-12

### Added
- Initial FastBee-Arduino framework based on ESP32 Arduino core.
- Web-based configuration interface (`WebConfigManager`).
- MQTT client with FastBee platform integration.
- Basic GPIO, PWM, ADC, sensor (DHT11/22, DS18B20) support.
- NeoPixel / WS2812B LED strip support.
- OLED (SSD1306/SH1106) and TM1637 seven-segment display support.

### Changed
- Migrated build system to PlatformIO with multi-environment support.

## [2.x.x] - 2025-09

### Added
- Basic ESP32 WiFi and Web server functionality.
- JSON configuration file support.

## [1.0.0] - 2025-06

### Added
- Initial project scaffold.
- Basic LED blink and serial communication examples.
