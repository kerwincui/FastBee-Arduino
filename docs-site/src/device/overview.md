---
title: 项目概述
order: 2
---

# FastBee-Arduino 项目概述

> 版本：1.0.0 | 芯片：ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 | 框架：Arduino-ESP32 3.x (IDF 5.1)

## 项目简介

FastBee-Arduino 是面向 ESP32 全系列的**零代码 Web 物联网固件**。烧录后通过浏览器即可完成网络、设备、协议、外设和规则配置，无需二次编程。

**核心定位**：ESP32 节点、轻量网关和现场采集控制终端。

## 核心特性

- **零代码配置**：全部配置通过 Web 浏览器完成，无需编程
- **多协议支持**：MQTT / Modbus RTU / TCP / HTTP / CoAP
- **多网络接入**：WiFi (AP+STA) / 以太网 (W5500) / 4G (EC801E)
- **外设丰富**：GPIO、LCD、传感器、继电器、NeoPixel、RFID 等
- **规则引擎**：定时触发、事件触发、MQTT 触发、条件触发
- **远程管理**：OTA 固件升级、实时监控、配置导入导出
- **安全认证**：多用户管理、会话管理、密码加密

## 技术栈

- 固件：C++ / Arduino-ESP32 3.x / ESP-IDF 5.1 / PlatformIO
- Web 前端：原生 JavaScript（无框架依赖）/ CSS / HTML
- 通信协议：MQTT / Modbus RTU / TCP / HTTP / CoAP
- 网络接入：WiFi (AP+STA) / 以太网 (W5500) / 4G (EC801E)
- 存储：LittleFS + NVS (Preferences)

## 应用场景

- 智能家居：灯光控制、环境监测、安防报警
- 工业物联网：设备监控、数据采集、远程控制
- 智慧农业：温室大棚、灌溉控制、气象监测
- 教育科研：物联网教学、原型验证、毕业设计

## 支持的硬件

| 芯片 | Flash | PSRAM | 功能级别 |
|------|-------|-------|---------|
| ESP32 | 4-16MB | 0-4MB | Standard / Full |
| ESP32-S3 | 8-16MB | 0-8MB | Standard+OTA / Full |
| ESP32-C3 | 4MB | 无 | Lite |
| ESP32-C6 | 4MB | 无 | Lite |

> 更多硬件详情请参阅 [硬件选型](./hardware-equipment.md) 和 [版本选择](./edition-comparison.md)。

## 快速开始

1. 使用 [在线烧录工具](./esp32-flasher.md) 或 [PlatformIO](./flashing-testing.md) 将固件写入 ESP32
2. 连接设备 WiFi 热点 `FastBee-XXXX`，密码 `fastbee123`
3. 浏览器访问 `http://192.168.4.1` 进入管理界面
4. 配置 WiFi 网络和 MQTT 服务器连接

> 详细步骤请参考 [快速入门](./quick-start.md)。

## 相关链接

- GitHub 仓库：[https://github.com/kerwincui/FastBee-Arduino](https://github.com/kerwincui/FastBee-Arduino)
- FastBee 物联网平台：[http://fastbee.cn/](http://fastbee.cn/)
