# FastBee-Arduino 外设配置完整指南

> 本指南整合了所有外设配置相关内容,包括外设类型、引脚分配、参数配置、Web配置步骤和联动示例。
>
> 选模块前先看：[支持的模块和传感器清单](supported-sensors-and-modules.md)。

## 目录

- [外设概述](#外设概述)
- [GPIO与PWM外设](#1-gpio与pwm外设)
- [电机控制外设](#2-电机控制外设)
- [显示设备](#3-显示设备)
- [温湿度传感器](#4-温湿度传感器)
- [环境传感器](#5-环境传感器)
- [电气传感器](#6-电气传感器)
- [通信设备](#7-通信设备)
- [其他外设](#8-其他外设)
- [引脚分配原则](#引脚分配原则)
- [Web配置流程](#web配置流程)
- [常见问题](#常见问题)

---

## 外设概述

FastBee支持25+种外设类型,通过Web界面可视化配置,无需编写代码。

### 外设类型速查表

| type值 | 类型名 | 说明 | 详细文档 |
|--------|--------|------|---------|
| 11 | GPIO_DIGITAL_INPUT | 数字输入 | [gpio-input.md](gpio-input.md) |
| 12 | GPIO_DIGITAL_OUTPUT | 数字输出 | [gpio-output.md](gpio-output.md) |
| 13 | GPIO_DIGITAL_INPUT_PULLUP | 上拉输入(按键) | [gpio-input.md](gpio-input.md) |
| 15 | GPIO_ANALOG_INPUT | ADC模拟输入 | [adc-input.md](adc-input.md) |
| 17 | GPIO_PWM_OUTPUT | PWM输出 | [pwm-output.md](pwm-output.md) |
| 36 | LCD | 显示屏(SSD1306/SH1106) | [display-oled.md](display-oled.md) |
| 38 | SENSOR | 传感器(DHT/DS18B20等) | 见各传感器文档 |
| 41 | PWM_SERVO | 舵机 | [servo.md](servo.md) |
| 42 | STEPPER_MOTOR | 步进电机 | [stepper-motor.md](stepper-motor.md) |
| 43 | ENCODER | 旋转编码器 | [encoder.md](encoder.md) |
| 44 | ONE_WIRE | 单总线(DS18B20) | [sensor-ds18b20.md](sensor-ds18b20.md) |
| 45 | NEO_PIXEL | WS2812B RGB灯带 | [neopixel.md](neopixel.md) |
| 47 | SEVEN_SEGMENT_TM1637 | TM1637数码管 | [display-tm1637.md](display-tm1637.md) |
| 51 | MODBUS_DEVICE | Modbus从站设备 | [modbus-device.md](modbus-device.md) |

---

## 1. GPIO与PWM外设

### 1.1 GPIO输出 (type:12)
**适用**: LED、继电器、蜂鸣器、激光传感器

**详细文档**: [gpio-output.md](gpio-output.md)

**JSON配置**:
```json
{
  "id": "led_01",
  "name": "LED指示灯",
  "type": 12,
  "enabled": true,
  "pinCount": 1,
  "pins": [26, 255, 255, 255, 255, 255, 255, 255],
  "params": {
    "initialState": 0
  }
}
```

### 1.2 GPIO输入 (type:11/13/14)
**适用**: 按键、PIR人体红外、震动传感器、干簧管

**详细文档**: [gpio-input.md](gpio-input.md)

### 1.3 ADC输入 (type:15/26)
**适用**: 光敏、土壤湿度、烟雾、雨滴、电压、电流传感器

**详细文档**: [adc-input.md](adc-input.md)

### 1.4 PWM输出 (type:17)
**适用**: LED调光、电机调速、舵机控制

**详细文档**: [pwm-output.md](pwm-output.md)

---

## 2. 电机控制外设

### 2.1 舵机 (type:41)
**适用**: SG90、MG996R舵机角度控制

**详细文档**: [servo.md](servo.md)

### 2.2 步进电机 (type:42)
**适用**: 28BYJ-48步进电机精确定位

**详细文档**: [stepper-motor.md](stepper-motor.md)

---

## 3. 显示设备

### 3.1 OLED显示 (type:36)
**适用**: SSD1306、SH1106 OLED屏幕

**详细文档**: [display-oled.md](display-oled.md)

**特性**:
- I2C接口,地址0x3C或0x3D
- 支持文字、数字、图形显示
- 可显示传感器数据、系统状态

### 3.2 TM1637数码管 (type:47)
**适用**: 4位7段数码管显示

**详细文档**: [display-tm1637.md](display-tm1637.md)

**特性**:
- CLK/DIO两线控制
- 支持数字、温度、湿度显示
- 可轮换显示多个参数

### 3.3 LCD1602 (type:36,占位)
**状态**: 驱动待扩展

**详细文档**: [display-lcd1602.md](display-lcd1602.md)

---

## 4. 温湿度传感器

### 4.1 DHT11/DHT22 (type:38)
**详细文档**: [sensor-dht.md](sensor-dht.md)

**特性**:
- 单总线通信
- DHT11: 温度0-50°C,湿度20-90%
- DHT22: 温度-40-80°C,湿度0-100%
- 采样间隔≥2秒

### 4.2 DS18B20 (type:38/44)
**详细文档**: [sensor-ds18b20.md](sensor-ds18b20.md)

**特性**:
- OneWire单总线
- 温度-55-125°C
- 可并联多个传感器(地址区分)

### 4.3 AHT20 (type:38)
**详细文档**: [sensor-aht20.md](sensor-aht20.md)

**特性**:
- I2C接口,地址0x38
- 温度-40-85°C,湿度0-100%
- 轻量I2C,slim版可用

### 4.4 SHT31 (type:38)
**详细文档**: [sensor-sht31.md](sensor-sht31.md)

**特性**:
- I2C接口,地址0x44或0x45
- 高精度温湿度
- 轻量I2C,slim版可用

---

## 5. 环境传感器

### 5.1 超声波HC-SR04 (type:38)
**详细文档**: [sensor-ultrasonic.md](sensor-ultrasonic.md)

**特性**:
- 测距2-400cm
- 需要触发和回声两个引脚
- 采样间隔≥100ms

### 5.2 BMP280气压 (type:38)
**详细文档**: [sensor-bmp280.md](sensor-bmp280.md)

**特性**:
- I2C接口
- 温度、气压、海拔
- 仅full版支持

### 5.3 MPU6050陀螺仪 (type:38)
**详细文档**: [sensor-mpu6050.md](sensor-mpu6050.md)

**特性**:
- I2C接口,地址0x68
- 六轴姿态(加速度+陀螺仪)
- 仅full版支持

### 5.4 BH1750光照 (type:38)
**详细文档**: [sensor-bh1750.md](sensor-bh1750.md)

**特性**:
- I2C接口,地址0x23或0x5C
- 光照强度1-65535 lux
- 轻量I2C,slim版可用

---

## 6. 电气传感器

### 6.1 电流传感器ACS712 (type:15)
**详细文档**: [sensor-current.md](sensor-current.md)

**特性**:
- ADC采集
- 量程5A/20A/30A
- 需要校准参数

### 6.2 电压传感器 (type:15)
**详细文档**: [sensor-voltage.md](sensor-voltage.md)

**特性**:
- ADC+分压电路
- 测量范围0-25V
- 需要分压比参数

---

## 7. 通信设备

### 7.1 Modbus设备 (type:51)
**详细文档**: [modbus-device.md](modbus-device.md)

**特性**:
- RS485接口
- Modbus RTU主站
- 支持多从站扫描

### 7.2 红外遥控 (type:38)
**详细文档**: [ir-remote.md](ir-remote.md)

**特性**:
- NEC/RC5解码
- 仅full版支持

### 7.3 RFID读卡器MFRC522 (type:38)
**详细文档**: [rfid-mfrc522.md](rfid-mfrc522.md)

**特性**:
- SPI接口
- 读取RFID卡UID
- 仅full版支持

---

## 8. 其他外设

### 8.1 NeoPixel WS2812B (type:45)
**详细文档**: [neopixel.md](neopixel.md)

**特性**:
- 单线控制RGB灯带
- 支持多个LED串联

### 8.2 旋转编码器 (type:43)
**详细文档**: [encoder.md](encoder.md)

**特性**:
- AB相计数
- 支持旋转和按下

### 8.3 SD卡 (type:37,占位)
**详细文档**: [storage-sd-card.md](storage-sd-card.md)

**状态**: 驱动待扩展,建议S3-full

### 8.4 设备事件 (type:60)
**详细文档**: [device-event.md](device-event.md)

**特性**:
- 逻辑事件源
- 用于规则联动

---

## 引脚分配原则

### 安全引脚
- ✅ **GPIO34-39**: 仅输入,适合ADC/数字输入
- ✅ **GPIO32-39**: ADC1,WiFi开启后可用(优先使用)
- ❌ **GPIO6-11**: 连接Flash,不建议使用
- ❌ **GPIO4,12-15,25-27**: ADC2,WiFi开启后不可用
- ⚠️ **GPIO0**: BOOT按键,谨慎使用
- ⚠️ **GPIO5**: STATE指示灯,默认占用

### I2C设备
- SDA: GPIO21/22(推荐)
- SCL: GPIO22/21(推荐)
- 多个I2C设备可共用SDA/SCL,但地址不能冲突

---

## Web配置流程

### 步骤1: 添加外设
1. 进入Web界面 → **外设配置**
2. 点击 **添加外设**
3. 选择外设类型
4. 填写外设ID和名称
5. 分配引脚(参考引脚分配原则)
6. 填写参数(如I2C地址、初始状态等)

### 步骤2: 启用外设
1. 确认引脚接线正确
2. 勾选 **启用**
3. 点击 **保存**
4. 查看串口日志确认初始化成功

### 步骤3: 创建执行规则
1. 进入 **外设执行**
2. 添加规则,配置触发器
3. 配置动作(如GPIO控制、传感器读取、显示等)
4. 保存并启用规则

### 步骤4: 配置备份
1. 进入 **设备配置 > 高级配置**
2. 导出peripherals.json备份
3. 保存为本地文件

---

## 常见问题

### Q1: 外设无响应?
- 检查外设是否启用
- 检查引脚配置是否正确
- 检查接线是否牢固
- 查看串口日志确认初始化状态

### Q2: I2C设备初始化失败?
- 检查SDA/SCL是否接反
- 检查I2C地址是否正确
- 检查是否有上拉电阻
- 确认电源为3.3V

### Q3: ADC数值异常?
- 检查电压是否超过3.3V
- 确认使用ADC1引脚(GPIO32-39)
- 检查ADC衰减和校准参数

### Q4: 传感器读数不准确?
- 检查采样间隔是否太短(DHT≥2秒)
- 检查传感器供电是否稳定
- 确认传感器类型配置正确

---

## 相关文档

- [示例文档](../examples/README.md) - 48个实际应用示例
- [外设执行指南](../periph-exec/README.md) - 触发器和动作配置
- [传感器完整指南](sensor-guide-complete.md) - 40+种传感器详细参数
- [架构设计](../architecture.md) - 系统架构和外设管理模块

---

**文档版本**: v2.0 (整合版)  
**最后更新**: 2026-06-03  
**维护者**: FastBee团队
