---
title: Protocol Overview
order: 30
---

# Communication Protocols

> FastBee-Arduino supports five communication protocols, managed uniformly by the ProtocolManager.

## Protocol Comparison

| Protocol | Use | Port | Direction | Suitable Scenarios |
|------|------|------|------|---------|
| **MQTT** | IoT messaging | 1883 (TCP) / 8883 (TLS) | Bidirectional | Cloud platform communication, remote control |
| **Modbus RTU** | Industrial device communication | (UART) | Master/Slave | PLCs, sensors, actuators |
| **TCP** | Raw TCP communication | 8080 (configurable) | Server | Custom protocols, transparent transmission |
| **HTTP** | Web requests | 80 | Client | Calling external APIs |
| **CoAP** | Constrained environment protocol | 5683 | Bidirectional | Low-power sensor networks |

## MQTT

MQTT is FastBee's core communication protocol for data exchange between devices and the IoT platform.

### Topic Templates

| Topic | Direction | Description |
|------|------|------|
| `/device/{deviceId}/status` | Uplink | Device online status |
| `/device/{deviceId}/sensor` | Uplink | Sensor data reporting |
| `/device/{deviceId}/event` | Uplink | Event reporting |
| `/device/{deviceId}/cmd` | Subscribe | Receive platform commands |

### Configuration Example

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

> For complete MQTT configuration, see [User Manual](../user-manual.md).

## Modbus RTU

Supports Modbus RTU master mode, can connect multiple Modbus slave devices.

### Functions

- Read Coils (FC 01)
- Read Discrete Inputs (FC 02)
- Read Holding Registers (FC 03)
- Read Input Registers (FC 04)
- Write Single Coil (FC 05)
- Write Single Register (FC 06)
- Write Multiple Coils (FC 15)
- Write Multiple Registers (FC 16)

### Typical Applications

- Connect to PLCs for reading/writing data
- Connect to Modbus sensors (temperature/humidity, pressure, flow, etc.)
- Connect to Modbus relay modules

> For Modbus configuration and sub-device management, see the Modbus sub-device section in [Peripheral Configuration](../peripherals/README.md).
