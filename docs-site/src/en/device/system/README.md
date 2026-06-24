---
title: System Features
order: 20
---

# System Features Overview

> FastBee-Arduino Web management interface feature modules.

## Dashboard

The home dashboard displays device running status overview:
- Device information (name, ID, firmware version)
- Network status (WiFi/Ethernet/4G connection status, IP address, signal strength)
- Memory usage (total heap, free memory, fragmentation rate, PSRAM status)
- System uptime
- MQTT connection status

## Device Configuration

The **Device Configuration** page manages basic device parameters:
- Device name and ID
- NTP time server and timezone
- Log level settings (DEBUG/INFO/WARN/ERROR)
- Device restart

## Network Configuration

The **Network Configuration** page manages network access:
- WiFi STA (connect to router) — SSID scan/password/static IP
- WiFi AP (device hotspot) — SSID/password customization
- Ethernet (W5500) — SPI pin configuration
- 4G Cellular (EC801E) — APN/UART pin configuration
- mDNS domain name configuration
- DNS server settings

> See [User Manual](../user-manual.md) for network configuration steps.

## Peripheral Configuration

The **Peripheral Configuration** page manages hardware peripherals:
- Add/edit/delete peripherals
- Supported peripheral types — see [Peripheral Configuration](../peripherals/README.md)
- Real-time peripheral status display

## Peripheral Execution

The **Peripheral Execution** page configures automation rules:
- Create/edit/enable/disable rules
- Four trigger modes: Schedule/Event/MQTT/Condition
- Complete action list — see [Peripheral Execution](../periph-exec/README.md)

## Protocol Configuration

The **Protocol Configuration** page manages communication protocols:
- MQTT — server/port/authentication/topics
- Modbus RTU — baud rate/data bits/parity
- TCP — port/max connections
- HTTP/CoAP — configuration management

> See [Protocol Overview](../protocols/README.md) for protocol details.

## Other Features (Full Edition)

| Feature | Description |
|------|------|
| File Manager | Browse/upload/download/delete LittleFS files |
| Log Viewer | Real-time system log viewing |
| User Management | Add/edit/delete multiple users |
| Config Import/Export | Batch import/export all configurations |
| OTA Upgrade | Online firmware/filesystem upgrade |
