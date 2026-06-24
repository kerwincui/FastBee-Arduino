---
title: 资源占用与功能裁剪
order: 76
---

# 资源占用与功能裁剪

> 本文档是 MEMGUARD（内存保护）详细参数和功能裁剪配置的**权威来源**。

## ESP32 内存背景

| 资源 | ESP32 | ESP32-S3 | ESP32-C3 | ESP32-C6 |
|------|-------|----------|----------|----------|
| 内部 SRAM | ~320KB | ~512KB | ~400KB | ~512KB |
| 可用堆（启动后） | ~220KB | ~310KB | ~140KB | ~220KB |
| PSRAM | 4-8MB | 2-8MB | 无 | 无 |

**基线内存占用**（WiFi + WebServer + MQTT 已启用）：**~130-180KB**

## HealthMonitor 四级内存保护 (MEMGUARD)

| 等级 | 条件 | 措施 | 恢复条件 |
|------|------|------|---------|
| **NORMAL** | freeHeap ≥ 20KB 且 largestBlock ≥ 12KB | 所有功能正常运行 | — |
| **WARN** | 10KB ≤ freeHeap < 20KB | 降低轮询频率、降低日志级别、暂停非关键任务 | freeHeap 恢复到 > 22KB 持续 30s |
| **SEVERE** | 6KB ≤ freeHeap < 10KB | 暂停 Modbus 轮询、MQTT 降采样、停止文件日志、释放传感器缓存 | freeHeap > 14KB 持续 60s |
| **CRITICAL** | freeHeap < 6KB | 禁用文件日志、拒绝大响应（>4KB）、仅保留关键页面、关闭 SSE 连接 | freeHeap > 10KB 持续 120s |

### DRAM 专项保护

- 每 5 秒检测 `MALLOC_CAP_INTERNAL` 空闲
- 连续 3 次（15 秒）低于 8KB → 触发 `ESP.restart()`
- 重启前通过 `RestartDiagnostics` 保存状态快照

## PSRAM 分配策略

```cpp
heap_caps_malloc_extmem_enable(512);  // ≥ 512B 的分配优先用 PSRAM
```

阈值设为 512（而非 4096）的原因：AsyncWebServer HTTP 请求缓冲区约 1-2KB，4096 阈值无法命中。

## 功能裁剪

### 裁剪原则

**设备不用的外设和协议就关闭**。不编译对应代码，不创建运行时对象。

详情请参阅 [构建配置](./build-config.md) 中的功能开关说明。

### 功能版本预设

| 预设 | 适用 | 特点 |
|------|------|------|
| Lite | C3/C6 4MB | 精简：MQTT + 基础外设 |
| Standard | ESP32 4-8MB | 标准：MQTT + Modbus + 规则脚本 |
| Full | S3/ESP32 8MB+ + PSRAM | 全功能：TCP/HTTP/CoAP/OTA/以太网/4G |

> 完整版本对比矩阵请参阅 [版本选择](./edition-comparison.md)。

## 典型配置方案

### 极致精简（C3 4MB 无 PSRAM）

关闭：Modbus、LCD、NeoPixel、I2C 传感器、RFID、红外、BLE
保留：MQTT、基础传感器 (DHT/DS18B20)、Web、GPIO

预估释放：~5KB RAM + ~60KB Flash

### 标准版（ESP32 4MB 无 PSRAM）

关闭：4G、以太网、BLE、未接的外设（LCD/RFID/红外）
保留：MQTT + Modbus + 基础外设 + Web

> 完整功能开关和 RAM 占用表请参考项目仓库 [`docs/feature-flags-ram-guide.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/feature-flags-ram-guide.md)。
