# 普中 ESP32 资料覆盖核对

核对来源：`C:\Users\kerwin\Desktop\03-项目资料\硬件开发资料\普中-ESP32开发板资料\ai` 下 57 个 Python 示例、20 张接线图和 `普中ESP32开发攻略_V1.2--基于MicroPython.pdf`。PDF 共 299 页，目录包含开发板功能、Python 基础、LED、蜂鸣器、继电器、按键、电机、ADC、显示、传感器、WiFi、Socket、MQTT 和手机控制 LED 等章节。

覆盖核对时，可以把资料中的模块映射到 Web 控制台的外设配置和执行规则：先确认是否已有外设类型，再确认是否能通过规则触发采集、控制或显示。

![外设配置列表](images/peripheral-management.png)

![新增外设弹窗](images/peripheral-add-dialog.png)

![外设执行规则列表](images/periph-exec-management.png)

截图要点：覆盖核对不只看“文档里有没有模块”，还要看 Web 端是否能配置、是否能被规则调度、是否能在现场被启用。外设配置截图用于查类型覆盖，新增弹窗用于查参数入口，外设执行截图用于查自动化能力。

![硬件资料覆盖矩阵](../images/hardware-coverage-matrix.svg)

覆盖矩阵用于快速判断某个硬件模块是否已经形成闭环：有示例文档、有外设类型、有 Web 配置入口、有规则联动路径，并且版本能力开关与目标固件档位一致。

## 总体结论

- 24 个基础实验和 20 个扩展实验已在 `docs/examples/01..44` 建立对应文档。
- 轻量模块可通过现有外设配置和外设执行覆盖：GPIO 输入/输出、ADC、PWM、继电器、蜂鸣器、按键、直流电机、步进电机、舵机、DHT11、DS18B20、HC-SR04、常见数字量/模拟量传感器、电流/电压传感器、OLED、TM1637、NeoPixel、Modbus RTU 子设备。
- 轻量 I2C 传感器已补充到精简固件：SHT31、AHT20、BH1750；高资源模块已按 `esp32s3-full` 分层：BMP280、MPU6050、MFRC522 RFID、红外遥控。
- 当前仍不是即插即用的模块：DS1302 三线 RTC、LCD1602(PCF8574)、SD 卡文件读写。文档已标注真实状态，可作为后续驱动扩展目标。
- PDF 明确提醒 WiFi 与 classic ESP32 ADC2 冲突；ADC、电流、电压类文档已要求生产环境优先使用 ADC1。

## 覆盖表

| 资料模块 | 当前实现方式 | 固件档位 | 文档 |
| --- | --- | --- | --- |
| LED 点亮/闪烁/流水灯 | GPIO 输出、脚本动作、定时触发 | slim/full | `docs/examples/01..03`、`docs/peripherals/gpio-output.md` |
| 蜂鸣器 | GPIO 输出或 PWM | slim/full | `docs/examples/04-buzzer.md` |
| 继电器 | GPIO 输出，支持反相动作 | slim/full | `docs/examples/05-relay.md`、`docs/peripherals/gpio-output.md` |
| 按键/外部中断 | 上拉/下拉输入、按键事件 | slim/full | `docs/examples/06-button-control.md`、`docs/examples/09-external-interrupt.md` |
| 直流电机 | GPIO/PWM 控制 | slim/full | `docs/examples/07-dc-motor.md` |
| 步进电机 | `STEPPER_MOTOR` 和 `ACTION_CALL_PERIPHERAL` | slim/full | `docs/examples/08-stepper-motor.md`、`docs/peripherals/stepper-motor.md` |
| 定时器 | 外设执行定时触发 | slim/full | `docs/examples/10-timer-interrupt.md`、`docs/periph-exec/triggers/timer-trigger.md` |
| PWM 呼吸灯 | PWM 输出、呼吸动作 | slim/full | `docs/examples/11-pwm-breathing.md`、`docs/peripherals/pwm-output.md` |
| 串口通信 | UART 外设、Modbus RTU | slim/full | `docs/examples/12-uart-communication.md`、`docs/protocols/modbus-rtu.md` |
| ADC/电位器 | `GPIO_ANALOG_INPUT` / `ADC` | slim/full | `docs/examples/13-adc-voltage.md`、`docs/peripherals/adc-input.md` |
| RGB 彩灯 | NeoPixel/RMT | slim/full | `docs/examples/14-rgb-neopixel.md`、`docs/peripherals/neopixel.md` |
| TM1637 数码管 | `SEVEN_SEGMENT_TM1637` | slim/full | `docs/examples/15-seven-segment.md`、`docs/peripherals/display-tm1637.md` |
| 系统 RTC/NTP | NTP 同步、定时规则 | slim/full | `docs/examples/16-rtc-clock.md` |
| DS1302 RTC | 未内置驱动，建议 NTP 或后续三线 RTC 驱动 | 待扩展 | `docs/examples/17-ds1302-rtc.md` |
| DS18B20 | OneWire 温度读取 | slim/full | `docs/examples/18-ds18b20-temperature.md`、`docs/peripherals/sensor-ds18b20.md` |
| DHT11/DHT22 | DHT 温湿度读取 | slim/full | `docs/examples/19-dht11-humidity.md`、`docs/peripherals/sensor-dht.md` |
| HC-SR04 | 超声波距离读取 | slim/full | `docs/examples/20-ultrasonic-distance.md`、`docs/peripherals/sensor-ultrasonic.md` |
| SHT31 | I2C 温湿度读取，轻量驱动 | slim/full | `docs/peripherals/sensor-sht31.md` |
| AHT20 | I2C 温湿度读取，轻量驱动 | slim/full | `docs/peripherals/sensor-aht20.md` |
| BH1750 | I2C 光照读取，轻量驱动 | slim/full | `docs/peripherals/sensor-bh1750.md` |
| 红外遥控 | IRremoteESP8266 事件驱动 | S3-full | `docs/examples/21-ir-remote.md`、`docs/peripherals/ir-remote.md` |
| 舵机 | PWM 舵机 | slim/full | `docs/examples/22-servo-sg90.md`、`docs/peripherals/servo.md` |
| OLED | SSD1306/SH1106 图形屏 | slim/full | `docs/examples/23-oled-display.md`、`docs/peripherals/display-oled.md` |
| SD 卡 | 配置占位，运行时读写驱动待补 | 待扩展，建议 S3-full | `docs/examples/24-sd-card.md`、`docs/peripherals/storage-sd-card.md` |
| 激光模块 | GPIO 输出 | slim/full | `docs/examples/25-laser-sensor.md` |
| 倾斜/震动/干簧管/对射光电/PIR/红外避障/寻迹 | 数字输入或上拉输入 | slim/full | `docs/examples/26..29`、`docs/examples/38`、`docs/examples/42`、`docs/examples/44` |
| 雨滴/声音/光敏/火焰/烟雾 | ADC 模拟输入，部分模块可用 DO 数字阈值 | slim/full | `docs/examples/30`、`docs/examples/32..35`、`docs/peripherals/adc-input.md` |
| PS2 摇杆 | 双 ADC + 按键输入 | slim/full | `docs/examples/31-ps2-joystick.md` |
| 触摸开关 | GPIO touch 或数字输入 | slim/full | `docs/examples/36-touch-sensor.md` |
| 旋转编码器 | `ENCODER` | slim/full | `docs/examples/37-rotary-encoder.md`、`docs/peripherals/encoder.md` |
| LCD1602(IIC) | PCF8574 字符屏驱动待补；当前 LCD 外设只覆盖图形屏 | 待扩展 | `docs/examples/39-lcd1602-display.md`、`docs/peripherals/display-lcd1602.md` |
| BMP280 | I2C 传感器驱动 | S3-full | `docs/examples/40-bmp280-pressure.md`、`docs/peripherals/sensor-bmp280.md` |
| MPU6050 | I2C 传感器驱动 | S3-full | `docs/examples/41-mpu6050-gyro.md`、`docs/peripherals/sensor-mpu6050.md` |
| MFRC522 RFID | SPI RFID 事件驱动 | S3-full | `docs/examples/43-rfid-mfrc522.md`、`docs/peripherals/rfid-mfrc522.md` |
| 电流型传感器 | ADC + 线性校准 | slim/full | `docs/peripherals/sensor-current.md` |
| 电压型传感器 | ADC + 分压比还原 | slim/full | `docs/peripherals/sensor-voltage.md` |
| WiFi/Socket/MQTT | WiFi 配网、MQTT、TCP/HTTP/CoAP 按档位启用 | MQTT slim/full，TCP/HTTP/CoAP S3-full | `docs/system/network-config.md`、`docs/protocols/mqtt-config.md` |

## 后续扩展建议

1. LCD1602：新增 HD44780 + PCF8574 字符屏驱动，复用 `LCD` type 36，并在参数里增加 `controller: "lcd1602"`、`address`、`cols`、`rows`。
2. SD 卡：新增 SPI SD/SDIO 挂载驱动和文件动作，例如 `file_append`、`file_read`、`file_delete`；建议只在 `esp32s3-full` 打开。
3. DS1302：新增三线 bit-bang RTC 驱动，或将其作为命令脚本/自定义驱动示例；联网场景继续优先 NTP。
4. 更多 I2C 传感器：SHT31/AHT20/BH1750 这类无第三方库、数据量小的模块可放入 slim；BME280、CCS811、SCD30、VL53L0X、INA219 等需要较多补偿算法或库依赖的模块建议优先放在 `esp32s3-full`。
