# 通信协议文档

> **概述**: FastBee 支持 MQTT、Modbus RTU、TCP、HTTP、CoAP 等多种通信协议,实现数据上报和命令下发。MQTT 用于云平台对接,支持 TLS 认证和自动重连;Modbus RTU 作为主站轮询从站设备;所有协议通过 ProtocolManager 统一管理,支持多种网络传输层(WiFi/以太网/4G/LoRa)。

FastBee-Arduino 支持的通信协议配置指南。

## 文档列表

| 文档 | 说明 |
|------|------|
| [MQTT 配置](mqtt-config.md) | MQTT 服务器连接、主题格式、认证方式、重连机制 |
| [Modbus RTU](modbus-rtu.md) | 串口配置、从站扫描、寄存器映射、轮询策略 |

## 协议概述

### MQTT

FastBee 设备通过 MQTT 协议与云平台/私有服务器通信，支持：
- TLS/非TLS 连接
- 自定义主题前缀
- 遗嘱消息（LWT）
- 自动重连与指数退避

#### 多网络传输支持

MQTT 通信支持多种网络传输层，通过统一的 Arduino Client 接口实现：

| 传输方式 | 底层 Client | 构建要求 |
|---------|--------------|------|
| WiFi | WiFiClient | 所有构建 |
| 以太网 | WiFiClient（ETH 兼容层） | `esp32s3-full` |
| 4G 蜂窝 | TinyGsmClient | `esp32s3-full` |
| LoRa 透传 | LoRaClient（自实现） | `esp32s3-full` |

切换网络方式时，MQTT 会自动使用对应的 Client 实例，无需手动配置。

### Modbus RTU

FastBee 支持作为 Modbus RTU Master，通过串口连接多个从站设备：
- 支持 UART0/1/2 端口配置
- 自动从站扫描与发现
- 寄存器批量轮询
- 读取结果映射到外设通道

## 相关文档

- [网络配置](../system/network-config.md) — WiFi、以太网、4G、LoRa 网络设置
- [Modbus 设备外设](../peripherals/modbus-device.md) — Modbus 外设类型配置
- [Modbus 使用指南](../modbus_usage_guide.md) — 详细 Modbus 使用教程
