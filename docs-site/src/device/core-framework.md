---
title: 核心框架
order: 71
---

# 核心框架

> FastBeeFramework 核心类关系和关键 API 概览。

## 类关系

```
FastBeeFramework (单例)
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
└── WebConfigManager (14 RouteHandler)
```

## 核心类

| 类 | 职责 | 行数 |
|----|------|------|
| `FastBeeFramework` | 核心框架单例，9 阶段初始化 + 主循环 | 1465 |
| `PeripheralManager` | 外设 CRUD、硬件初始化、GPIO 兼容层 | 3466 |
| `PeriphExecManager` | 执行规则 CRUD、事件/条件触发回调 | 2352 |
| `PeriphExecExecutor` | 规则执行引擎（GPIO/显示/延时/通信动作） | 1807 |
| `PeriphExecScheduler` | 定时器调度、Cron 表达式解析 | 958 |

> 外设类型枚举完整列表请参阅 [外设配置](./peripherals/README.md)。
> 触发器与动作类型完整列表请参阅 [外设执行](./periph-exec/README.md)。

## 初始化流程

```
setup() → FastBeeFramework::initialize()
  ├── 1. initStorageAndFS()       → NVS + LittleFS
  ├── 2. initLogger()             → LOG 宏可用
  ├── 3. initWebServer()          → AsyncWebServer 创建
  ├── 4. initNetwork()            → WiFi/以太网/4G
  ├── 5. initSecurity()           → 用户认证
  ├── 6. initWebConfig()          → 14 个 RouteHandler
  ├── 7. initOTA()                → OTA 管理
  ├── 8. initSystems()            → 任务调度、健康监控
  └── 9. initProtocols()          → MQTT/Modbus/TCP/HTTP/CoAP

loop() → framework->run()
  ├── healthMonitor->update()     → 内存保护
  ├── protocolManager->handle()   → 协议循环
  ├── network->update()           → 网络状态
  └── taskManager->update()       → 定时任务
```

## 条件编译

所有功能通过 `FASTBEE_ENABLE_*` 宏控制，详见 [构建配置](./build-config.md) 和 [资源优化](./resource-tuning.md)：

```cpp
#if FASTBEE_ENABLE_MQTT
    MQTTClient* getMQTTClient() const { return mqttClient.get(); }
#else
    MQTTClient* getMQTTClient() const { return nullptr; }
#endif
```

## 测试覆盖

测试文件覆盖核心类：

| 测试 | 覆盖 |
|------|------|
| `test_periph_config.cpp` | 外设 CRUD、类型验证 |
| `test_periph_exec.cpp` | 执行规则、触发器、动作 |
| `test_mqtt_protocol.cpp` | MQTT 连接、消息 |
| `test_network_config.cpp` | 网络配置 |

> 完整测试列表请参阅 [测试验证](./testing.md)。
