---
title: Hardware Selection
order: 6
---

# Hardware Selection Guide

> Recommended ESP32 dev boards and common peripheral module purchasing advice.

## Recommended Dev Boards

### ESP32 Standard

| Model | Chip | Flash | PSRAM | Price (Ref) | Recommended Use |
|------|------|-------|-------|---------|---------|
| ESP32 DevKitC | ESP32-WROOM-32 | 4MB | None | $3-5 | Getting started |
| NodeMCU-32S | ESP32-WROOM-32 | 4MB | None | $2-4 | General projects |
| ESP32-WROOM-32E | ESP32 | 8MB | 4MB | $5-7 | Mid-range applications |

### ESP32-S3 High Performance

| Model | Chip | Flash | PSRAM | Price (Ref) | Recommended Use |
|------|------|-------|-------|---------|---------|
| ESP32-S3-DevKitC | ESP32-S3-WROOM-1 | 8MB | None | $5-7 | Standard gateway |
| ESP32-S3-WROOM-1-N16R8 | ESP32-S3 | 16MB | 8MB | $7-10 | Full-featured terminal |

### ESP32-C3 Compact

| Model | Chip | Flash | Price (Ref) | Recommended Use |
|------|------|-------|---------|---------|
| ESP32-C3-DevKitM | ESP32-C3-MINI-1 | 4MB | $2-4 | Lightweight node |
| ESP32-C3 SuperMini | ESP32-C3 | 4MB | $1-2 | Smallest form factor |

## Peripheral Module Recommendations

### Sensors

| Module | Model | Interface | Price (Ref) | Use |
|------|------|------|---------|------|
| Temp/Humidity | DHT22 / AM2302 | GPIO | $1-2 | Environment monitoring |
| Temperature | DS18B20 | OneWire | $0.5-1 | High-precision temp |
| Barometric | BMP280 | I2C | $1-2 | Weather station |
| Light | BH1750 | I2C | $1-2 | Brightness detection |
| Motion | MPU6050 | I2C | $1-2 | Motion detection |
| RFID | MFRC522 | SPI | $1-2 | Access cards |
| IR | VS1838B | GPIO | $0.5-1 | Remote control receiver |

### Actuators

| Module | Model | Interface | Price (Ref) | Use |
|------|------|------|---------|------|
| Relay | SRD-05VDC | GPIO | $1-2 | Switch control |
| LED Strip | WS2812B | GPIO | $2-5/m | Ambient lighting |
| Servo | SG90 | PWM | $1-2 | Angle control |
| 7-Segment | TM1637 | GPIO (2-wire) | $1-2 | Numeric display |

### Communication Modules

| Module | Model | Interface | Price (Ref) | Use |
|------|------|------|---------|------|
| Ethernet | W5500 | SPI | $3-5 | Wired network |
| 4G | EC801E-CN | UART | $6-10 | Cellular network |
| RS485 | MAX485 | UART | $0.5-1 | Modbus communication |

### Display Modules

| Module | Model | Interface | Price (Ref) | Use |
|------|------|------|---------|------|
| OLED | SSD1306 0.96" | I2C | $2-3 | Status display |
| LCD1602 | PCF8574 I2C | I2C | $2-4 | Character display |

> For complete peripheral support list and pin wiring, see [Peripheral Configuration](./peripherals/README.md).
> For detailed sensor information, see [Sensor Guide](./peripherals/sensor-guide-complete.md).
