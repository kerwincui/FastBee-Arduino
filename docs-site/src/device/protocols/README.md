---
title: 协议概览
order: 30
---

# 通信协议

> FastBee-Arduino 支持五种通信协议，通过 ProtocolManager 统一管理。

## 协议对比

| 协议 | 用途 | 端口 | 方向 | 适用场景 |
|------|------|------|------|---------|
| **MQTT** | 物联网消息传输 | 1883 (TCP) / 8883 (TLS) | 双向 | 与云平台通信、远程控制 |
| **Modbus RTU** | 工业设备通信 | (UART) | 主站/从站 | PLC、传感器、执行器 |
| **TCP** | 原始 TCP 通信 | 8080 (可配置) | 服务器 | 自定义协议、透传 |
| **HTTP** | Web 请求 | 80 | 客户端 | 调用外部 API |
| **CoAP** | 受限环境协议 | 5683 | 双向 | 低功耗传感器网络 |

## MQTT

MQTT 是 FastBee 的核心通信协议，用于设备与物联网平台之间的数据交互。

### 主题模板

| 主题 | 方向 | 说明 |
|------|------|------|
| `/device/{deviceId}/status` | 上报 | 设备在线状态 |
| `/device/{deviceId}/sensor` | 上报 | 传感器数据上报 |
| `/device/{deviceId}/event` | 上报 | 事件上报 |
| `/device/{deviceId}/cmd` | 订阅 | 接收平台指令 |

### 配置示例

```json
{
  "mqtt": {
    "enabled": true,
    "server": "iot.fastbee.cn",
    "port": 1883,
    "clientId": "FB-001",
    "username": "device",
    "password": "password",
    "keepAlive": 60
  }
}
```

> 完整 MQTT 配置说明请参阅 [用户手册](../user-manual.md)。

## Modbus RTU

支持 Modbus RTU 主站模式，可连接多个 Modbus 从站设备。

### 功能

- 读取线圈 (FC 01)
- 读取离散输入 (FC 02)
- 读取保持寄存器 (FC 03)
- 读取输入寄存器 (FC 04)
- 写单个线圈 (FC 05)
- 写单个寄存器 (FC 06)
- 写多个线圈 (FC 15)
- 写多个寄存器 (FC 16)

### 典型应用

- 连接 PLC 读取/写入数据
- 连接 Modbus 传感器（温湿度、压力、流量等）
- 连接 Modbus 继电器模块

> Modbus 配置和子设备管理请参阅 [外设配置](../peripherals/README.md) 中的 Modbus 子设备部分。
