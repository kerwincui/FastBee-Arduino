# 示例文档

FastBee-Arduino 配置示例大全，涵盖基础 GPIO 操作、传感器接入、显示控制、通信协议及 Modbus 子设备接入。

> 推荐阅读：[支持的模块和传感器清单](../peripherals/supported-sensors-and-modules.md) - 先确认版本支持；[传感器与模块完整指南](../peripherals/sensor-guide-complete.md) - 查看详细配置参考

## 示例列表

### 基础实验（01-24）

| 编号 | 名称 | 核心外设类型 |
|------|------|-------------|
| [01](01-led-first.md) | LED 点亮 | GPIO_DIGITAL_OUTPUT |
| [02](02-led-blink.md) | LED 闪烁 | GPIO_DIGITAL_OUTPUT + periph-exec |
| [03](03-led-flowing.md) | LED 流水灯 | 多 GPIO + JavaScript 脚本 |
| [03b](03b-led-flowing-command-script.md) | LED 流水灯（命令脚本） | 多 GPIO + PERIPH/DELAY 命令序列 |
| [04](04-buzzer.md) | 蜂鸣器 | GPIO_DIGITAL_OUTPUT / PWM |
| [05](05-relay.md) | 继电器 | GPIO_DIGITAL_OUTPUT |
| [06](06-button-control.md) | 按键控制 | GPIO_DIGITAL_INPUT_PULLUP |
| [07](07-dc-motor.md) | 直流电机 | GPIO_DIGITAL_OUTPUT |
| [08](08-stepper-motor.md) | 步进电机 | STEPPER_MOTOR |
| [09](09-external-interrupt.md) | 外部中断 | GPIO_INTERRUPT |
| [10](10-timer-interrupt.md) | 定时触发 | TIMER_TRIGGER |
| [11](11-pwm-breathing.md) | PWM 呼吸灯 | GPIO_PWM_OUTPUT |
| [12](12-uart-communication.md) | 串口通信 | UART |
| [13](13-adc-voltage.md) | ADC 电压采集 | GPIO_ANALOG_INPUT |
| [14](14-rgb-neopixel.md) | RGB 灯带 | NEO_PIXEL |
| [15](15-seven-segment.md) | 数码管显示 | SEVEN_SEGMENT_TM1637 |
| [16](16-rtc-clock.md) | NTP 时钟 | 系统 NTP + 定时 |
| [17](17-ds1302-rtc.md) | DS1302 RTC | 外部 RTC |
| [18](18-ds18b20-temperature.md) | DS18B20 温度 | SENSOR(ONE_WIRE) |
| [19](19-dht11-humidity.md) | DHT11 温湿度 | SENSOR(DHT) |
| [20](20-ultrasonic-distance.md) | 超声波测距 | SENSOR(HC-SR04) |
| [21](21-ir-remote.md) | 红外遥控 | IR_REMOTE（S3-full） |
| [22](22-servo-sg90.md) | 舵机控制 | PWM_SERVO |
| [23](23-oled-display.md) | OLED 显示 | LCD(SSD1306) |
| [24](24-sd-card.md) | SD 卡读写 | SDIO 配置占位（驱动待扩展，建议 S3-full） |

### 扩展实验（25-44）

| 编号 | 名称 | 核心外设类型 |
|------|------|-------------|
| [25](25-laser-sensor.md) | 激光传感器 | GPIO_DIGITAL_OUTPUT |
| [26](26-tilt-sensor.md) | 倾斜传感器 | GPIO_DIGITAL_INPUT_PULLUP |
| [27](27-vibration-sensor.md) | 震动传感器 | GPIO_DIGITAL_INPUT |
| [28](28-reed-switch.md) | 干簧管 | GPIO_DIGITAL_INPUT |
| [29](29-photoelectric-sensor.md) | 对射光电 | GPIO_DIGITAL_INPUT |
| [30](30-rain-sensor.md) | 雨滴传感器 | ADC + GPIO |
| [31](31-ps2-joystick.md) | PS2 摇杆 | 双 ADC + 按键 |
| [32](32-sound-sensor.md) | 声音传感器 | GPIO_ANALOG_INPUT |
| [33](33-light-sensor.md) | 光敏传感器 | GPIO_ANALOG_INPUT |
| [34](34-flame-sensor.md) | 火焰传感器 | GPIO_ANALOG_INPUT |
| [35](35-smoke-sensor.md) | 烟雾传感器 MQ-2 | GPIO_ANALOG_INPUT |
| [36](36-touch-sensor.md) | 触摸传感器 | GPIO_TOUCH |
| [37](37-rotary-encoder.md) | 旋转编码器 | ENCODER |
| [38](38-ir-obstacle.md) | 红外避障 | GPIO_DIGITAL_INPUT |
| [39](39-lcd1602-display.md) | LCD1602 显示 | LCD 配置占位（PCF8574 驱动待扩展） |
| [40](40-bmp280-pressure.md) | BMP280 气压 | I2C SENSOR（S3-full） |
| [41](41-mpu6050-gyro.md) | MPU6050 陀螺仪 | I2C SENSOR（S3-full） |
| [42](42-ir-tracking.md) | 红外寻迹 | GPIO_DIGITAL_INPUT |
| [43](43-rfid-mfrc522.md) | RFID 射频卡 | SPI SENSOR（S3-full） |
| [44](44-pir-motion.md) | 人体红外 PIR | GPIO_DIGITAL_INPUT |

### Modbus 子设备（45-48）

| 编号 | 名称 | 核心外设类型 |
|------|------|-------------|
| [45](45-modbus-temp-humidity.md) | Modbus 温湿度传感器 | UART(Modbus RTU) |
| [46](46-modbus-stepper-motor.md) | Modbus 步进电机控制 | UART(Modbus RTU) |
| [47](47-modbus-relay.md) | Modbus 继电器模块 | UART(Modbus RTU) |
| [48](48-modbus-soil-sensor.md) | Modbus 土壤传感器 | UART(Modbus RTU) |

### 射频与雷达（49-50）

| 编号 | 名称 | 核心外设类型 |
|------|------|-------------|
| [49](49-433mhz-rf-module.md) | 433MHz 射频模块 | RF_MODULE |
| [50](50-radar-sensor.md) | 雷达感应模块 | RADAR_SENSOR |

## 最佳实践

### 引脚分配原则

- **GPIO34-39**: 仅输入,适合 ADC/数字输入
- **GPIO6-11**: 连接 Flash,不建议使用
- **GPIO32-39**: ADC1,WiFi 开启后可用 (优先使用)
- **GPIO4,12-15,25-27**: ADC2,WiFi 开启后不可用
- **GPIO0**: BOOT 按键,谨慎使用
- **GPIO5**: STATE 指示灯,默认占用

### 传感器配置建议

- **DHT11/DHT22**: 采样间隔 >= 2 秒,避免读取过快导致数据异常
- **DS18B20**: 单总线可并联多个传感器,通过地址区分
- **I2C 设备**: 可共用 SDA/SCL,但 I2C 地址不能冲突
- **超声波 HC-SR04**: 触发引脚和回声引脚需不同,采样间隔 >= 100ms
- **BMP280/MPU6050**: 仅 full 版支持,需较多内存

### 外设执行规则设计

- **定时采集**: intervalSec >= 5 秒,避免频繁读取传感器
- **事件触发**: 先确认采集规则已生成 `ds:<id>_<field>` 数据源
- **联动控制**: 注意继电器有效电平 (高有效/低有效)
- **脚本执行**: 仅 full 版本支持完整 RuleScript,slim 版使用命令脚本
- **同步延时**: 动作间增加 syncDelayMs (100-500ms) 避免并发冲突

## 常见场景

### 场景 1: 温湿度监控报警

**需求**: 温度超过 30°C 时打开风扇 (继电器)

**步骤**:
1. 配置 DHT11 传感器 (type: 38, 引脚: 13)
2. 配置继电器 (type: 12, 引脚: 25)
3. 创建定时采集规则 (每 10 秒读取 DHT11)
   - 触发器: timerTrigger, intervalSec: 10
   - 动作 1: sensorRead (dht_01, temperature)
4. 创建事件触发规则 (温度 > 30°C)
   - 触发器: eventTrigger, eventId: `ds:dht_01_temperature`, operator: >, value: 30
   - 动作: GPIO 高电平 (relay_01)
5. 上报 MQTT 云平台 (reportAfterExec: true)

**参考示例**: [19-dht11-humidity.md](19-dht11-humidity.md), [05-relay.md](05-relay.md)

### 场景 2: 光照自动开灯

**需求**: 光照低于 50 lux 时自动打开 LED

**步骤**:
1. 配置 BH1750 光照传感器 (type: 38, I2C 地址: 0x23)
   - 或使用光敏电阻 ADC (type: 15, 引脚: 34)
2. 配置 LED (type: 12, 引脚: 26)
3. 定时读取光照值 (每 5 秒)
   - 动作: sensorRead (bh1750_01, illuminance)
4. 事件触发: 光照 < 50 lux
   - 触发器: eventTrigger, eventId: `ds:bh1750_01_illuminance`, operator: <, value: 50
   - 动作: GPIO 高电平 (led_01)
5. 延时关闭或再次检测 (避免频繁开关)

**参考示例**: [33-light-sensor.md](33-light-sensor.md), [01-led-first.md](01-led-first.md)

### 场景 3: Modbus 设备控制

**需求**: 通过 Modbus RTU 控制温湿度传感器和继电器

**步骤**:
1. 配置 UART 为 Modbus RTU (type: 1, 引脚: TX-17, RX-16)
2. 扫描从站设备 (地址 1-247)
3. 映射寄存器到数据点
   - 保持寄存器 40001-40002: 温度
   - 保持寄存器 40003-40004: 湿度
4. 创建轮询规则 (每 10 秒)
   - 触发器: pollTrigger, 读取保持寄存器
   - 动作: 解析数据,写入 sensor_cache
5. 条件触发控制命令
   - 事件触发: 温度 > 35°C
   - 动作: 写入 Modbus 线圈 (打开继电器)

**参考示例**: [45-modbus-temp-humidity.md](45-modbus-temp-humidity.md), [47-modbus-relay.md](47-modbus-relay.md)

### 场景 4: 本地显示监控

**需求**: OLED 屏幕显示温度和湿度,每秒刷新

**步骤**:
1. 配置 DHT11 传感器 (type: 38)
2. 配置 OLED 显示屏 (type: 36, I2C 地址: 0x3C)
3. 定时读取传感器数据 (每 2 秒)
   - 动作 1: sensorRead (dht_01, temperature)
   - 动作 2: sensorRead (dht_01, humidity)
4. 显示动作输出
   - 动作 3: displayOLED, 显示 "温度: 28.5°C"
   - 动作 4: displayOLED, 显示 "湿度: 65%"
5. 或使用 TM1637 数码管轮换显示
   - 动作 3: displayTM1637, 显示温度 2 秒
   - 动作 4: displayTM1637, 显示湿度 2 秒

**参考示例**: [23-oled-display.md](23-oled-display.md), [15-seven-segment.md](15-seven-segment.md)

### 场景 5: 按键多功能控制

**需求**: 单按键实现短按开关灯、长按切换模式

**步骤**:
1. 配置按键 (type: 13, GPIO_DIGITAL_INPUT_PULLUP, 引脚: 0)
2. 配置 LED (type: 12, 引脚: 26)
3. 创建事件触发规则 (按键按下)
   - 触发器: eventTrigger, eventId: `button:btn_01:click`
   - 动作: 切换 LED 状态 (HIGH/LOW)
4. 创建长按规则
   - 触发器: eventTrigger, eventId: `button:btn_01:long_press`
   - 动作: 切换模式 (自动/手动)

**参考示例**: [06-button-control.md](06-button-control.md)

### 场景 6: 电流过载保护

**需求**: 电流超过 15A 时自动断电并报警

**步骤**:
1. 配置 ACS712 电流传感器 ADC (type: 15, 引脚: 34)
2. 配置继电器 (type: 12, 引脚: 25)
3. 定时读取电流值 (每 1 秒)
   - 动作: sensorRead (current_01, current)
   - 高级参数: sensitivity, zeroOffset, vRef, adcMax
4. 事件触发: current > 15
   - 动作 1: GPIO 低电平 (关闭继电器)
   - 动作 2: triggerEvent, 上报 `over_current` 事件
   - 动作 3: MQTT 上报告警消息

**参考示例**: [13-adc-voltage.md](13-adc-voltage.md)

## 外设类型速查表

| type 值 | 类型名 | 说明 |
|---------|--------|------|
| 1 | UART | 串口 / Modbus RTU |
| 11 | GPIO_DIGITAL_INPUT | 数字输入 |
| 12 | GPIO_DIGITAL_OUTPUT | 数字输出 |
| 13 | GPIO_DIGITAL_INPUT_PULLUP | 上拉输入（按键） |
| 15 | GPIO_ANALOG_INPUT | ADC 模拟输入 |
| 17 | GPIO_PWM_OUTPUT | PWM 输出 |
| 18 | GPIO_INTERRUPT_RISING | 上升沿中断 |
| 19 | GPIO_INTERRUPT_FALLING | 下降沿中断 |
| 20 | GPIO_INTERRUPT_CHANGE | 双沿中断 |
| 21 | GPIO_TOUCH | 触摸输入 |
| 36 | LCD | 显示屏（当前支持 SSD1306/SH1106，LCD1602 待扩展） |
| 37 | SDIO | SD 卡接口配置占位 |
| 38 | SENSOR | 传感器（DHT/DS18B20/BMP280等） |
| 41 | PWM_SERVO | 舵机 |
| 42 | STEPPER_MOTOR | 步进电机 |
| 43 | ENCODER | 旋转编码器 |
| 44 | ONE_WIRE | 单总线（DS18B20） |
| 45 | NEO_PIXEL | WS2812B RGB 灯带 |
| 47 | SEVEN_SEGMENT_TM1637 | TM1637 数码管 |
| 48 | RF_MODULE | 433MHz 射频发射/接收电平 |
| 49 | RADAR_SENSOR | RCWL-0516 / 5.8GHz 雷达感应 |

## 固件兼容性

| 标记 | 含义 |
|------|------|
| Lite / Standard / Full | 各版本均支持，具体芯片差异见支持清单 |
| Standard / Full | 需要标准版或完整功能版；红外遥控当前仅 Standard |

## 相关文档

- [外设管理](../system/peripheral-management.md) — 添加和配置外设
- [外设执行管理](../system/periph-exec-management.md) — 规则联动配置
- [外设文档](../peripherals/README.md) — 各外设类型详细说明
- [Modbus RTU 协议](../protocols/modbus-rtu.md) — Modbus 通信配置
