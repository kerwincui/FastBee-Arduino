# FastBee-Arduino 外设配置指南

> 本文档基于源码 `include/core/PeripheralTypes.h`、`src/core/PeripheralManager.cpp`、`data/config/peripherals.json`、`include/core/FeatureFlags.h`、`include/core/interfaces/ISensorDriver.h`、`include/core/DriverRegistry.h` 和 `platformio.ini` 整理，用于说明外设配置系统支持的所有类型、引脚要求、参数含义、支持状态以及最佳实践。

## 当前版本提示

- 默认 `esp32`、`esp32c3`、`esp32s3` 构建为精简版，保留 UART/I2C/SPI、GPIO、传感器、OLED/LCD、TM1637、Modbus 子设备等核心外设能力。
- 精简版默认关闭 NeoPixel/LED 屏、BLE、OTA、文件管理、日志查看、多用户/角色和 RuleScript；如需验证完整能力，请使用 `esp32s3-full`。
- 外设配置保存到 `/config/peripherals.json`，可在 Web 的“配置导入/导出”中按“外设配置”单独备份和恢复。
- 传感器数据要作为外设执行事件触发来源时，需要先在外设执行中配置传感器采集动作，系统会将采集结果缓存为 `ds:<外设ID>_<字段>` 数据源。
- Modbus 子设备由通信协议页管理，作为虚拟外设参与设备控制和外设执行，不占用本地 GPIO。

## 目录

- [1. 总览](#1-总览)
- [2. 外设类型 ID 映射总表](#2-外设类型-id-映射总表)
- [3. 通信接口](#3-通信接口)
- [4. GPIO 接口](#4-gpio-接口)
- [5. 模拟信号接口](#5-模拟信号接口)
- [6. 调试接口](#6-调试接口)
- [7. 专用外设](#7-专用外设)
- [8. Modbus 外设](#8-modbus-外设)
- [9. 虚拟/逻辑外设](#9-虚拟逻辑外设)
- [10. 配置结构与字段说明](#10-配置结构与字段说明)
- [11. 功能编译开关（FeatureFlags）](#11-功能编译开关featureflags)
- [12. 常见问题与排错](#12-常见问题与排错)
- [13. 最佳实践](#13-最佳实践)
- [14. 传感器驱动扩展（ISensorDriver）](#14-传感器驱动扩展isensordriver)

---

## 1. 总览

外设配置系统采用"**类型驱动 + 引脚占用校验 + 运行时状态**"的三层模型：

- **配置层**：`data/config/peripherals.json`，冷启动从 LittleFS 加载。
- **管理层**：[`PeripheralManager`](../src/core/PeripheralManager.cpp) 单例，负责 CRUD、引脚冲突检测、硬件初始化/释放、通用读写接口。
- **驱动层**：按类型分派到底层：
  - GPIO 系列 → `pinMode/digitalWrite/ledcWrite`
  - I²C/SPI/UART → `Wire/SPI/Serial`
  - LCD/OLED → U8g2（`LCDManager`）
  - TM1637 → 自写 bit-bang（`SevenSegmentDriver`）
  - Modbus → 通过回调委托给 `ModbusHandler`

**约定**：所有硬件操作统一经过 `PeripheralManager`，禁止在业务代码里直接 `pinMode`/`digitalWrite`，以避免引脚占用冲突和状态不一致。

---

## 2. 外设类型 ID 映射总表

| ID  | 枚举常量 | 分类 | 显示名（中文） | 实现状态 | 引脚数 |
| ---:| --- | --- | --- | :---: | :---: |
| 0   | `UNCONFIGURED` | —— | 未配置 | 占位 | 0 |
| 1   | `UART` | 通信接口 | 串口 | ✅ 已实现 | 2 (RX,TX) |
| 2   | `I2C` | 通信接口 | I2C 总线 | ✅ 已实现 | 2 (SDA,SCL) |
| 3   | `SPI` | 通信接口 | SPI 总线 | ✅ 已实现 | 4 (MISO,MOSI,SCK,CS) |
| 4   | `CAN` | 通信接口 | CAN 总线 | ⚠️ 配置框架就绪，驱动 TODO | 2 |
| 5   | `USB` | 通信接口 | USB 接口 | ⚠️ 配置框架就绪，驱动 TODO | 2 |
| 11  | `GPIO_DIGITAL_INPUT` | GPIO | 数字输入 | ✅ 已实现 | 1 |
| 12  | `GPIO_DIGITAL_OUTPUT` | GPIO | 数字输出 | ✅ 已实现 | 1 |
| 13  | `GPIO_DIGITAL_INPUT_PULLUP` | GPIO | 数字输入(上拉) | ✅ 已实现 / 按键默认类型 | 1 |
| 14  | `GPIO_DIGITAL_INPUT_PULLDOWN` | GPIO | 数字输入(下拉) | ✅ 已实现 | 1 |
| 15  | `GPIO_ANALOG_INPUT` | GPIO | 模拟输入 | ✅ 已实现（`analogRead`） | 1 |
| 16  | `GPIO_ANALOG_OUTPUT` | GPIO | 模拟输出 | ✅ 已实现（LEDC PWM 模拟） | 1 |
| 17  | `GPIO_PWM_OUTPUT` | GPIO | PWM 输出 | ✅ 已实现（LEDC） | 1 |
| 18  | `GPIO_INTERRUPT_RISING` | GPIO | 中断(上升沿) | 🟡 ISR 骨架存在，事件队列 TODO | 1 |
| 19  | `GPIO_INTERRUPT_FALLING` | GPIO | 中断(下降沿) | 🟡 ISR 骨架存在，事件队列 TODO | 1 |
| 20  | `GPIO_INTERRUPT_CHANGE` | GPIO | 中断(变化) | 🟡 ISR 骨架存在，事件队列 TODO | 1 |
| 21  | `GPIO_TOUCH` | GPIO | 电容触摸 | ⚠️ 芯片依赖；ESP32 classic 支持 | 1 |
| 26  | `ADC` | 模拟信号 | ADC | ✅ 已实现 | 1 |
| 27  | `DAC` | 模拟信号 | DAC | ✅ 已实现（仅 GPIO25/26，ESP32 classic） | 1 |
| 31  | `JTAG` | 调试 | JTAG 调试 | 🔘 仅类型标记，芯片级固定 | 4 |
| 32  | `SWD` | 调试 | SWD 调试 | 🔘 仅类型标记，芯片级固定 | 2 |
| 36  | `LCD` | 专用外设 | **LCD/OLED 显示屏** | ✅ 已实现（U8g2：SSD1306/SH1106 等） | 2+ |
| 37  | `SDIO` | 专用外设 | SD 卡接口 | ⚠️ 配置框架就绪，驱动 TODO | 6 |
| 38  | `SENSOR` | 专用外设 | 通用传感器 (DHT/DS18B20) | ✅ 已实现（`FASTBEE_ENABLE_SENSOR_DRIVER`） | 1 |
| 39  | `CAMERA` | 专用外设 | 摄像头 | ⚠️ 配置框架就绪，驱动 TODO | 8 |
| 40  | `ETHERNET` | 专用外设 | 以太网 | ⚠️ 配置框架就绪，驱动 TODO | 4 |
| 41  | `PWM_SERVO` | 专用外设 | 舵机 | ✅ 已实现（LEDC） | 1 |
| 42  | `STEPPER_MOTOR` | 专用外设 | 步进电机 | ✅ 已实现（ULN2003 四相半步，非阻塞 Ticker） | 4 (IN1,IN2,IN3,IN4) |
| 43  | `ENCODER` | 专用外设 | 编码器 | ⚠️ 配置框架就绪，驱动 TODO | 2 |
| 44  | `ONE_WIRE` | 专用外设 | 单总线 | ✅ 通过 SENSOR 驱动链路实现（DS18B20） | 1 |
| 45  | `NEO_PIXEL` | 专用外设 | **LED 灯带 (NeoPixel)** | 🔒 默认禁用（`FASTBEE_ENABLE_LED_SCREEN=0`） | 1 |
| 46  | `RESERVED_46` | 兼容保留 | 保留位 | 🔒 旧版蜂鸣器类型占位，UI 不再展示 | 0 |
| 47  | `SEVEN_SEGMENT_TM1637` | 专用外设 | TM1637 4 位数码管 | ✅ 已实现（`FASTBEE_ENABLE_SEVEN_SEGMENT`） | 2 (CLK,DIO) |
| 51  | `MODBUS_DEVICE` | Modbus | Modbus 子设备 | ✅ 已实现（不占 GPIO） | 0 |
| 60  | `DEVICE_EVENT` | 虚拟 | 设备事件发射源 | ✅ 已实现（无硬件） | 0 |

**图例**：✅ 完整实现且默认启用 / 🟡 部分实现 / ⚠️ 仅配置占位 / 🔒 默认禁用 / 🔘 仅类型标记

---

## 3. 通信接口

### 3.1 UART (type=1)

| 字段 | 含义 | 取值 |
| --- | --- | --- |
| `pins[0]` | RX（接收） | 任意有效 GPIO |
| `pins[1]` | TX（发送） | 任意有效 GPIO |
| `params.baudRate` | 波特率 | 1 ~ 5000000，常用 9600/115200 |
| `params.dataBits` | 数据位 | 5~8 |
| `params.stopBits` | 停止位 | 1 或 2 |
| `params.parity` | 校验位 | 0=无 / 1=奇 / 2=偶 |

**示例**：
```json
{ "id": "uart0", "name": "串口0-调试", "type": 1, "enabled": true,
  "pins": [1, 3],
  "params": { "baudRate": 115200, "dataBits": 8, "stopBits": 1, "parity": 0 } }
```

**注意**：ESP32 默认 `Serial` 映射 GPIO1/3（USB 调试）。Modbus RTU 通常使用 `Serial2`（GPIO16/17）。

### 3.2 I2C (type=2)

| 字段 | 含义 | 取值 |
| --- | --- | --- |
| `pins[0]` | SDA | 推荐 GPIO21 |
| `pins[1]` | SCL | 推荐 GPIO22 |
| `params.frequency` | 时钟频率 | 仅支持 100000 / 400000 / 1000000 |
| `params.address` | 从机地址 | 0~127，主机模式为 0 |
| `params.isMaster` | 是否主机 | `true`（推荐） |

**示例**：
```json
{ "id": "i2c", "name": "I2C总线", "type": 2, "enabled": false,
  "pins": [21, 22],
  "params": { "frequency": 100000, "address": 0, "isMaster": true } }
```

### 3.3 SPI (type=3)

| 字段 | 含义 |
| --- | --- |
| `pins[0]` | MISO |
| `pins[1]` | MOSI |
| `pins[2]` | SCK |
| `pins[3]` | CS |
| `params.frequency` | 1 ~ 80 MHz |
| `params.mode` | 0~3 |
| `params.msbFirst` | `true` = MSB 先 |

### 3.4 CAN / USB（未实现驱动）

配置数据会被保存，但 `setupHardware` 会打印 `not yet implemented`。使用前需业务层自行实现。

---

## 4. GPIO 接口

### 4.1 数字输入 / 输出 (type=11~14)

| 类型 | 说明 | 典型用途 |
| --- | --- | --- |
| 11 `GPIO_DIGITAL_INPUT` | 高阻输入，外部需上/下拉 | 编码器 A/B 相 |
| 12 `GPIO_DIGITAL_OUTPUT` | 推挽输出 | 继电器、LED |
| 13 `GPIO_DIGITAL_INPUT_PULLUP` | 内部上拉 | 按键（按下接 GND） |
| 14 `GPIO_DIGITAL_INPUT_PULLDOWN` | 内部下拉 | 按键（按下接 VCC） |

> **按键事件**：只有 type=13/14 会被 [`PeriphExecScheduler::checkButtonEvents`](../src/core/PeriphExecScheduler.cpp) 扫描，支持 `button_click` / `button_double_click` / `button_long_press_2s/5s/10s`。

### 4.2 模拟输入 (type=15) / ADC (type=26)

- ESP32 classic：ADC1 (GPIO32~39) 推荐用于 WiFi 同时工作；ADC2 与 WiFi 冲突。
- `analogRead(pin)` 返回 0~4095（12 位）。

### 4.3 PWM 输出 (type=17) / 模拟输出 (type=16) / 舵机 (type=41)

| 参数 | 说明 | 约束 |
| --- | --- | --- |
| `pwmChannel` | LEDC 通道 | 0 ~ `CHIP_MAX_PWM_CH-1`（ESP32 为 16） |
| `pwmFrequency` | 频率 Hz | `freq × 2^resolution ≤ 80MHz` |
| `pwmResolution` | 分辨率位数 | 1~16 |
| `defaultDuty` | 默认占空比 | 0 ~ (2^resolution - 1) |

**频率/分辨率组合限制**（示例）：
- 1kHz × 13 位 = 8.192M ✅
- 40kHz × 12 位 = 163.84M ❌

### 4.4 中断 (type=18/19/20)

当前实现：`isrHandler` 仅记录触发引脚号，**尚未接入 FreeRTOS 队列分发给主循环**。上层业务使用按键事件（type=13/14）替代。

### 4.5 触摸 (type=21)

仅 `CHIP_HAS_TOUCH` 芯片有效（ESP32 classic 支持 T0~T9）。

---

## 5. 模拟信号接口

### 5.1 DAC (type=27)

- **硬件限制**：ESP32 classic 仅 GPIO25/26 有真实 DAC；其他芯片/引脚会被拒绝。
- 输出范围：8 位（0~255），对应 0V~VDD。

### 5.2 ADC (type=26)

与 `GPIO_ANALOG_INPUT` 等价，可用 `params.attenuation`（0~3）与 `params.resolution`（9~12）区分。

---

## 6. 调试接口

`JTAG=31` / `SWD=32` 为类型标记位。ESP32 的 JTAG 引脚固定为 GPIO12~15，开启后会影响这些引脚的普通 GPIO 功能。

---

## 7. 专用外设

### 7.1 LCD/OLED 显示屏 (type=36)

> **实现状态**：✅ 完整实现。默认 `esp32` 环境启用（`FASTBEE_ENABLE_LCD=1`）。

**支持的控制器**（由 U8g2 库覆盖）：
- SSD1306（128×64 / 128×32 OLED，I²C 0x3C）
- SH1106（128×64 OLED）
- 其他 U8g2 支持的字符/图形 LCD

**参数**：

| 字段 | 含义 | 取值 |
| --- | --- | --- |
| `pins[0]` | SDA（I²C）/ MOSI（SPI） | |
| `pins[1]` | SCL（I²C）/ SCK（SPI） | |
| `pins[2]` | CS（仅 SPI） | 可选 |
| `pins[3]` | DC（仅 SPI） | 可选 |
| `params.width` | 宽 | 128 |
| `params.height` | 高 | 64 / 32 |
| `params.interface` | 接口 | 0=并口 / 1=SPI / 2=I2C（默认） |

**示例**：
```json
{ "id": "oled_display", "name": "OLED显示屏", "type": 36, "enabled": true,
  "pins": [23, 22],
  "params": { "width": 128, "height": 64, "interface": 2 } }
```

**相关规则动作**：`ACTION_DISPLAY_CUSTOM`（OLED 自定义显示，支持多行文本 + 变量插值）。详见 [`oled_usage_guide.md`](./oled_usage_guide.md)。

### 7.2 TM1637 数码管 (type=47)

> **实现状态**：✅ 已实现，自写 bit-bang 驱动（`SevenSegmentDriver`），默认 `esp32` 环境启用。

| 字段 | 含义 |
| --- | --- |
| `pins[0]` | CLK |
| `pins[1]` | DIO |
| `params.brightness` | 亮度 0~7 |

**约束**：CLK/DIO 与其他外设不可共用引脚。由按键引脚冲突的历史教训，务必为数码管单独分配 GPIO。

### 7.3 通用传感器 (type=38) / 单总线 (type=44)

| 子类 | 引脚 | 说明 |
| --- | --- | --- |
| DHT11/DHT22 | 1 个 DATA | 温湿度 |
| DS18B20 | 1 个 DQ（1-Wire） | 温度 |

通过外设执行动作 `ACTION_SENSOR_READ` 读取并缓存。需启用 `FASTBEE_ENABLE_SENSOR_DRIVER`（默认开）。

### 7.4 保留位 (type=46)

旧版专用蜂鸣器类型已移除，`type=46` 仅作为历史编号保留，Web 不再提供新增入口。

### 7.5 NeoPixel 灯带 (type=45)

> **实现状态**：🔒 默认禁用（`FASTBEE_ENABLE_LED_SCREEN=0`）。启用需：
> 1. `platformio.ini` 添加 `-DFASTBEE_ENABLE_LED_SCREEN=1`
> 2. 添加 `Adafruit NeoPixel` 库依赖

### 7.6 舵机 (type=41)

使用 LEDC 50Hz PWM，脉宽 0.5ms~2.5ms 对应 0°~180°。

### 7.7 步进电机 (type=42)

面向 28BYJ-48 + ULN2003 一类四相步进电机驱动板，`pins[0..3]` 按顺序接 ULN2003 的 `IN1, IN2, IN3, IN4`。驱动采用 8 拍半步序列，通过 `Ticker` 非阻塞输出，不会在外设执行动作里长时间阻塞 Web 服务。

| 字段 | 含义 | 默认值 / 范围 |
| --- | --- | --- |
| `pins[0]` | IN1 | 有效 GPIO |
| `pins[1]` | IN2 | 有效 GPIO |
| `pins[2]` | IN3 | 有效 GPIO |
| `pins[3]` | IN4 | 有效 GPIO |
| `params.stepsPerRevolution` | 每圈步数 | 默认 `2048` |
| `params.speed` | 默认转速 RPM | 默认 `8`，最大 `30` |

外设执行可通过 `ACTION_CALL_PERIPHERAL` 控制该外设，支持 `forward`、`reverse`、`stop`、`faster`、`slower`、`setSpeed`、`direction` 等动作。

> 安全提醒：经典 ESP32 的 GPIO9/10/11 通常被 Flash SPI 占用，固件会拒绝在当前芯片保留引脚上启用步进电机，避免误配置导致重启。GPIO 11/10/9/13 更适合 ESP32-S3；经典 ESP32 建议换成空闲 GPIO。

### 7.8 SDIO / 摄像头 / 以太网 / 编码器

当前仅保存配置，**底层驱动未实现**。需要使用请自行扩展 `PeripheralManager::setupHardware`。

---

## 8. Modbus 外设

### 8.1 `MODBUS_DEVICE` (type=51)

虚拟外设，**不占用本地 GPIO**，通过 RS485 / Modbus TCP 总线通信。

| `params.modbus.*` 字段 | 含义 | 取值 |
| --- | --- | --- |
| `slaveAddress` | 从站地址 | 1~247 |
| `deviceType` | 设备类型 | 0=继电器 / 1=PWM / 2=PID 等 |
| `controlProtocol` | 控制协议 | 0=线圈 FC05 / 1=寄存器 FC06 |
| `coilBase` | 线圈基地址 | |
| `pwmRegBase` | PWM 寄存器基地址 | |
| `ncMode` | 常闭模式（状态反转） | bool |

**注意事项**：
- Modbus 外设不通过 `peripherals.json` 持久化，由 `protocol.json` 统一管理。
- 物理总线（RS485）需要一条 UART + DE 控制引脚，**DE 引脚不可被按键或其他 GPIO 外设复用**。
- 参见历史 Lesson：[Modbus dePin 与按键引脚不可复用](#12-常见问题与排错)。

---

## 9. 虚拟/逻辑外设

### 9.1 `DEVICE_EVENT` (type=60)

- **无引脚、无硬件**。
- 仅作为规则系统中的"事件发射源"，触发后通过 MQTT `DEVICE_EVENT` 主题上报。
- 配置时 `pinCount=0`，`pins` 可省略，校验只校验 `id`/`name` 非空。

**典型用途**：系统状态变更、用户动作、组合事件（如"连续三次按键单击触发警报"）。

---

## 10. 配置结构与字段说明

`peripherals.json` 顶级结构：

```jsonc
{
  "peripherals": [
    {
      "id": "唯一ID（英文/数字/下划线）",
      "name": "显示名",
      "type": 36,              // PeripheralType 枚举值
      "enabled": true,         // 仅 enabled=true 的外设会占用引脚
      "pins": [21, 22],        // 按顺序排列，未使用位设为 255 或省略
      "params": {              // 类型特定参数（见各章）
        "width": 128,
        "height": 64,
        "interface": 2
      }
    }
  ]
}
```

### 10.1 通用约束

- `id` 必须全局唯一；改名（name）可直接 PUT，改 ID 需先 DELETE 后 POST。
- `pins[]` 最大 8 个，`pinCount` 自动从非 255 的数量推断。
- **禁用 (`enabled=false`) 的外设不占引脚**——允许多外设声明同一引脚但只启用其中一个。
- 保留引脚（Flash SPI、Boot、USB D+/D-）由 [`ChipConfig.h`](../include/core/ChipConfig.h) 定义，`validatePinForType` 会拒绝越界。

### 10.2 引脚冲突检测

- 增加或启用外设时，`checkPinConflict` 会扫描所有**已启用**的非 Modbus 外设。
- 若检测到残留缓存（`pinToPeripheral` 与 `peripherals` 不一致），会自动 `rebuildPinMapping` 一次并重试。

---

## 11. 功能编译开关（FeatureFlags）

关键开关位于 [`include/core/FeatureFlags.h`](../include/core/FeatureFlags.h)，可在 `platformio.ini` 的 `build_flags` 中覆盖：

| 宏 | 默认值 | 说明 |
| --- | :---: | --- |
| `FASTBEE_ENABLE_LCD` | 0（esp32=1） | U8g2 LCD/OLED 驱动 |
| `FASTBEE_ENABLE_SEVEN_SEGMENT` | 0（esp32=1） | TM1637 驱动 |
| `FASTBEE_ENABLE_LED_SCREEN` | 0 | NeoPixel 灯带 |
| `FASTBEE_ENABLE_SENSOR_DRIVER` | 1 | DHT/DS18B20 |
| `FASTBEE_ENABLE_MODBUS` | 1 | Modbus RTU/TCP |
| `FASTBEE_ENABLE_PERIPH_EXEC` | 1 | 外设执行规则（定时/按键/事件） |

### 11.1 预设环境（来自 `platformio.ini`）

| 预设 | LCD | TM1637 | NeoPixel | CoAP |
| --- | :---: | :---: | :---: | :---: |
| `esp32` 默认 | ✅ | ✅ | ❌ | ❌ |
| `minimal` | ❌ | ❌ | ❌ | ❌ |
| `full` | ✅ | ✅ | ✅ | ✅ |

---

## 12. 常见问题与排错

### 12.1 "引脚 X 已被外设 '<未知>' 占用"

**原因**：`pinToPeripheral` 缓存残留（已在 `addPeripheral` 中加入自动 `rebuildPinMapping` 回退）。
**排查**：
1. 确认 `peripherals.json` 中是否真的存在该引脚的启用外设。
2. 串口观察日志 `stale pin mapping detected, rebuilding cache`。
3. 若仍报冲突，说明目标外设确实启用中，需先禁用或改引脚。

### 12.2 按键事件长时间运行后失效

**历史根因**（已修复）：
- `dispatchAsync` 自愈条件盲区（`startTime` 缺失时永久 skip）。
- `dispatchByRuleId` 锁超时无重试，`checkTimerTriggers` 持锁 100ms 期间按键事件被丢弃。

**当前实现**：`dispatchAsync` 检测 `startTimeMissing || stuck>60s` 自动清理；`dispatchByRuleId` 3 次 × 50ms 重试。详见 Lesson：`FastBee-Arduino按键失效自愈机制修复`。

### 12.3 TM1637 与按键共用引脚

**禁止**。TM1637 bit-bang 驱动会频繁翻转 CLK/DIO，与按键扫描冲突导致双向失效。
**方案**：为 TM1637 单独分配 2 个 GPIO（典型 GPIO18/19）。

### 12.4 Modbus dePin 与按键/GPIO 冲突

Modbus RTU 的 RS485 DE（Direction Enable）引脚由 `protocol.json` 中 `modbus.dePin` 配置，**不得与 `peripherals.json` 中任一启用外设的引脚重复**。重复会导致通信方向切换异常或按键失效。

### 12.5 DAC 写入失败

仅 ESP32 classic 的 GPIO25 / GPIO26 支持硬件 DAC。ESP32-S3/C3 不支持，启用会返回 `DAC not supported on this chip`。

### 12.6 OLED 不亮

排查顺序：
1. I²C 地址：大多数 SSD1306 为 `0x3C`，少数 `0x3D`（需手动在 `LCDManager` 中调整）。
2. 引脚：SDA/SCL 顺序不可颠倒。
3. 电源：0.96" OLED 典型工作电压 3.3V，电流 20mA 内。
4. `params.interface` 必须为 `2`（I²C）。

### 12.7 LCD vs LED 容易混淆

| | LCD/OLED (type=36) | LED 灯带 (type=45, NeoPixel) |
| --- | --- | --- |
| 用途 | 字符/图形显示 | 多彩像素灯 |
| 接口 | I²C / SPI | RMT（单信号线 WS2812B） |
| 驱动库 | U8g2 | Adafruit NeoPixel |
| 默认启用 | ✅ esp32 启用 | ❌ 默认禁用 |

如果你要驱动 "一只 LED 单灯"，应使用 `GPIO_DIGITAL_OUTPUT` (type=12) 或 `GPIO_PWM_OUTPUT` (type=17)；如果要 "WS2812 灯带"，使用 `NEO_PIXEL` (type=45)；显示屏才是 `LCD` (type=36)。

---

## 13. 最佳实践

### 13.1 命名

- `id` 用 snake_case 英文（如 `oled_display`、`key1`、`tm1637_01`），便于规则引擎脚本引用。
- `name` 用中文/英文均可，面向用户展示。

### 13.2 引脚分配策略

1. **优先使用安全引脚**：GPIO4/5/16/17/18/19/21/22/23/25/26/27/32/33。
2. **保留**：GPIO0（Boot）、GPIO1/3（UART0 调试）、GPIO6-11（Flash）。
3. **输入专用**：GPIO34~39，只能做输入。
4. **为按键预留独立 GPIO**，不与 TM1637/OLED/Modbus DE 共用。

### 13.3 配置演进

- 修改外设类型或引脚时，先在 Web UI 禁用 → 保存 → 重新启用，避免热切换时残留中断。
- 规则配置 (`periph_exec.json`) 引用的外设 ID 变更时，需同步更新 `targets[]`。

### 13.4 调试手段

- 串口日志：`Peripheral Manager: ...` 前缀可过滤。
- Web → 外设管理：一览所有外设、实时状态、引脚占用。
- `pio device monitor -p COM6 -b 115200`。

### 13.5 扩展新外设

1. 在 [`PeripheralTypes.h`](../include/core/PeripheralTypes.h) 添加枚举值（遵循区段 ID 规则）。
2. 更新 `getPeripheralTypeName` / `parsePeripheralType` / `getPeripheralPinCount`。
3. 在 `PeripheralManager::setupHardware` 添加初始化分支。
4. 在 [`web-src/pages/modals.html`](../web-src/pages/modals.html) 添加 `<option>` 及 `data-i18n`。
5. 在 `web-src/i18n/i18n-zh-CN.js` / `i18n-en.js` 添加翻译键。
6. （可选）在 [`web-src/modules/runtime/periph-exec-form.js`](../web-src/modules/runtime/periph-exec-form.js) 添加规则动作支持。

> **提示**：若要扩展的是**传感器类**外设（需周期性读取温度/湿度/光照等数值），推荐优先使用第 14 章的 `ISensorDriver` 驱动接口，无需侵入 `PeripheralManager` 核心代码。

---

## 14. 传感器驱动扩展（ISensorDriver）

> 本章节描述本次优化新增的**传感器驱动抽象接口与热插拔注册机制**，目标是把传感器硬件读取逻辑从 `PeripheralManager`/`PeriphExec` 的硬编码分支中解耦，便于后续新增 SHT31、BMP280、SCD41 等器件时不再修改核心调度层。

### 14.1 设计目标

- 将"硬件初始化 + 周期读取 + 多通道输出"封装到独立驱动类。
- 使用**静态注册宏**在 `main()` 之前完成自注册，新增驱动只需"新建一个头文件 + 包含一次"。
- 驱动注册表使用**固定容量静态数组**（`MAX_SENSOR_DRIVERS = 8`），零动态分配、零碎片。
- 支持最多 4 通道读取（如 DHT 的温度 + 湿度），通道具名（name/unit）方便前端渲染。

### 14.2 核心接口

头文件：[`include/core/interfaces/ISensorDriver.h`](../include/core/interfaces/ISensorDriver.h)

```cpp
struct SensorReading {
    bool success = false;
    float values[4] = {0};      // 最多 4 通道
    uint8_t channelCount = 0;
    unsigned long timestamp = 0;
    // 语义化访问
    float temperature() const { return channelCount > 0 ? values[0] : NAN; }
    float humidity()    const { return channelCount > 1 ? values[1] : NAN; }
};

class ISensorDriver {
public:
    virtual ~ISensorDriver() = default;
    virtual const char* getName() const = 0;                         // 驱动类型名，如 "sht31"
    virtual uint8_t getChannelCount() const = 0;                      // 通道数 1~4
    virtual const char* getChannelName(uint8_t ch) const = 0;         // "temperature" / "humidity" 等
    virtual const char* getChannelUnit(uint8_t ch) const = 0;         // "℃" / "%" / "lux" 等
    virtual bool init(uint8_t pin, const char* params = nullptr) = 0; // params 为 JSON 字符串，可选
    virtual bool read(SensorReading& reading) = 0;
    virtual void deinit() = 0;
    virtual unsigned long getMinInterval() const { return 1000; }     // 最小采样间隔 (ms)
};
```

### 14.3 注册机制

头文件：[`include/core/DriverRegistry.h`](../include/core/DriverRegistry.h)

| 要素 | 说明 |
|------|------|
| `DriverRegistry::getInstance()` | 单例，全局唯一 |
| `registerDriver(name, factory)` | 由静态构造函数自动调用，通常不需要手动调用 |
| `createDriver(name)` | 按名称创建驱动实例，返回 `ISensorDriver*`，失败返回 `nullptr` |
| `hasDriver(name)` | 判断驱动是否已注册 |
| `FASTBEE_REGISTER_SENSOR(name, Class)` | 宏，在文件末尾使用一次即可完成自注册 |

注册表内部是 `std::array<SensorDriverEntry, 8>`，满载时 `registerDriver` 返回 `false`（构建期可见），避免运行时失败。

### 14.4 编写自定义驱动

以 [`include/peripherals/drivers/SHT31Driver.h`](../include/peripherals/drivers/SHT31Driver.h) 为模板：

```cpp
#include "core/interfaces/ISensorDriver.h"
#include "core/DriverRegistry.h"

class MySensorDriver : public ISensorDriver {
public:
    const char* getName() const override { return "my_sensor"; }
    uint8_t getChannelCount() const override { return 2; }
    const char* getChannelName(uint8_t ch) const override {
        return ch == 0 ? "temperature" : "humidity";
    }
    const char* getChannelUnit(uint8_t ch) const override {
        return ch == 0 ? "℃" : "%";
    }
    bool init(uint8_t pin, const char* params) override {
        // TODO: 初始化 I2C / OneWire / 引脚
        return true;
    }
    bool read(SensorReading& r) override {
        r.values[0] = 25.3f;
        r.values[1] = 60.0f;
        r.channelCount = 2;
        r.success = true;
        r.timestamp = millis();
        return true;
    }
    void deinit() override {}
    unsigned long getMinInterval() const override { return 2000; }
};

// 自动注册：在任意 .cpp 包含此头文件一次即可
FASTBEE_REGISTER_SENSOR("my_sensor", MySensorDriver);
```

**关键点**：

1. 驱动头文件放在 `include/peripherals/drivers/` 目录。
2. 需要被 `src/main.cpp`（或任一被链接的 `.cpp`）显式 `#include` 一次，触发静态对象构造 → 完成注册。
3. 使用方：`ISensorDriver* drv = DriverRegistry::getInstance().createDriver("my_sensor");`

### 14.5 与现有传感器逻辑的关系

- **当前状态**：`PeriphExec` 中对 `DHT11/DHT22/DS18B20` 等的硬编码读取分支仍在使用，未迁移到 `ISensorDriver`，功能与兼容性不受影响。
- **建议策略**：
  - 新增器件（SHT31/BMP280/SCD41/BH1750 等）**优先使用** `ISensorDriver`。
  - 存量器件待完整回归后按需迁移，不强制。
- **容量提升**：若 8 种驱动不够，修改 `DriverRegistry.h` 中的 `MAX_SENSOR_DRIVERS` 常量并重新编译。

---

## 参考文档

- [`oled_usage_guide.md`](./oled_usage_guide.md) — OLED 自定义显示规则
- [`modbus_usage_guide.md`](./modbus_usage_guide.md) — Modbus 使用指南
- [`periph_exec_flow.md`](./periph_exec_flow.md) — 外设执行规则流程
- [`script-guide.md`](./script-guide.md) — 规则脚本手册
