# FastBee-Arduino 传感器与模块完整指南

本文档是 FastBee-Arduino 项目所有支持的传感器、模块和外设的完整参考手册，涵盖接线说明、配置参数、JSON示例、外设执行联动、编译开关和引脚占用等信息。

传感器和模块都先在“外设配置”中登记为硬件对象，再由“外设执行”规则负责周期采集、阈值判断、显示或上报。

![外设配置列表](../system/images/peripheral-management.png)

![外设执行规则列表](../system/images/periph-exec-management.png)

![传感器数据采集与联动链路](../images/sensor-data-pipeline.svg)

传感器读数并不是直接上报平台：它先经过外设配置、驱动读取和数据缓存，再由外设执行、显示屏或 MQTT 上报消费。

![GPIO、ADC、PWM 接线校验图](../images/gpio-adc-pwm-wiring-check.svg)

GPIO、ADC 和 PWM 类模块接入前先核对引脚能力、电平、供电和默认状态；会驱动负载的模块必须先禁用保存并单项验证。

## 目录

- [1. GPIO 基础模块](#1-gpio-基础模块)
- [2. 显示与灯光模块](#2-显示与灯光模块)
- [3. 时钟模块](#3-时钟模块)
- [4. 温湿度传感器](#4-温湿度传感器)
- [5. 环境与距离传感器](#5-环境与距离传感器)
- [6. 运动与姿态传感器](#6-运动与姿态传感器)
- [7. 控制模块](#7-控制模块)
- [8. 通信模块](#8-通信模块)
- [9. 环境检测传感器](#9-环境检测传感器)
- [10. 输入设备](#10-输入设备)
- [11. 识别与存储模块](#11-识别与存储模块)
- [附录A：编译开关对照表](#附录a编译开关对照表)
- [附录B：引脚分配建议](#附录b引脚分配建议)
- [附录C：资源消耗参考](#附录c资源消耗参考)

---

## 1. GPIO 基础模块

### 1.1 GPIO 数字输出
- **类型ID**: 12 (GPIO_DIGITAL_OUTPUT)
- **应用场景**: LED指示灯、继电器控制、蜂鸣器、直流电机开关
- **接线**: GPIO引脚 → 负载 → GND（或VCC，视电路而定）
- **配置示例**: 详见 [gpio-output.md](./gpio-output.md)

### 1.2 GPIO 数字输入
- **类型ID**: 11/13/14/18/19/20/21
- **支持类型**:
  - 11: 浮空输入
  - 13: 内部上拉（按键常用）
  - 14: 内部下拉（PIR常用）
  - 18/19/20: 中断输入（上升沿/下降沿/变化）
  - 21: 触摸输入（仅ESP32特定引脚）
- **按键事件**: 单击、双击、长按2s/5s/10s、按下、释放
- **配置示例**: 详见 [gpio-input.md](./gpio-input.md)

### 1.3 PWM 输出
- **类型ID**: 17 (GPIO_PWM_OUTPUT)
- **应用场景**: 呼吸灯、电机调速、舵机控制、LED调光
- **参数**: pwmChannel(0-15), pwmFrequency(1-40000Hz), pwmResolution(1-20bit)
- **配置示例**: 详见 [pwm-output.md](./pwm-output.md)

### 1.4 ADC 模拟输入
- **类型ID**: 26 (ADC)
- **应用场景**: 电位器、光敏电阻、声音传感器、火焰/烟雾传感器
- **ESP32限制**: 仅GPIO32-39支持ADC1（推荐），ADC2与WiFi冲突
- **ESP32-S3**: GPIO1-20支持ADC1
- **分辨率**: 9-12bit可配置
- **配置示例**: 详见 [adc-input.md](./adc-input.md)

---

## 2. 显示与灯光模块

### 2.1 NeoPixel RGB灯带 (WS2812B)
- **类型ID**: 45 (NEO_PIXEL)
- **编译开关**: `FASTBEE_ENABLE_LED_SCREEN`
- **Flash占用**: ~10KB (RMT轻量驱动)
- **RAM**: 24 × 灯珠数 × 4字节（默认限制64颗）
- **接线**: VCC(5V/3.3V) → DIN → GPIO → GND
- **引脚**: 任意GPIO（推荐GPIO2/4/12/13/14/15/25/26/27）
- **支持动作**: 设置颜色、设置亮度、彩虹效果、关闭
- **配置示例**: 详见 [neopixel.md](./neopixel.md)

### 2.2 TM1637 四位数码管
- **类型ID**: 47 (SEVEN_SEGMENT_TM1637)
- **编译开关**: `FASTBEE_ENABLE_SEVEN_SEGMENT`
- **Flash占用**: ~3KB (bit-bang驱动，无外部库)
- **RAM**: ~32B × 实例数
- **接线**: VCC → CLK → GPIO1, DIO → GPIO2, GND
- **引脚**: 任意2个GPIO
- **支持动作**: 显示数字、显示文本、显示小数点、清空
- **配置示例**: 详见 [display-tm1637.md](./display-tm1637.md)

### 2.3 OLED 显示屏 (SSD1306/SH1106)
- **类型ID**: 36 (LCD)
- **编译开关**: `FASTBEE_ENABLE_LCD`
- **Flash占用**: ~20KB (U8g2库)
- **RAM**: 1KB帧缓冲区 + ~200B管理器
- **接口**: I2C (SDA/SCL) 或 SPI
- **I2C接线**: VCC → SDA(GPIO21) → SCL(GPIO22) → GND
- **分辨率**: 128×64 (SSD1306), 128×64/132×64 (SH1106)
- **支持动作**: 显示文本、显示数字、绘制图形、清空
- **配置示例**: 详见 [display-oled.md](./display-oled.md)

---

## 3. 时钟模块

### 3.1 NTP 网络时钟（内置）
- **说明**: ESP32联网时自动通过NTP同步时间，无需额外硬件
- **精度**: ±50ms（取决于网络延迟）
- **用途**: 定时触发规则、时间戳记录

### 3.2 DS1302 RTC实时时钟
- **当前状态**: 未内置驱动
- **建议**: 
  - 联网场景使用NTP（推荐）
  - 离线场景后续可补bit-bang RTC驱动
  - 可外接I2C RTC模块（如DS3231，通过自定义脚本读取）

---

## 4. 温湿度传感器

### 4.1 DHT11 / DHT22
- **类型ID**: 38 (SENSOR), sensorType: 3=DHT11, 4=DHT22
- **编译开关**: `FASTBEE_ENABLE_SENSOR_DRIVER`
- **Flash占用**: ~3KB (DHT库)
- **接口**: 单总线（1个GPIO）
- **接线**: VCC → DATA → GPIO → GND（需4.7K-10K上拉）
- **读取间隔**: ≥2000ms（硬件限制）
- **数据字段**: `temperature`, `humidity`
- **对比**:
  | 参数 | DHT11 | DHT22 |
  |------|-------|-------|
  | 温度范围 | 0~50°C | -40~80°C |
  | 温度精度 | ±2°C | ±0.5°C |
  | 湿度范围 | 20~80% | 0~100% |
  | 湿度精度 | ±5% | ±2~5% |
- **配置示例**: 详见 [sensor-dht.md](./sensor-dht.md)

### 4.2 DS18B20 温度传感器
- **类型ID**: 38 (SENSOR), sensorType: 2
- **接口**: 单总线（OneWire，1个GPIO）
- **接线**: VCC → DATA → GPIO → GND（需4.7K上拉）
- **特性**: 
  - 防水探头可选
  - 总线可挂载多个传感器（不同ROM地址）
  - 精度±0.5°C，范围-55~125°C
- **数据字段**: `temperature`
- **配置示例**: 详见 [sensor-ds18b20.md](./sensor-ds18b20.md)

### 4.3 SHT31 / SHT30
- **类型ID**: 38 (SENSOR), sensorType: 5
- **接口**: I2C（SDA/SCL）
- **地址**: 0x44（默认）或0x45（ADDR引脚拉高）
- **精度**: 温度±0.3°C，湿度±2%RH
- **范围**: -40~125°C，0~100%RH
- **数据字段**: `temperature`, `humidity`
- **配置示例**: 详见 [sensor-sht31.md](./sensor-sht31.md)

### 4.4 AHT20 / AHT21
- **类型ID**: 38 (SENSOR), sensorType: 6
- **接口**: I2C
- **地址**: 0x38
- **精度**: 温度±0.3°C，湿度±2~3%RH
- **特点**: 性价比高，响应快
- **数据字段**: `temperature`, `humidity`
- **配置示例**: 详见 [sensor-aht20.md](./sensor-aht20.md)

---

## 5. 环境与距离传感器

### 5.1 HC-SR04 超声波测距
- **类型ID**: 38 (SENSOR), sensorType: 1
- **接口**: GPIO（2个引脚：Trig/Echo）
- **接线**: VCC → Trig → GPIO1, Echo → GPIO2 → GND
- **范围**: 2cm ~ 400cm
- **精度**: ±3mm
- **数据字段**: `distance`
- **配置示例**: 详见 [sensor-ultrasonic.md](./sensor-ultrasonic.md)

### 5.2 BH1750 光照传感器
- **类型ID**: 38 (SENSOR), sensorType: 7
- **接口**: I2C
- **地址**: 0x23（ADDR拉低）或0x5C（ADDR拉高）
- **范围**: 1~65535 lux
- **精度**: ±20%
- **数据字段**: `lux`
- **配置示例**: 详见 [sensor-bh1750.md](./sensor-bh1750.md)

### 5.3 BMP280 气压/温度传感器
- **类型ID**: 38 (SENSOR), sensorType: 8
- **编译开关**: `FASTBEE_ENABLE_I2C_SENSORS`（仅S3-full）
- **接口**: I2C或SPI
- **地址**: 0x76（默认）或0x77（SDO拉高）
- **数据字段**: `pressure`, `temperature`, `altitude`
- **精度**: 气压±1hPa，温度±1°C，海拔±1m
- **配置示例**: 详见 [sensor-bmp280.md](./sensor-bmp280.md)

---

## 6. 运动与姿态传感器

### 6.1 MPU6050 陀螺仪加速度计
- **类型ID**: 38 (SENSOR), sensorType: 9
- **编译开关**: `FASTBEE_ENABLE_I2C_SENSORS`（仅S3-full）
- **接口**: I2C
- **地址**: 0x68（AD0拉低）或0x69（AD0拉高）
- **数据字段**: 
  - `acc_x`, `acc_y`, `acc_z` (加速度 m/s²)
  - `gyro_x`, `gyro_y`, `gyro_z` (角速度 °/s)
  - `temperature` (芯片温度)
- **量程**: 加速度±2/4/8/16g，陀螺仪±250/500/1000/2000°/s
- **配置示例**: 详见 [sensor-mpu6050.md](./sensor-mpu6050.md)

---

## 7. 控制模块

### 7.1 SG90 舵机
- **类型ID**: 43 (PWM_SERVO)
- **接口**: PWM（1个GPIO）
- **接线**: 红线(VCC 5V) → 信号线(PWM GPIO) → 棕线(GND)
- **角度范围**: 0~180°
- **控制**: actionValue为角度值(0-180)
- **注意**: 需外部5V供电，ESP32 3.3V可能驱动不稳
- **配置示例**: 详见 [servo.md](./servo.md)

### 7.2 步进电机 (28BYJ-48 + ULN2003)
- **类型ID**: 44 (STEPPER_MOTOR)
- **接口**: GPIO（4个引脚：IN1~IN4）
- **接线**: IN1~IN4 → GPIO1~4, VCC → 5V, GND → GND
- **参数**: stepsPerRevolution(2048), speed(RPM 1~15)
- **支持动作**: 正转、反转、停止、设置角度
- **配置示例**: 详见 [stepper-motor.md](./stepper-motor.md)

### 7.3 继电器
- **类型ID**: 12 (GPIO_DIGITAL_OUTPUT)
- **接线**: VCC → IN → GPIO → GND
- **负载**: 常开(NO)/常闭(NC)触点，最大10A/250VAC
- **注意**: 控制交流电时务必注意安全隔离
- **配置示例**: 详见 [gpio-output.md](./gpio-output.md)

---

## 8. 通信模块

### 8.1 UART 串口通信
- **类型ID**: 1 (UART)
- **接口**: HardwareSerial (Serial1/Serial2)
- **接线**: TX → RX_other, RX ← TX_other, GND共地
- **参数**: baudRate(9600/115200等), dataBits(8), stopBits(1), parity(NONE)
- **引脚**: 
  - ESP32: Serial1默认GPIO9/10, Serial2默认GPIO16/17
  - ESP32-S3: 任意可复用引脚
- **数据源规则**: 支持按需轮询（仅当有规则监听时轮询）
- **配置示例**: 详见示例文档 [12-uart-communication.md](../examples/12-uart-communication.md)

### 8.2 红外遥控
- **类型ID**: GPIO_INTERRUPT_CHANGE (20) + IRremoteESP8266解码
- **编译开关**: `FASTBEE_ENABLE_IR_REMOTE`（仅S3-full）
- **接口**: GPIO中断输入
- **协议**: NEC/RC5/SONY/SAMSUNG等
- **接线**: IR接收头OUT → GPIO
- **事件**: 解码后的红外码值作为事件触发源
- **配置示例**: 详见 [ir-remote.md](./ir-remote.md)

### 8.3 Modbus RTU 通信
- **类型ID**: 51 (MODBUS_DEVICE)
- **接口**: RS485（UART + DE/RE控制）
- **功能码**: 03(读寄存器), 06(写单寄存器), 10(写多寄存器)
- **设备**: 继电器板、温控器、变频器、土壤传感器等
- **配置示例**: 详见 [modbus-device.md](./modbus-device.md)

---

## 9. 环境检测传感器

所有环境检测传感器均可配置为GPIO数字输入（类型13/14）或ADC模拟输入（类型26），取决于模块是否有数字输出(DO)和模拟输出(AO)。

### 9.1 激光传感器
- **接线**: VCC → DO → GPIO(PULLUP) → GND
- **输出**: 检测到激光输出低电平
- **应用**: 安防对射、位置检测

### 9.2 倾斜传感器
- **接线**: VCC → DO → GPIO(PULLUP) → GND
- **输出**: 倾斜时导通输出低电平
- **应用**: 姿态检测、防盗报警

### 9.3 震动传感器
- **接线**: VCC → DO → GPIO(PULLUP) → GND
- **输出**: 震动时导通输出低电平
- **应用**: 碰撞检测、振动报警

### 9.4 干簧管传感器
- **接线**: VCC → DO → GPIO(PULLUP) → GND
- **输出**: 磁铁靠近时导通输出低电平
- **应用**: 门窗开合检测、转速测量

### 9.5 对射光电传感器
- **接线**: VCC → DO → GPIO(PULLUP) → GND
- **输出**: 光束被遮挡时输出低电平
- **应用**: 计数、位置检测、安全光幕

### 9.6 雨滴传感器
- **接线**: 
  - DO → GPIO(PULLUP)（有雨滴输出低电平）
  - AO → ADC GPIO（模拟电压0~3.3V，雨滴越大电压越低）
- **应用**: 天气监测、自动关窗

### 9.7 声音传感器
- **接线**: 
  - DO → GPIO(PULLUP)（声音超过阈值输出低电平）
  - AO → ADC GPIO（模拟电压，声音越大电压越高）
- **应用**: 声控开关、噪音监测

### 9.8 光敏传感器
- **接线**: 
  - DO → GPIO(PULLUP)（暗环境输出低电平）
  - AO → ADC GPIO（模拟电压，光照越强电压越高）
- **应用**: 自动夜灯、光照度监测

### 9.9 火焰传感器
- **接线**: 
  - DO → GPIO(PULLUP)（检测到火焰输出低电平）
  - AO → ADC GPIO（模拟电压，火焰越强电压越低）
- **波长**: 760nm~1100nm红外线
- **应用**: 火灾报警、火焰探测

### 9.10 烟雾传感器 (MQ-2)
- **接线**: 
  - DO → GPIO(PULLUP)（烟雾超标输出低电平）
  - AO → ADC GPIO（模拟电压，浓度越高电压越低）
- **应用**: 火灾预警、空气质量监测

### 9.11 触摸传感器
- **接线**: VCC → OUT → GPIO(PULLUP) → GND
- **输出**: 触摸时输出高电平
- **ESP32内置触摸**: GPIO 0/2/4/12/13/14/15/27/32/33（类型ID 21）
- **应用**: 触摸开关、人机交互

### 9.12 红外避障传感器
- **接线**: VCC → DO → GPIO(PULLUP) → GND
- **输出**: 检测到障碍物输出低电平
- **范围**: 2~30cm可调
- **应用**: 小车避障、位置检测

---

## 10. 输入设备

### 10.1 按键
- **类型ID**: 13 (GPIO_DIGITAL_INPUT_PULLUP)
- **接线**: 一端接GPIO，另一端接GND
- **事件**: 单击、双击、长按2s/5s/10s、按下、释放
- **消抖**: 软件消抖~50ms
- **配置示例**: 详见 [gpio-input.md](./gpio-input.md)

### 10.2 PS2 摇杆
- **接线**: 
  - VRx → ADC GPIO1（左右，中间约1.65V）
  - VRy → ADC GPIO2（上下，中间约1.65V）
  - SW → GPIO(PULLUP)（按下按键）
- **输出**: 模拟电压0~3.3V，对应0~4095（12bit ADC）
- **应用**: 方向控制、游戏手柄、云台控制

### 10.3 旋转编码器
- **类型ID**: 46 (ENCODER) 或 GPIO_INTERRUPT
- **接线**: CLK → GPIO1, DT → GPIO2, SW → GPIO3(PULLUP)
- **输出**: 脉冲信号，通过中断计数方向和步数
- **应用**: 旋钮调节、菜单选择、音量控制
- **配置示例**: 详见 [encoder.md](./encoder.md)

### 10.4 PIR 人体热释电传感器
- **类型ID**: 14 (GPIO_DIGITAL_INPUT_PULLDOWN)
- **接线**: VCC → OUT → GPIO → GND
- **输出**: 检测到人输出高电平（持续数秒）
- **范围**: 3~7m，角度<120°
- **延迟**: 检测后持续输出0.5~5s（模块可调）
- **应用**: 人体检测、自动照明、安防报警

---

## 11. 识别与存储模块

### 11.1 MFRC522 RFID射频卡模块
- **类型ID**: 通过外设执行规则触发（刷卡事件）
- **编译开关**: `FASTBEE_ENABLE_RFID`（仅S3-full）
- **接口**: SPI（MOSI/MISO/SCK/SDA/SS）
- **接线**:
  ```
  MFRC522    ESP32
  SDA   →    GPIO5  (SS)
  SCK   →    GPIO18
  MOSI  →    GPIO23
  MISO  →    GPIO19
  IRQ   →    不接
  GND   →    GND
  RST   →    GPIO21
  3.3V  →    3.3V
  ```
- **频率**: 13.56MHz
- **支持卡片**: MIFARE Classic/Ultralight/DESFire等
- **功能**: 卡片UID读取、扇区读写
- **事件触发**: 刷卡时触发规则，UID作为事件数据
- **Flash占用**: ~12KB
- **配置示例**: 详见 [rfid-mfrc522.md](./rfid-mfrc522.md)

### 11.2 SD卡 / TF卡
- **类型ID**: 37 (SDIO)
- **接口**: SDMMC或SPI模式
- **当前状态**: 配置框架就绪，运行时挂载和文件动作待完善
- **建议**: 
  - S3-full可补SD驱动和文件动作
  - 用于日志落盘、数据记录、固件备份
- **接线** (SPI模式):
  ```
  SD卡模块  ESP32
  MOSI  →   GPIO23
  MISO  →   GPIO19
  CLK   →   GPIO18
  CS    →   GPIO5
  VCC   →   3.3V/5V（视模块）
  GND   →   GND
  ```
- **配置示例**: 详见 [storage-sd-card.md](./storage-sd-card.md)

---

## 附录A：编译开关对照表

| 功能模块 | 编译开关 | ESP32 | ESP32-C3 | ESP32-S3 | S3-full | Flash占用 |
|---------|---------|:-----:|:--------:|:--------:|:-------:|:---------:|
| **GPIO/ADC/PWM** | `FASTBEE_ENABLE_GPIO` | ✅ | ✅ | ✅ | ✅ | ~3KB |
| **DHT/DS18B20** | `FASTBEE_ENABLE_SENSOR_DRIVER` | ✅ | ✅ | ✅ | ✅ | ~8KB |
| **超声波/电流/电压** | `FASTBEE_ENABLE_SENSOR_DRIVER` | ✅ | ✅ | ✅ | ✅ | ~5KB |
| **SHT31/AHT20/BH1750** | `FASTBEE_ENABLE_SENSOR_DRIVER` | ✅ | ✅ | ✅ | ✅ | ~4KB |
| **NeoPixel LED** | `FASTBEE_ENABLE_LED_SCREEN` | ⚙️ | ⚙️ | ⚙️ | ⚙️ | ~10KB |
| **OLED/LCD** | `FASTBEE_ENABLE_LCD` | ⚙️ | ⚙️ | ⚙️ | ⚙️ | ~20KB |
| **TM1637数码管** | `FASTBEE_ENABLE_SEVEN_SEGMENT` | ⚙️ | ⚙️ | ⚙️ | ⚙️ | ~3KB |
| **BMP280/MPU6050** | `FASTBEE_ENABLE_I2C_SENSORS` | ❌ | ❌ | ❌ | ✅ | ~15KB |
| **MFRC522 RFID** | `FASTBEE_ENABLE_RFID` | ❌ | ❌ | ❌ | ✅ | ~12KB |
| **红外遥控** | `FASTBEE_ENABLE_IR_REMOTE` | ❌ | ❌ | ❌ | ✅ | ~8KB |
| **以太网W5500** | `FASTBEE_ENABLE_ETHERNET` | ❌ | ❌ | ❌ | ✅ | ~8KB |
| **4G蜂窝EC801E** | `FASTBEE_ENABLE_CELLULAR` | ❌ | ❌ | ❌ | ✅ | ~15KB |
| **LoRa透传** | `FASTBEE_ENABLE_LORA` | ❌ | ❌ | ❌ | ✅ | ~5KB |

**说明**: ✅默认启用 ❌默认禁用 ⚙️按需配置

---

## 附录B：引脚分配建议

### ESP32 推荐引脚分配

| 功能 | 推荐引脚 | 说明 |
|------|---------|------|
| **UART1** | GPIO9(TX)/GPIO10(RX) | 默认Serial1 |
| **UART2** | GPIO16(TX)/GPIO17(RX) | 默认Serial2 |
| **I2C** | GPIO21(SDA)/GPIO22(SCL) | 默认Wire |
| **SPI** | GPIO23(MOSI)/GPIO19(MISO)/GPIO18(SCK)/GPIO5(CS) | 默认VSPI |
| **ADC1** | GPIO32~39 | 推荐，不与WiFi冲突 |
| **DAC1/2** | GPIO25/26 | 仅ESP32支持 |
| **TOUCH** | GPIO0/2/4/12/13/14/15/27/32/33 | 仅ESP32支持 |
| **NeoPixel** | GPIO2/4/12/13/14/15/25/26/27 | 任意GPIO |
| **舵机PWM** | GPIO2/4/12/13/14/15/25/26/27 | 需支持PWM |

### 避免使用的引脚

| 引脚 | 原因 |
|------|------|
| GPIO6~11 | Flash芯片引脚，不可用 |
| GPIO34~39 | 仅支持输入，无内部上拉/下拉 |
| GPIO0 | 启动模式选择，慎用 |
| GPIO3 | 默认串口打印 |

### ESP32-S3 引脚差异

- **ADC1**: GPIO1~20支持
- **无DAC**: 不支持DAC输出
- **无TOUCH**: 不支持触摸功能
- **USB**: GPIO19(D-)/GPIO20(D+)用于USB OTG

---

## 附录C：资源消耗参考

### Flash占用参考（典型值）

| 模块/库 | Flash占用 | 备注 |
|---------|----------|------|
| 核心框架 | ~150KB | 包含网络、协议、Web服务器 |
| DHT库 | ~3KB | DHT11/DHT22 |
| OneWire+DallasTemperature | ~5KB | DS18B20 |
| U8g2 (OLED) | ~15KB | 字体和图形 |
| NeoPixel RMT驱动 | ~10KB | 轻量实现 |
| TM1637 bit-bang | ~3KB | 自写驱动 |
| BMP280库 | ~5KB | I2C传感器 |
| MPU6050库 | ~10KB | I2C传感器 |
| MFRC522库 | ~12KB | SPI RFID |
| IRremoteESP8266 | ~8KB | 红外解码 |
| Modbus库 | ~20KB | RTU主从 |
| MQTT (PubSubClient) | ~15KB | 消息协议 |
| TinyGSM (4G) | ~15KB | 蜂窝模块 |
| Ethernet W5500 | ~8KB | ESP-IDF驱动 |

### RAM占用参考（运行时）

| 模块 | RAM占用 | 备注 |
|------|---------|------|
| 核心框架 | ~30KB | 任务栈、缓冲区 |
| WiFi STA | ~15KB | 连接状态 |
| Web服务器 | ~20KB | 请求处理 |
| MQTT客户端 | ~5KB | 订阅缓冲 |
| 传感器缓存 | ~2KB | 最近读取值 |
| 规则引擎 | ~6KB | 规则列表、运行状态 |
| NeoPixel发送 | ~24×灯珠数 | 临时分配 |
| OLED帧缓冲 | 1KB | 128×64单色 |

### JSON文档分配

- **小型文档**: StaticJsonDocument<256>~<512>（简单响应）
- **中型文档**: StaticJsonDocument<1024>~<2048>（配置读写）
- **大型文档**: StaticJsonDocument<4096>~<8192>（完整配置加载）
- **动态文档**: JsonDocument（ArduinoJson v7自动管理）

---

## 快速配置导入

所有文档中的JSON配置示例均设置为 `"enabled": false`（禁用状态），使用步骤：

1. 复制JSON配置到 `data/config/peripherals.json` 的 `peripherals` 数组中
2. 通过Web界面"配置导入/导出"功能导入
3. 在Web界面中修改引脚号、启用状态等参数
4. 保存配置并重启设备生效

## 外设执行联动

所有传感器均可与外设执行系统联动：

1. **定时触发**: 按固定周期读取传感器数据
2. **数据源事件**: 传感器数据变化触发规则（`ds:periphId_field`）
3. **平台触发**: MQTT消息触发（如云端下发查询命令）
4. **动作执行**: 读取传感器(`ACTION_SENSOR_READ`)、控制GPIO(`ACTION_GPIO_WRITE`)、上报数据

详见 [外设执行文档](../system/periph-exec-management.md)

---

**文档版本**: 2025-02-01  
**适用固件**: v2.0+ (esp32/esp32s3-full)  
**维护者**: FastBee团队
