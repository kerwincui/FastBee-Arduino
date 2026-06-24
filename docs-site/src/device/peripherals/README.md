---
title: 外设配置
order: 40
---

# 外设配置

> 本文档是外设类型、引脚分配原则的**权威来源**，其他文档通过链接引用此处。

## 外设类型枚举

FastBee-Arduino 支持以下完整外设类型体系（定义在 `include/core/PeripheralTypes.h`）：

### 通信接口类

| 类型 | 枚举值 | 说明 |
|------|--------|------|
| UART | `1` | 串口通信 |
| I2C | `2` | I2C 总线 |
| SPI | `3` | SPI 总线 |
| CAN | `4` | CAN 总线 |
| USB | `5-10` | USB 接口 |

### GPIO 接口类

| 类型 | 枚举值 | 说明 |
|------|--------|------|
| 数字输入 | `11` | 数字信号读取 |
| 数字输出 | `12` | 数字信号输出（继电器等） |
| PWM 输出 | `13` | 脉冲宽度调制（舵机、调光） |
| GPIO 中断 | `14` | 引脚变化中断 |
| 触摸输入 | `15` | ESP32 电容触摸 |
| 计数器 | `16-25` | 脉冲计数 |

### 模拟信号类

| 类型 | 枚举值 | 说明 |
|------|--------|------|
| ADC 输入 | `26` | 模拟数字转换 |
| DAC 输出 | `27-30` | 数字模拟转换 |

### 专用外设类

| 类型 | 枚举值 | 说明 |
|------|--------|------|
| LCD 显示 | `36` | OLED/LCD 屏幕（U8g2） |
| 传感器 | `37` | DHT11/DHT22/DS18B20 |
| NeoPixel | `38` | WS2812 LED 灯带 |
| LED 点阵 | `39` | WS2812B 点阵屏 |
| 数码管 | `40` | TM1637 七段数码管 |
| RFID | `41` | MFRC522 射频识别 |
| I2C 传感器 | `42` | BMP280/MPU6050 等 |
| 红外遥控 | `43` | 红外收发 |
| DS1302 | `44` | 实时时钟 |
| LCD1602 | `45` | 字符液晶 |
| Modbus 子设备 | `50-59` | Modbus RTU 从站设备 |
| DEVICE_EVENT | `60` | 虚拟事件源（无物理引脚） |

## 已支持传感器

| 传感器 | 型号 | 接口 | 驱动 | 测量参数 |
|--------|------|------|------|---------|
| 温湿度 | DHT11 | GPIO (OneWire) | SensorDriver | 温度 0-50°C, 湿度 20-90% |
| 温湿度 | DHT22 | GPIO (OneWire) | SensorDriver | 温度 -40-80°C, 湿度 0-100% |
| 温度 | DS18B20 | GPIO (OneWire) | SensorDriver | 温度 -55-125°C |
| 温度 | BMP280 | I2C | BMP280Driver | 温度 + 气压 |
| 温湿度 | SHT31 | I2C | SHT31Driver | 温度 + 湿度 |
| 温湿度 | AHT20 | I2C | AHT20Driver | 温度 + 湿度 |
| 光照 | BH1750 | I2C | BH1750Driver | 光照强度 1-65535 lx |
| 姿态 | MPU6050 | I2C | MPU6050Driver | 3轴加速度 + 3轴陀螺仪 |
| RFID | MFRC522 | SPI | RFIDDriver | 13.56MHz 卡片识别 |
| 红外 | VS1838B | GPIO | IRRemoteDriver | 红外遥控码 |
| 时钟 | DS1302 | GPIO (3线) | DS1302Driver | 年月日时分秒 |
| 显示 | LCD1602 | I2C (PCF8574) | LCD1602Driver | 2×16 字符液晶 |

> 完整传感器分类、接线图和配置示例请参阅 [传感器完整指南](./sensor-guide-complete.md)。

## 引脚分配原则

### 基本原则

1. **避免冲突**：不同外设不能共用同一 GPIO 引脚
2. **注意特殊引脚**：某些引脚在启动时有特殊功能（如 GPIO 0 用于烧录模式）
3. **ADC2 限制**：WiFi 启用时 ADC2 不可用（GPIO 0, 2, 4, 12-15, 25-27），仅使用 ADC1（GPIO 32-39）
4. **I2C 默认引脚**：SDA=21, SCL=22（ESP32）；SDA=41, SCL=42（ESP32-S3）
5. **SPI 默认引脚**：MISO=19, MOSI=23, SCK=18, SS=5（可配置）
6. **UART 默认引脚**：UART0 用于调试日志，UART1/2 可用于 Modbus 等

### 各芯片引脚差异

| 特性 | ESP32 | ESP32-S3 | ESP32-C3 | ESP32-C6 |
|------|-------|----------|----------|----------|
| GPIO 总数 | 34 | 45 | 22 | 30 |
| ADC 通道 | 18 | 20 | 6 | 7 |
| 触摸传感器 | 10 | 14 | 无 | 无 |
| DAC 通道 | 2 | 无 | 无 | 无 |
| 默认 I2C | 21,22 | 41,42 | 4,5 | 6,7 |

## 配置示例

### GPIO 输出（继电器）

```json
{
  "name": "水泵继电器",
  "type": 12,
  "pins": {
    "gpio": 16
  },
  "config": {
    "initialState": 0,
    "description": "控制水泵开关"
  }
}
```

### I2C 传感器（BMP280）

```json
{
  "name": "气压传感器",
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

### NeoPixel 灯带

```json
{
  "name": "氛围灯带",
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

> 更多配置示例请参阅 [示例教程](../examples/README.md)。
