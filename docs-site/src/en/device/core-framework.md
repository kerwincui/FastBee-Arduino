---
title: Core Framework
order: 71
---

# Core Framework

> FastBeeFramework core class relationships and key API overview.

## Class Relationships

```
FastBeeFramework (Singleton)
├── ConfigStorage (NVS + LittleFS)
├── FBNetworkManager
│   ├── WiFiManager
│   ├── EthernetAdapter
│   ├── CellularAdapter
│   ├── DNSManager
│   └── IPManager
├── ProtocolManager
│   ├── MQTTClient
│   ├── ModbusHandler
│   ├── TCPHandler
│   ├── HTTPClientWrapper
│   └── CoAPHandler
├── PeripheralManager
│   └── SensorDriver / LCDManager / ...
├── PeriphExecManager
│   ├── PeriphExecScheduler
│   ├── PeriphExecExecutor
│   └── PeriphExecWorkerPool
├── HealthMonitor
├── TaskManager
├── Security (AuthManager + UserManager)
└── WebConfigManager (14 RouteHandlers)
```

## Core Classes

| Class | Responsibility | Lines |
|----|------|------|
| `FastBeeFramework` | Core framework singleton, 9-phase init + main loop | 1465 |
| `PeripheralManager` | Peripheral CRUD, hardware init, GPIO compatibility layer | 3466 |
| `PeriphExecManager` | Execution rule CRUD, event/condition trigger callbacks | 2352 |
| `PeriphExecExecutor` | Rule execution engine (GPIO/display/delay/comm actions) | 1807 |
| `PeriphExecScheduler` | Timer scheduling, Cron expression parsing | 958 |

> For the complete peripheral type enum list, see [Peripheral Configuration](./peripherals/README.md).
> For the complete trigger and action type list, see [Peripheral Execution](./periph-exec/README.md).

## Initialization Flow

```
setup() → FastBeeFramework::initialize()
  ├── 1. initStorageAndFS()       → NVS + LittleFS
  ├── 2. initLogger()             → LOG macros available
  ├── 3. initWebServer()          → AsyncWebServer created
  ├── 4. initNetwork()            → WiFi/Ethernet/4G
  ├── 5. initSecurity()           → User authentication
  ├── 6. initWebConfig()          → 14 RouteHandlers
  ├── 7. initOTA()                → OTA management
  ├── 8. initSystems()            → Task scheduling, health monitor
  └── 9. initProtocols()          → MQTT/Modbus/TCP/HTTP/CoAP

loop() → framework->run()
  ├── healthMonitor->update()     → Memory protection
  ├── protocolManager->handle()   → Protocol loops
  ├── network->update()           → Network status
  └── taskManager->update()       → Timed tasks
```

## Conditional Compilation

All features are controlled via `FASTBEE_ENABLE_*` macros. See [Build Configuration](./build-config.md) and [Resource Tuning](./resource-tuning.md):

```cpp
#if FASTBEE_ENABLE_MQTT
    MQTTClient* getMQTTClient() const { return mqttClient.get(); }
#else
    MQTTClient* getMQTTClient() const { return nullptr; }
#endif
```

## Test Coverage

Test files covering core classes:

| Test | Coverage |
|------|------|
| `test_periph_config.cpp` | Peripheral CRUD, type validation |
| `test_periph_exec.cpp` | Execution rules, triggers, actions |
| `test_mqtt_protocol.cpp` | MQTT connection, messages |
| `test_network_config.cpp` | Network configuration |

> For complete test list, see [Testing](./testing.md).
