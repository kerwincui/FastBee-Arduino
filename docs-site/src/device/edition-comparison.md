---
title: 版本选择
order: 4
---

# 版本选择与功能对比

> 根据您的硬件配置和应用需求，选择最合适的固件版本。

## 版本概览

FastBee-Arduino 提供三个功能级别，通过 PlatformIO 环境名进行选择：

| 版本 | 适用芯片 | Flash 要求 | PSRAM | 定位 |
|------|---------|-----------|-------|------|
| **Lite** | ESP32-C3, ESP32-C6 | 4MB | 无 | 精简节点 |
| **Standard** | ESP32, ESP32-S3 | 4-8MB | 无 | 标准网关 |
| **Full** | ESP32, ESP32-S3 | 8MB+ | 4MB+ | 全功能终端 |

## 功能对比矩阵

### 网络接入

| 功能 | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| WiFi STA + AP | ✅ | ✅ | ✅ |
| mDNS 域名 | ✅ | ✅ | ✅ |
| 以太网 (W5500) | ❌ | ❌ | ✅ |
| 4G 蜂窝 (EC801E) | ❌ | ❌ | ✅ |
| BLE 蓝牙 | ❌ | ❌ | ❌ |

### 通信协议

| 功能 | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| MQTT | ✅ | ✅ | ✅ |
| Modbus RTU 主站 | ❌ | ✅ | ✅ |
| Modbus 从站 | ❌ | ❌ | ✅ |
| TCP 服务器 | ❌ | ❌ | ✅ |
| HTTP 客户端 | ❌ | ❌ | ✅ |
| CoAP | ❌ | ❌ | ✅ |

### 外设支持

| 功能 | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| GPIO 输入/输出 | ✅ | ✅ | ✅ |
| DHT11/DHT22 传感器 | ✅ | ✅ | ✅ |
| DS18B20 温度传感器 | ✅ | ✅ | ✅ |
| OLED/LCD 显示屏 | ✅ | ✅ | ✅ |
| NeoPixel LED | ✅ | ✅ | ✅ |
| 七段数码管 | ✅ | ✅ | ✅ |
| I2C 高级传感器 | ❌ | ✅ | ✅ |
| RFID 读卡器 | ❌ | ✅ | ✅ |
| 红外遥控 | ❌ | ✅ | ✅ |
| LED 点阵屏 | ❌ | ✅ | ✅ |
| DS1302 实时时钟 | ❌ | ❌ | ✅ |
| LCD1602 字符液晶 | ❌ | ❌ | ✅ |

### 系统功能

| 功能 | Lite | Standard | Full |
|------|:----:|:--------:|:----:|
| Web 管理界面 | ✅ | ✅ | ✅ |
| 外设执行规则 | ✅ | ✅ | ✅ |
| 规则脚本引擎 | ❌ | ✅ | ✅ |
| 命令脚本 | ❌ | ✅ | ✅ |
| OTA 固件升级 | ❌ | ⚠️ | ✅ |
| 多用户管理 | ❌ | ❌ | ✅ |
| 文件管理器 | ❌ | ❌ | ✅ |
| 日志查看器 | ❌ | ❌ | ✅ |
| 文件日志 | ❌ | ❌ | ✅ |
| 国际化 i18n | ❌ | ❌ | ✅ |

## 构建环境映射

| PlatformIO 环境 | 芯片 | Flash | PSRAM | 功能级别 | 分区表 |
|----------------|------|-------|-------|---------|--------|
| `esp32c3-F4R0` | ESP32-C3 | 4MB | 无 | Lite | `fastbee.csv` |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | 无 | Lite | `fastbee.csv` |
| `esp32-F4R0` | ESP32 | 4MB | 无 | Standard | `fastbee.csv` |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | 无 | Standard+OTA | `fastbee-8MB.csv` |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | Full | `fastbee-8MB.csv` |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | Full | `fastbee-8MB.csv` |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | Full | `fastbee-16MB.csv` |

### 分区表说明

| 分区文件 | Flash | App 槽位 | OTA | LittleFS |
|---------|-------|---------|-----|----------|
| `fastbee.csv` | 4MB | 2.88MB × 1 | 否 | 1MB |
| `fastbee-8MB.csv` | 8MB | 3.5MB × 2 | 是 | 960KB |
| `fastbee-16MB.csv` | 16MB | 4MB × 2 | 是 | 7.9MB |

## 如何选择

1. **首先确认硬件**：查看您的 ESP32 模块的 Flash 和 PSRAM 大小
2. **确认功能需求**：是否需要 Modbus、OTA、以太网、4G 等
3. **参考上表选择**：匹配对应的 PlatformIO 环境名

> 烧录和部署命令请参阅 [烧录与部署](./flashing-testing.md)。
