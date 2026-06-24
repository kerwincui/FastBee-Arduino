---
title: Peripheral Configuration
order: 40
---

# Peripheral Configuration

> This document is the **authoritative source** for peripheral types and pin assignment principles. Other documents reference this page via links.

## Peripheral Type Enumeration

FastBee-Arduino supports the following complete peripheral type system (defined in `include/core/PeripheralTypes.h`):

### Communication Interfaces

| Type | Enum Value | Description |
|------|--------|------|
| UART | `1` | Serial communication |
| I2C | `2` | I2C bus |
| SPI | `3` | SPI bus |
| CAN | `4` | CAN bus |
| USB | `5-10` | USB interface |

### GPIO Interfaces

| Type | Enum Value | Description |
|------|--------|------|
| Digital Input | `11` | Digital signal reading |
| Digital Output | `12` | Digital signal output (relays, etc.) |
| PWM Output | `13` | Pulse width modulation (servos, dimming) |
| GPIO Interrupt | `14` | Pin change interrupt |
| Touch Input | `15` | ESP32 capacitive touch |
| Counter | `16-25` | Pulse counting |

### Analog Signals

| Type | Enum Value | Description |
|------|--------|------|
| ADC Input | `26` | Analog-to-digital conversion |
| DAC Output | `27-30` | Digital-to-analog conversion |

### Specialized Peripherals

| Type | Enum Value | Description |
|------|--------|------|
| LCD Display | `36` | OLED/LCD screen (U8g2) |
| Sensor | `37` | DHT11/DHT22/DS18B20 |
| NeoPixel | `38` | WS2812 LED strip |
| LED Matrix | `39` | WS2812B matrix display |
| 7-Segment Display | `40` | TM1637 seven-segment display |
| RFID | `41` | MFRC522 RFID reader |
| I2C Sensor | `42` | BMP280/MPU6050, etc. |
| IR Remote | `43` | Infrared transceiver |
| DS1302 | `44` | Real-time clock |
| LCD1602 | `45` | Character LCD |
| Modbus Sub-device | `50-59` | Modbus RTU slave devices |
| DEVICE_EVENT | `60` | Virtual event source (no physical pins) |

## Supported Sensors

| Sensor | Model | Interface | Driver | Measurement |
|--------|------|------|------|---------|
| Temp/Humidity | DHT11 | GPIO (OneWire) | SensorDriver | Temp 0-50°C, Humidity 20-90% |
| Temp/Humidity | DHT22 | GPIO (OneWire) | SensorDriver | Temp -40-80°C, Humidity 0-100% |
| Temperature | DS18B20 | GPIO (OneWire) | SensorDriver | Temp -55-125°C |
| Temperature | BMP280 | I2C | BMP280Driver | Temperature + Pressure |
| Temp/Humidity | SHT31 | I2C | SHT31Driver | Temperature + Humidity |
| Temp/Humidity | AHT20 | I2C | AHT20Driver | Temperature + Humidity |
| Light | BH1750 | I2C | BH1750Driver | Light intensity 1-65535 lx |
| Motion | MPU6050 | I2C | MPU6050Driver | 3-axis accel + 3-axis gyro |
| RFID | MFRC522 | SPI | RFIDDriver | 13.56MHz card detection |
| IR | VS1838B | GPIO | IRRemoteDriver | IR remote codes |
| RTC | DS1302 | GPIO (3-wire) | DS1302Driver | Year-month-day hour:min:sec |
| Display | LCD1602 | I2C (PCF8574) | LCD1602Driver | 2×16 character LCD |

> For complete sensor classification, wiring diagrams, and configuration examples, see [Sensor Guide](./sensor-guide-complete.md).

## Pin Assignment Principles

### Basic Principles

1. **Avoid Conflicts**: Different peripherals cannot share the same GPIO pin
2. **Beware Special Pins**: Some pins have special functions at boot (e.g., GPIO 0 for flash mode)
3. **ADC2 Limitations**: ADC2 is unavailable when WiFi is enabled (GPIO 0, 2, 4, 12-15, 25-27), use only ADC1 (GPIO 32-39)
4. **I2C Default Pins**: SDA=21, SCL=22 (ESP32); SDA=41, SCL=42 (ESP32-S3)
5. **SPI Default Pins**: MISO=19, MOSI=23, SCK=18, SS=5 (configurable)
6. **UART Default Pins**: UART0 for debug logs, UART1/2 available for Modbus, etc.

### Chip-Specific Pin Differences

| Feature | ESP32 | ESP32-S3 | ESP32-C3 | ESP32-C6 |
|------|-------|----------|----------|----------|
| GPIO Count | 34 | 45 | 22 | 30 |
| ADC Channels | 18 | 20 | 6 | 7 |
| Touch Sensors | 10 | 14 | None | None |
| DAC Channels | 2 | None | None | None |
| Default I2C | 21,22 | 41,42 | 4,5 | 6,7 |

## Configuration Examples

### GPIO Output (Relay)

```json
{
  "name": "Water Pump Relay",
  "type": 12,
  "pins": {
    "gpio": 16
  },
  "config": {
    "initialState": 0,
    "description": "Controls water pump on/off"
  }
}
```

### I2C Sensor (BMP280)

```json
{
  "name": "Pressure Sensor",
  "type": 42,
  "pins": {
    "sda": 21,
    "scl": 22
  },
  "config": {
    "i2cAddress": "0x76",
    "sensorModel": "BMP280"
  }
}
```

### NeoPixel Strip

```json
{
  "name": "Ambient Light Strip",
  "type": 38,
  "pins": {
    "data": 14
  },
  "config": {
    "ledCount": 30,
    "brightness": 128
  }
}
```

> For more configuration examples, see [Examples](../examples/README.md).
