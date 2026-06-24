---
title: FastBee-Arduino 设备端文档
index: false
order: 1
---

# FastBee-Arduino 设备端文档

> 面向 ESP32 全系列的零代码 Web 物联网固件，烧录后通过浏览器即可完成配置。

## 快速导航

| 章节 | 说明 |
|------|------|
| [项目概述](./overview.md) | 项目背景、核心特性、应用场景 |
| [快速入门](./quick-start.md) | 5 步快速上手，从烧录到运行 |
| [版本选择](./edition-comparison.md) | Lite/Standard/Full 版本功能对比 |
| [用户手册](./user-manual.md) | 完整操作指南：网络、外设、规则、传感器 |
| [硬件选型](./hardware-equipment.md) | 推荐开发板、外设模块选购指南 |

## 部署与验证

| 章节 | 说明 |
|------|------|
| [在线烧录工具](./esp32-flasher.md) | 浏览器一键烧录固件 |
| [烧录与部署](./flashing-testing.md) | 烧录、部署、打包命令完整参考 |
| [测试验证](./testing.md) | 测试矩阵、冒烟测试、长稳测试 |
| [发布清单](./stability-release-checklist.md) | 版本发布前检查清单 |

## 开发参考

| 章节 | 说明 |
|------|------|
| [架构设计](./architecture.md) | 系统架构、模块设计、数据流 |
| [核心框架](./core-framework.md) | 类关系、初始化流程、关键API |
| [构建配置](./build-config.md) | PlatformIO 构建配置完整说明 |
| [资源优化](./resource-tuning.md) | 内存管理、功能裁剪、性能调优 |
| [开发指南](./development-guide.md) | 二次开发入门与最佳实践 |

## 外设与协议

| 章节 | 说明 |
|------|------|
| [外设配置](./peripherals/README.md) | 外设类型、引脚分配、驱动支持 |
| [传感器指南](./peripherals/sensor-guide-complete.md) | 传感器分类、接线、配置示例 |
| [外设执行](./periph-exec/README.md) | 触发器、动作类型、规则配置 |
| [协议概览](./protocols/README.md) | MQTT/Modbus/TCP/HTTP/CoAP 协议 |
