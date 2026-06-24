---
title: Complete Sensor Guide
---

# Complete Sensor Guide

> This document is the **authoritative source** for sensor classification, containing detailed wiring and configuration for all supported sensors.

## Sensor Categories (sensorCategory)

| Category ID | Name | Included Sensors |
|---------|---------|----------|
| `temperature` | Temperature | DS18B20, BMP280, SHT31, AHT20 |
| `humidity` | Humidity | DHT11, DHT22, SHT31, AHT20 |
| `temperature_humidity` | Temp & Humidity | DHT11, DHT22 |
| `pressure` | Barometric | BMP280 |
| `light` | Light | BH1750 |
| `motion` | Motion/Attitude | MPU6050 |
| `rfid` | RFID | MFRC522 |
| `infrared` | Infrared | VS1838B |
| `rtc` | Real-Time Clock | DS1302 |
| `display` | Display | LCD1602, OLED |

## Detailed Sensor Descriptions

### DHT11 Temperature & Humidity Sensor

| Parameter | Value |
|------|-----|
| Interface | GPIO (OneWire) |
| Operating Voltage | 3.3V - 5V |
| Temperature Range | 0 - 50°C (±2°C) |
| Humidity Range | 20% - 90% (±5%) |
| Sampling Period | ≥1 second |

**Wiring**:
| DHT11 | ESP32 |
|-------|-------|
| VCC | 3.3V |
| GND | GND |
| DATA | GPIO 4 (configurable) |

> Recommend adding a 4.7K-10KΩ pull-up resistor from DATA to VCC.

### DHT22 Temperature & Humidity Sensor

| Parameter | Value |
|------|-----|
| Interface | GPIO (OneWire) |
| Operating Voltage | 3.3V - 5V |
| Temperature Range | -40 - 80°C (±0.5°C) |
| Humidity Range | 0% - 100% (±2%) |
| Sampling Period | ≥2 seconds |

**Wiring**: Same as DHT11

### DS18B20 Temperature Sensor

| Parameter | Value |
|------|-----|
| Interface | GPIO (OneWire) |
| Operating Voltage | 3.0V - 5.5V |
| Temperature Range | -55 - 125°C (±0.5°C) |
| Features | Multiple sensors on same bus, each with unique address |

**Wiring**:
| DS18B20 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| DATA | GPIO 4 (configurable) |

> DATA pin MUST have a 4.7KΩ pull-up resistor to VCC.

### BMP280 Barometric Pressure & Temperature Sensor

| Parameter | Value |
|------|-----|
| Interface | I2C (address 0x76 or 0x77) |
| Operating Voltage | 3.3V |
| Pressure Range | 300 - 1100 hPa |
| Temperature Range | -40 - 85°C |

**Wiring**:
| BMP280 | ESP32 |
|--------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

### BH1750 Light Sensor

| Parameter | Value |
|------|-----|
| Interface | I2C (address 0x23 or 0x5C) |
| Operating Voltage | 3.3V |
| Measurement Range | 1 - 65535 lx |

**Wiring**: Same as BMP280 (VCC/GND/SDA/SCL)

### MPU6050 6-Axis Sensor

| Parameter | Value |
|------|-----|
| Interface | I2C (address 0x68 or 0x69) |
| Operating Voltage | 3.3V |
| Measurement | 3-axis acceleration + 3-axis gyroscope |

**Wiring**:
| MPU6050 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| INT | (Optional) |

### MFRC522 RFID Module

| Parameter | Value |
|------|-----|
| Interface | SPI |
| Operating Voltage | 3.3V |
| Frequency | 13.56 MHz |
| Supported Tags | MIFARE Classic, Ultralight |

**Wiring**:
| MFRC522 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA(SS) | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |

### LCD1602 I2C Character LCD

| Parameter | Value |
|------|-----|
| Interface | I2C (PCF8574 backpack) |
| Operating Voltage | 5V |
| Display | 2 rows × 16 characters |

**Wiring**:
| LCD1602 I2C | ESP32 |
|-------------|-------|
| VCC | 5V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

## Configuration JSON Example

Sensor configuration is stored in `peripherals.json`, managed via Web interface or API.

```json
{
  "peripherals": [
    {
      "id": "dht22-001",
      "name": "Living Room Temp/Humidity",
      "type": 37,
      "pins": { "data": 4 },
      "config": {
        "sensorModel": "DHT22",
        "sensorCategory": "temperature_humidity",
        "readInterval": 60
      }
    },
    {
      "id": "bmp280-001",
      "name": "Outdoor Pressure",
      "type": 42,
      "pins": { "sda": 21, "scl": 22 },
      "config": {
        "i2cAddress": "0x76",
        "sensorModel": "BMP280",
        "sensorCategory": "pressure"
      }
    }
  ]
}
```

> For peripheral type enumeration and complete configuration reference, see [Peripheral Configuration](./README.md).
> For automation rule configuration, see [Peripheral Execution](../periph-exec/README.md).
