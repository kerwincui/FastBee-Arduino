# 支持的传感器与模块清单

本文按当前代码和固件档位整理 FastBee-Arduino 可配置、可联动或可扩展的传感器与模块。`slim` 指 `esp32`、`esp32c3`、`esp32s3` 精简稳定版；`S3-full` 指 `esp32s3-full` 完整功能版。

## 状态说明

| 状态 | 含义 |
| --- | --- |
| 已内置 | 当前固件已有外设类型、驱动或外设执行动作，可直接配置使用 |
| 通用接入 | 通过 GPIO、ADC、PWM、UART、I2C、SPI 等通用外设接入，模块本身不需要专用驱动 |
| S3-full | 需要完整功能版或对应编译开关，适合 ESP32-S3 高性能版本 |
| 待扩展 | 已有文档或配置占位，但运行时专用驱动/动作还需要补充 |

## 核心外设与执行模块

| 模块 | 当前状态 | 适用版本 | 接入方式/说明 |
| --- | --- | --- | --- |
| 外部中断 | 已内置 | slim / S3-full | `GPIO_INTERRUPT_RISING/FALLING/CHANGE`，可触发外设执行事件 |
| 定时器中断/定时任务 | 已内置 | slim / S3-full | 外设执行定时触发器，支持周期、时间点、轮询触发 |
| PWM 呼吸灯 | 已内置 | slim / S3-full | PWM 输出 + 外设执行呼吸/闪烁动作 |
| ADC | 已内置 | slim / S3-full | ADC 或 GPIO 模拟输入，支持线性校准类传感器 |
| RGB 彩灯 | 已内置 | slim / S3-full | NeoPixel/WS2812B，注意灯珠数量和电源容量 |
| 舵机 | 已内置 | slim / S3-full | PWM 舵机输出，适合 SG90 等小舵机 |
| 步进电机 | 已内置 | slim / S3-full | 本地步进电机外设；Modbus 步进电机支持软限位字段 |
| Socket 通信 | S3-full | S3-full | TCP/HTTP/CoAP 等扩展协议建议放在完整功能版 |
| SD 卡 | 待扩展 | 建议 S3-full | `SDIO` 配置占位已有，运行时挂载和文件动作待补 |

## 时钟模块

| 模块 | 当前状态 | 适用版本 | 接入方式/说明 |
| --- | --- | --- | --- |
| RTC 实时时钟 | 已内置 | slim / S3-full | 使用系统时间 + NTP 同步，供定时规则和日志时间戳使用 |
| DS1302 实时时钟 | 待扩展 | 建议 S3-full | 三线 RTC 驱动未内置，可按示例文档扩展 bit-bang 驱动 |

## 温湿度、距离与环境传感器

| 模块 | 当前状态 | 适用版本 | 接入方式/说明 |
| --- | --- | --- | --- |
| DS18B20 温度传感器 | 已内置 | slim / S3-full | OneWire 温度采集 |
| DHT11/DHT22 | 已内置 | slim / S3-full | DHT 温湿度采集 |
| SHT31 | 已内置 | slim / S3-full | I2C 轻量温湿度驱动 |
| AHT20 | 已内置 | slim / S3-full | I2C 轻量温湿度驱动 |
| BH1750 光照传感器 | 已内置 | slim / S3-full | I2C 轻量光照驱动 |
| 超声波测距 | 已内置 | slim / S3-full | HC-SR04，Trig/Echo 双 GPIO |
| BMP280 气压传感器 | S3-full | S3-full | I2C 高级传感器驱动 |
| MPU6050 陀螺仪加速传感器 | S3-full | S3-full | I2C 六轴姿态驱动 |

## 数字量/模拟量传感器

这些模块大多可通过 GPIO 数字输入、GPIO 中断输入或 ADC 模拟输入接入，适合精简版长期运行。

| 模块 | 当前状态 | 适用版本 | 推荐接入 |
| --- | --- | --- | --- |
| 激光传感器/激光模块 | 通用接入 | slim / S3-full | GPIO 输出或 GPIO/ADC 输入，视模块类型而定 |
| 倾斜传感器 | 通用接入 | slim / S3-full | GPIO 输入/上拉输入/中断输入 |
| 震动传感器 | 通用接入 | slim / S3-full | GPIO 输入或 ADC 输入 |
| 干簧管传感器 | 通用接入 | slim / S3-full | GPIO 上拉输入或中断输入 |
| 对射光电传感器 | 通用接入 | slim / S3-full | GPIO 输入或中断输入 |
| 雨滴探测传感器 | 通用接入 | slim / S3-full | ADC 模拟输入，DO 可接 GPIO 阈值输入 |
| PS2 操作杆 | 通用接入 | slim / S3-full | 双 ADC + 一个 GPIO 按键输入 |
| 声音传感器 | 通用接入 | slim / S3-full | ADC 模拟输入，DO 可接 GPIO 阈值输入 |
| 光敏传感器 | 通用接入 | slim / S3-full | ADC 模拟输入 |
| 火焰传感器 | 通用接入 | slim / S3-full | ADC 模拟输入或 GPIO 阈值输入 |
| 烟雾传感器检测 | 通用接入 | slim / S3-full | MQ 系列建议 ADC 输入，DO 可接 GPIO |
| 触摸开关传感器 | 通用接入 | slim / S3-full | ESP32 touch 输入或普通 GPIO 输入 |
| 红外避障传感器 | 通用接入 | slim / S3-full | GPIO 输入 |
| 红外循迹模块 | 通用接入 | slim / S3-full | GPIO 或 ADC，多路循迹占用多个引脚 |
| PIR 人体热释电感应模块 | 通用接入 | slim / S3-full | GPIO 输入/下拉输入/中断输入 |

## 输入、识别与通信模块

| 模块 | 当前状态 | 适用版本 | 接入方式/说明 |
| --- | --- | --- | --- |
| 旋转编码器 | 已内置 | slim / S3-full | `ENCODER` 外设，双相 GPIO 输入 |
| 红外遥控 | S3-full | S3-full | IRremoteESP8266 解码，需 `FASTBEE_ENABLE_IR_REMOTE` |
| MFRC522 RFID 射频卡模块 | S3-full | S3-full | SPI RFID，需 `FASTBEE_ENABLE_RFID` |
| Modbus 子设备 | 已内置 | slim / S3-full | RTU 主站支持采集任务、寄存器映射、继电器/PWM/PID/电机子设备 |

## 选型建议

- ESP32-C3：优先选择 GPIO、ADC、DHT、DS18B20、超声波、简单 Modbus 和少量规则。
- classic ESP32：适合默认精简版，外设和规则规模中等时稳定性较好。
- ESP32-S3 精简版：适合需要更多余量但仍以核心功能为主的项目。
- ESP32-S3 full：用于 BMP280、MPU6050、RFID、红外遥控、SD/TCP/HTTP/CoAP、OTA、文件管理、RuleScript 等高资源能力。

更多接线和 JSON 示例见 [传感器与模块完整指南](./sensor-guide-complete.md)、[外设配置指南](../peripheral-configuration-guide.md) 和 [示例目录](../examples/)。
