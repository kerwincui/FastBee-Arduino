---
title: 传感器完整指南
---

# 传感器完整指南

> 本文档是传感器分类的**权威来源**，包含所有支持传感器的详细接线和配置方案。

## 传感器分类 (sensorCategory)

| 分类标识 | 中文名称 | 包含传感器 |
|---------|---------|----------|
| `temperature` | 温度 | DS18B20, BMP280, SHT31, AHT20 |
| `humidity` | 湿度 | DHT11, DHT22, SHT31, AHT20 |
| `temperature_humidity` | 温湿度一体 | DHT11, DHT22 |
| `pressure` | 气压 | BMP280 |
| `light` | 光照 | BH1750 |
| `motion` | 运动/姿态 | MPU6050 |
| `rfid` | 射频识别 | MFRC522 |
| `infrared` | 红外 | VS1838B |
| `rtc` | 实时时钟 | DS1302 |
| `display` | 显示 | LCD1602, OLED |

## 各传感器详细说明

### DHT11 温湿度传感器

| 参数 | 值 |
|------|-----|
| 接口 | GPIO (OneWire) |
| 工作电压 | 3.3V - 5V |
| 温度范围 | 0 - 50°C (±2°C) |
| 湿度范围 | 20% - 90% (±5%) |
| 采样周期 | ≥1 秒 |

**接线**：
| DHT11 | ESP32 |
|-------|-------|
| VCC | 3.3V |
| GND | GND |
| DATA | GPIO 4（可配置） |

> 建议 DATA 引脚加 4.7K-10KΩ 上拉电阻到 VCC。

### DHT22 温湿度传感器

| 参数 | 值 |
|------|-----|
| 接口 | GPIO (OneWire) |
| 工作电压 | 3.3V - 5V |
| 温度范围 | -40 - 80°C (±0.5°C) |
| 湿度范围 | 0% - 100% (±2%) |
| 采样周期 | ≥2 秒 |

**接线**：同 DHT11

### DS18B20 温度传感器

| 参数 | 值 |
|------|-----|
| 接口 | GPIO (OneWire) |
| 工作电压 | 3.0V - 5.5V |
| 温度范围 | -55 - 125°C (±0.5°C) |
| 特点 | 可并联多个，每个有唯一地址 |

**接线**：
| DS18B20 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| DATA | GPIO 4（可配置） |

> DATA 引脚必须加 4.7KΩ 上拉电阻到 VCC。

### BMP280 气压温度传感器

| 参数 | 值 |
|------|-----|
| 接口 | I2C（地址 0x76 或 0x77） |
| 工作电压 | 3.3V |
| 气压范围 | 300 - 1100 hPa |
| 温度范围 | -40 - 85°C |

**接线**：
| BMP280 | ESP32 |
|--------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

### BH1750 光照传感器

| 参数 | 值 |
|------|-----|
| 接口 | I2C（地址 0x23 或 0x5C） |
| 工作电压 | 3.3V |
| 测量范围 | 1 - 65535 lx |

**接线**：同 BMP280（VCC/GND/SDA/SCL）

### MPU6050 六轴传感器

| 参数 | 值 |
|------|-----|
| 接口 | I2C（地址 0x68 或 0x69） |
| 工作电压 | 3.3V |
| 测量 | 3轴加速度 + 3轴陀螺仪 |

**接线**：
| MPU6050 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| INT | (可选) |

### MFRC522 RFID 模块

| 参数 | 值 |
|------|-----|
| 接口 | SPI |
| 工作电压 | 3.3V |
| 频率 | 13.56 MHz |
| 支持标签 | MIFARE Classic, Ultralight |

**接线**：
| MFRC522 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA(SS) | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |

### LCD1602 I2C 字符液晶

| 参数 | 值 |
|------|-----|
| 接口 | I2C（PCF8574 扩展板） |
| 工作电压 | 5V |
| 显示 | 2行 × 16字符 |

**接线**：
| LCD1602 I2C | ESP32 |
|-------------|-------|
| VCC | 5V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

## 配置 JSON 示例

传感器配置保存在 `peripherals.json` 中，通过 Web 界面或 API 管理。

```json
{
  "peripherals": [
    {
      "id": "dht22-001",
      "name": "客厅温湿度",
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
      "name": "室外气压",
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

> 外设类型枚举和外设配置完整参考请参阅 [外设配置](./README.md)。
> 自动化规则配置请参阅 [外设执行](../periph-exec/README.md)。
