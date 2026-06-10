# 支持的模块和传感器清单

本文按当前代码、默认配置和 PlatformIO 版本档位整理 FastBee-Arduino 支持的模块。用于选型时先判断“能不能用、用哪个固件、从哪里配置”，接线和参数细节再看对应示例。

## 版本标记

| 标记 | PlatformIO 环境 | 说明 |
| --- | --- | --- |
| Lite | `esp32c3`、`esp32c6` | 低资源版本，优先保证 WiFi、MQTT、GPIO、基础传感器和外设执行稳定 |
| Standard | `esp32`、`esp32s3` | 默认推荐版本，增加 Modbus、以太网、4G、RFID、红外、更多 I2C 传感器 |
| Full | `esp32s3-full` | ESP32-S3 16MB Flash + PSRAM，增加 OTA、文件、日志、用户角色、RuleScript、LoRa 等完整管理功能 |

状态含义：`内置` 表示固件已有运行时支持；`通用接入` 表示通过 GPIO/ADC/PWM/UART/I2C/SPI 等通用外设配置；`占位` 表示类型或文档已有，但运行时专用驱动仍需扩展。

> 注意：ESP32-C6 当前禁用 DS18B20/OneWire；`esp32s3-full` 当前禁用红外遥控驱动，避免 S3 RMT 兼容问题。需要红外遥控优先使用 Standard 固件。

## 快速选型

| 模块/传感器 | 状态 | 版本 | 接入/外设类型 | 文档 |
| --- | --- | --- | --- | --- |
| LED、继电器、蜂鸣器、普通开关量输出 | 内置 | Lite / Standard / Full | `GPIO_DIGITAL_OUTPUT` | [LED](../examples/01-led-first.md)、[继电器](../examples/05-relay.md) |
| 按键、门磁、PIR、震动、倾斜、对射光电 | 通用接入 | Lite / Standard / Full | `GPIO_DIGITAL_INPUT` / `GPIO_DIGITAL_INPUT_PULLUP` | [按键](../examples/06-button-control.md)、[PIR](../examples/44-pir-motion.md) |
| 外部中断输入 | 内置 | Lite / Standard / Full | `GPIO_INTERRUPT_RISING/FALLING/CHANGE` | [外部中断](../examples/09-external-interrupt.md) |
| ADC 模拟量输入 | 内置 | Lite / Standard / Full | `GPIO_ANALOG_INPUT` / `ADC` | [ADC 电压](../examples/13-adc-voltage.md) |
| PWM 输出、调光、电机调速 | 内置 | Lite / Standard / Full | `GPIO_PWM_OUTPUT` | [PWM 呼吸灯](../examples/11-pwm-breathing.md) |
| 触摸输入 | 内置 | Lite / Standard / Full | `GPIO_TOUCH` | [触摸传感器](../examples/36-touch-sensor.md) |
| WS2812B / NeoPixel RGB 灯带 | 内置 | Lite / Standard / Full | `NEO_PIXEL` | [RGB 灯带](../examples/14-rgb-neopixel.md) |
| TM1637 四位数码管 | 内置 | Lite / Standard / Full | `SEVEN_SEGMENT_TM1637` | [数码管](../examples/15-seven-segment.md) |
| OLED SSD1306 / SH1106 | 内置 | Lite / Standard / Full | `LCD`，I2C | [OLED 显示](display-oled.md) |
| LCD1602/PCF8574 字符屏 | 占位 | 建议 Full | `LCD`，驱动待扩展 | [LCD1602](../examples/39-lcd1602-display.md) |
| 舵机 SG90/MG996R | 内置 | Lite / Standard / Full | `PWM_SERVO` | [舵机](../examples/22-servo-sg90.md) |
| ULN2003/28BYJ-48 步进电机 | 内置 | Lite / Standard / Full | `STEPPER_MOTOR` | [步进电机](../examples/08-stepper-motor.md) |
| 旋转编码器 | 内置 | Lite / Standard / Full | `ENCODER` | [编码器](../examples/37-rotary-encoder.md) |
| 433MHz OOK/EV1527/PT2262 射频模块 | 内置 | Lite / Standard / Full | `RF_MODULE` | [433MHz 射频](../examples/49-433mhz-rf-module.md) |
| RCWL-0516 / 5.8GHz 雷达感应 | 内置 | Lite / Standard / Full | `RADAR_SENSOR` | [雷达感应](../examples/50-radar-sensor.md) |
| 系统 NTP 时钟/定时规则 | 内置 | Lite / Standard / Full | 系统时间 + 外设执行定时触发 | [NTP 时钟](../examples/16-rtc-clock.md) |
| DS1302 外部 RTC | 示例/可扩展 | Lite / Standard / Full | GPIO bit-bang 示例 | [DS1302](../examples/17-ds1302-rtc.md) |
| UART 串口模块 | 内置 | Lite / Standard / Full | `UART` | [串口通信](../examples/12-uart-communication.md) |
| I2C 总线设备 | 内置 | Lite / Standard / Full | `I2C` | [外设指南](README.md) |
| SPI 总线设备 | 内置 | Lite / Standard / Full | `SPI` | [外设指南](README.md) |
| Modbus RTU 设备 | 内置 | Standard / Full | `UART` + Modbus RTU | [Modbus 温湿度](../examples/45-modbus-temp-humidity.md) |
| Modbus 继电器/PWM/PID/电机子设备 | 内置 | Standard / Full | `MODBUS_DEVICE` | [Modbus 继电器](../examples/47-modbus-relay.md)、[Modbus 步进](../examples/46-modbus-stepper-motor.md) |
| 以太网模块 | 内置开关 | Standard / Full | `ETHERNET`，需匹配硬件 | [部署指南](../deployment.md) |
| 4G/蜂窝模块 | 内置开关 | Standard / Full | UART + TinyGSM | [部署指南](../deployment.md) |
| LoRa | 内置开关 | Full | LoRa 功能建议 PSRAM 版本 | [版本对比](../system/edition-comparison.md) |
| MFRC522 RFID | 内置 | Standard / Full | SPI，`FASTBEE_ENABLE_RFID` | [RFID](../examples/43-rfid-mfrc522.md) |
| 红外遥控接收 | 内置 | Standard | `FASTBEE_ENABLE_IR_REMOTE` | [红外遥控](../examples/21-ir-remote.md) |
| SD 卡 | 占位 | 建议 Full | `SDIO`，运行时挂载/文件动作待补 | [SD 卡](../examples/24-sd-card.md) |
| 摄像头 | 占位 | 建议 Full | `CAMERA` 类型占位 | [外设配置指南](peripheral-configuration-guide.md) |
| CAN / USB 专用外设 | 占位 | 按硬件扩展 | `CAN` / `USB` 类型占位 | [外设配置指南](peripheral-configuration-guide.md) |

## 已内置专用传感器

| 传感器 | 状态 | 版本 | 采集字段/说明 | 文档 |
| --- | --- | --- | --- | --- |
| DHT11/DHT22/AM2302 | 内置 | Lite / Standard / Full | 温度、湿度；采样间隔建议 >= 2 秒 | [DHT](sensor-dht.md) |
| DS18B20 | 内置 | Lite(不含 esp32c6) / Standard / Full | OneWire 温度；可多探头 | [DS18B20](sensor-ds18b20.md) |
| AHT20 | 内置 | Lite / Standard / Full | I2C 温湿度，地址 `0x38` | [AHT20](sensor-aht20.md) |
| SHT31 | 内置 | Lite / Standard / Full | I2C 温湿度，地址 `0x44/0x45` | [SHT31](sensor-sht31.md) |
| BH1750 | 内置 | Lite / Standard / Full | I2C 光照，地址 `0x23/0x5C` | [BH1750](sensor-bh1750.md) |
| HC-SR04 超声波 | 内置 | Lite / Standard / Full | Trig/Echo 双 GPIO，距离 | [超声波](sensor-ultrasonic.md) |
| ACS712 电流传感器 | 内置 | Lite / Standard / Full | ADC + 线性校准，电流 | [电流](sensor-current.md) |
| 分压电压传感器 | 内置 | Lite / Standard / Full | ADC + 分压比，电压 | [电压](sensor-voltage.md) |
| BMP280 | 内置 | Standard / Full | I2C 温度、气压、海拔 | [BMP280](sensor-bmp280.md) |
| MPU6050 | 内置 | Standard / Full | I2C 加速度、角速度、温度 | [MPU6050](sensor-mpu6050.md) |
| MFRC522 RFID | 内置 | Standard / Full | 卡片检测/移除事件、UID | [RFID](../examples/43-rfid-mfrc522.md) |
| 红外遥控 | 内置 | Standard | 红外码接收；Full 当前关闭 | [红外遥控](../examples/21-ir-remote.md) |
| 433MHz RF 接收电平 | 内置 | Lite / Standard / Full | `value=1/0` 电平监测 | [433MHz 射频](../examples/49-433mhz-rf-module.md) |
| 雷达存在检测 | 内置 | Lite / Standard / Full | `presence=1/0` | [雷达感应](../examples/50-radar-sensor.md) |

## 可通过 GPIO/ADC 接入的常见模块

这些模块不需要专用驱动，按输出信号选择 GPIO 或 ADC 类型即可。

| 模块 | 推荐接入 | 说明 |
| --- | --- | --- |
| 激光发射/激光接收模块 | GPIO 输出/输入 | 发射端按数字输出，接收端按数字输入或 ADC |
| 倾斜开关 | GPIO 输入/上拉输入 | 适合事件触发 |
| 震动传感器 | GPIO 输入或 ADC | DO 接 GPIO，AO 接 ADC |
| 干簧管 | GPIO 上拉输入 | 门磁、水位浮球等同类开关量 |
| 对射/漫反射光电 | GPIO 输入 | NPN/PNP 输出需按模块电平转换 |
| 雨滴传感器 | ADC + GPIO | AO 看强度，DO 看阈值 |
| PS2 摇杆 | 双 ADC + GPIO | X/Y 接 ADC，SW 接 GPIO |
| 声音传感器 | ADC + GPIO | AO 看声强，DO 看阈值 |
| 光敏电阻模块 | ADC | 建议做现场阈值标定 |
| 火焰传感器 | ADC + GPIO | AO/DO 二选一或同时使用 |
| MQ-2/MQ 系列烟雾/气体 | ADC + GPIO | 需预热，建议只做趋势/阈值判断 |
| 红外避障 | GPIO 输入 | 适合近距离开关量 |
| 红外循迹 | 多路 GPIO/ADC | 多路模块按多个外设配置 |
| PIR 人体红外 | GPIO 输入 | 上电需预热，适合低频事件 |
| 土壤湿度、电导率等模拟模块 | ADC | 注意供电、电极腐蚀和校准 |

## 默认配置模板

`data/config/peripherals.json` 当前提供 46 个安全禁用的默认模板，覆盖 UART/I2C/SPI、GPIO 输入输出、ADC/PWM、DHT11、DS18B20、OLED、TM1637、舵机、步进电机、NeoPixel、SDIO、以太网、Modbus 等。首次使用时不要直接启用全部模板，请只启用实际接线的模块。

## 选型建议

- 低成本传感节点：选 Lite，优先 GPIO、ADC、DHT、AHT20、SHT31、BH1750、HC-SR04、OLED、TM1637、RF/雷达这类轻量模块。
- 工业采集和控制：选 Standard，增加 Modbus RTU、以太网、4G、RFID、红外、BMP280、MPU6050。
- 完整管理和二次开发：选 Full，适合 OTA、文件管理、日志、用户角色、RuleScript、LoRa 和更复杂 Web 管理，但红外遥控当前不要选 Full。
- 长期稳定优先：少启用未接线外设，采样间隔不低于传感器建议值，ADC 优先使用 ADC1 引脚，避免 Flash/PSRAM/启动绑带脚。

## 相关入口

- [外设配置完整指南](README.md)
- [50 个示例教程](../examples/README.md)
- [外设执行规则](../periph-exec/README.md)
- [版本对比](../system/edition-comparison.md)