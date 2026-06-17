# MQTT 内存与堆阈值指南

## 概述

FastBee 设备的 MQTT 客户端（`MQTTClient.cpp`）和协议管理器（`ProtocolManager.cpp`）在资源受限的 ESP32 环境中运行，
需要精细的堆内存保护策略，避免 JSON 序列化或 TLS 握手时 OOM 导致 `abort()`。

本文档说明各堆阈值的设定依据、HTTPS 降级策略，以及 PSRAM 对 MQTT 稳定性的影响。

---

## 堆内存阈值一览

| 检查点 | 文件 | 旧阈值 | 当前阈值 | 说明 |
|--------|------|--------|----------|------|
| MQTT `doReconnect()` 入口 | `MQTTClient.cpp` | 49152 (48KB) | **15000 (15KB)** | 堆低于此值跳过重连 |
| MQTT `restartMQTTDeferred()` | `ProtocolManager.cpp` | 25000 (25KB) | **15000 (15KB)** | 堆低于此值跳过客户端重建 |
| Modbus `restartModbus()` | `ProtocolManager.cpp` | 25000 (25KB) | **15000 (15KB)** | 堆低于此值跳过 Modbus 重启 |
| `ProtocolManager::handle()` 堆保护 | `ProtocolManager.cpp` | 30000 (30KB) | **30000（不变）** | 堆低于此值跳过重协议处理 |

---

## 为什么降低阈值？

### ESP32-S3 实际堆状况

WiFi + MQTT + AsyncWebServer 全开后，设备堆内存典型值：

| 状态 | 堆空闲 | 说明 |
|------|--------|------|
| 启动后稳态 | 22-35 KB | PSRAM 阈值=512 后，HTTP 缓冲卸载到 PSRAM |
| 页面加载峰值 | 18-25 KB | 多个 HTTP 并发请求消耗内部 DRAM |
| Web 空闲 | 30-40 KB | 无活跃 HTTP 请求 |

**旧阈值 49152 (48KB)** 在设备运行期间几乎永远无法满足，导致 MQTT 永远无法建立/重连连接。
**当前阈值 15000 (15KB)** 允许在正常负载下执行重连，同时防止在真正内存紧张时强行分配导致 `abort()`。

### MQTT 连接实际内存需求

| 操作 | 典型内存消耗 |
|------|------------|
| `PubSubClient::connect()` | ~2-4 KB（内部缓冲区） |
| MQTT TLS 握手（如启用） | ~8-16 KB（WiFiClientSecure） |
| `publishDeviceInfo()` JSON 序列化 | ~1-3 KB（ArduinoJson String） |
| 命令队列处理 | ~500B-2 KB |

> **结论**：MQTT 普通连接（非 TLS）需要约 4-6 KB 空闲堆，15KB 阈值留有 2-3x 安全余量。

---

## ProtocolManager handle() 策略

### 旧逻辑（问题）

```cpp
// 旧: 堆低于 30KB 时全部跳过，包括 MQTT handle()
if (freeHeap < 30000) {
    return;  // MQTT 状态维护也被跳过，永远无法重连
}
mqttClient->handle();
modbusHandler->handle();
```

设备堆常在 25-35 KB 波动，30KB 阈值导致 MQTT `handle()` 长时间无法运行，
无法执行后台重连调度，MQTT 连接永远建立不起来。

### 当前逻辑

```cpp
// 新: MQTT handle() 始终运行，其余重型处理受堆保护
bool heapSufficient = (freeHeap >= 30000);

// MQTT handle() 始终运行：内部有堆保护（doReconnect 检查 15KB）
if (mqttClient) mqttClient->handle();

// 堆不足时跳过 Modbus/规则脚本等重型处理
if (!heapSufficient) return;
modbusHandler->handle();
// ...
```

MQTT `handle()` 本身是轻量操作（检查连接状态、调度重连），不会触发大分配。
真正的内存消耗在 `doReconnect()` 中，该函数内部独立检查 15KB 阈值。

---

## NTP HTTPS → HTTP 降级

### 背景

`MQTTClient::getNtpTime()` 和 `TimeUtils::syncNTPFromHTTPWithTimestamp()` 向 FastBee 云端发起 NTP 时间同步请求。
NTP 接口是**只读公开接口**，返回时间戳，无需 TLS 加密。

### 问题

`WiFiClientSecure` 在 ESP32-S3 上分配 SSL 上下文需要 **8-16 KB 连续堆块**，
在堆碎片化严重时分配失败 → `abort()`。

### 当前策略

```cpp
// NTP 时间同步无需加密，强制降级 HTTPS → HTTP 避免 SSL 内存分配失败
if (url.startsWith("https://")) {
    url = "http://" + url.substring(8);
}

HTTPClient http;
static WiFiClient plainClient;
http.begin(plainClient, url);  // 使用 WiFiClient 而非 WiFiClientSecure
```

影响范围：
- `src/protocols/MQTTClient.cpp` — `getNtpTime()`
- `src/network/handlers/MqttRouteHandler.cpp` — `fetchNtpTimeForMqttTest()`
- `src/utils/TimeUtils.cpp` — `syncNTPFromHTTPWithTimestamp()`

---

## 启动流程：MQTT 客户端初始化

`FastBeeFramework::initialize()` 中，MQTT 客户端在 `ProtocolManager` 初始化后立即创建：

```
步骤11: ProtocolManager 初始化
步骤11.1: restartMQTTDeferred() — 加载配置并创建 MQTT 客户端
步骤11.2: restartModbus() — 创建 Modbus 处理器（如有）
```

> 旧逻辑中 `ProtocolManager::initialize()` 只设置标志位，不创建客户端，
> 依赖后续 `handle()` 中的懒初始化。但在堆保护阈值过高的情况下，`handle()` 永远无法执行懒初始化，
> 导致 MQTT 客户端始终未创建。新逻辑显式在启动时调用 `restartMQTTDeferred()`。

---

## 调试日志

在需要诊断 MQTT 连接问题时，在 `platformio.ini` 对应环境添加：

```ini
; 临时开启，诊断 MQTT 连接问题
-DCORE_DEBUG_LEVEL=3
-DFASTBEE_STRIP_INFO_LOGS=0
-DFASTBEE_DEBUG_LOG=1
-DFASTBEE_VERBOSE_ERROR=1
```

运行时关键日志标记：

| 日志标记 | 含义 |
|----------|------|
| `[BOOT] PSRAM malloc enabled (threshold=512)` | PSRAM 阈值已设置 |
| `[STEP11.1] Starting MQTT client...` | MQTT 客户端初始化开始 |
| `[MQTT] doReconnect: heap too low` | 堆不足，跳过重连 |
| `[MQTT] doReconnect: starting` | 重连开始 |
| `[PROTO] MQTT handle alive` | MQTT handle() 正常运行 |
| `[LOOP-ALIVE]` | 主 loop 周期确认（每15秒） |

---

## 关键文件

| 文件 | 说明 |
|------|------|
| `src/main.cpp` | PSRAM 阈值设置（`heap_caps_malloc_extmem_enable(512)`） |
| `src/core/FastBeeFramework.cpp` | 启动流程，MQTT/Modbus 显式初始化 |
| `src/protocols/MQTTClient.cpp` | MQTT 客户端，doReconnect 堆保护，NTP HTTP 降级 |
| `src/protocols/ProtocolManager.cpp` | handle() 策略，MQTT 始终运行 |
| `src/utils/TimeUtils.cpp` | NTP 时间同步，HTTP 降级 |
| `platformio.ini` | 各环境 TCP/PSRAM/调试标志 |
