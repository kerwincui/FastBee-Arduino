# 外设执行（PeriphExec）配置使用指南

> 本文档面向**设备配置用户 / 集成人员**，介绍外设执行规则（PeriphExec Rule）的配置方法、触发类型与动作类型的使用差异、字段含义与典型场景。
>
> 如果你关心**底层实现架构、任务调度、源码逻辑**，请阅读 [`periph_exec_flow.md`](./periph_exec_flow.md)。
>
> 如果你关心**外设本身**（类型、引脚、编译开关），请阅读 [`peripheral-configuration-guide.md`](../peripherals/peripheral-configuration-guide.md)。

---

## 当前版本提示

- 精简版默认保留外设执行能力和轻量命令脚本，关闭完整 RuleScript 引擎。
- 事件触发已支持本地传感器数据源，事件 ID 使用 `ds:<sourceId>` 形式，例如 `ds:dht_01_temperature`。
- “本地传感器”下拉只显示已经在外设执行规则中通过传感器采集动作配置过的数据源；未配置采集动作时不会显示无效传感器来源。
- 默认配置不再内置专用蜂鸣器外设或蜂鸣器预设动作；`actionType=20` 作为历史保留位。
- 显示屏动作在 Web 界面中按“显示屏”类别合并展示，保留“显示数字、显示文本、数码管清屏、OLED 自定义显示”四类动作，便于用户选择。
- 配置备份和迁移建议使用“设备配置 > 高级配置 > 配置导入/导出”，可按“外设执行”单独导入导出 `periph_exec.json`。

外设执行页面用于查看规则列表、启用状态、触发器和动作入口。新建规则时建议先保持禁用，保存后通过“执行一次”和日志确认动作安全。

![外设执行规则列表](../system/images/periph-exec-management.png)

![外设执行规则链路图](../images/periph-exec-rule-flow.svg)

配置时建议按链路图从左到右填写：先确认触发器来源，再选择动作目标，最后决定是否上报执行结果；复杂规则应先禁用保存，手动执行确认后再启用。

![外设执行规则生命周期](../images/periph-exec-rule-lifecycle.svg)

规则上线前请按生命周期图执行：先禁用保存，再检查触发器和动作参数，手动执行确认安全后才启用自动触发；异常时优先禁用规则并恢复备份。

![触发器与动作选择地图](../images/trigger-action-selection-map.svg)

配置规则时先回答“什么时候执行”，再回答“执行什么动作”。触发器保持单一明确，动作先从可观察、可回滚的单动作开始。

![触发器时序对比](../images/trigger-timing-comparison.svg)

触发器选择错误会让后续排查绕远路。平台触发排查平台消息和协议在线，定时触发排查时钟和间隔，事件触发排查事件 ID 和防抖，轮询触发排查采样周期、超时、重试和资源占用。

![外设执行规则表单安全检查](../images/rule-builder-safety-checklist.svg)

在启用规则前，按图检查基础信息、触发器、动作、结果上报和回滚手段。现场有真实负载时，强动作规则必须先禁用保存并手动验证。

## 目录

- [1. 什么是外设执行规则](#1-什么是外设执行规则)
- [2. 规则结构总览](#2-规则结构总览)
- [3. 触发器（Trigger）详解](#3-触发器trigger详解)
  - [3.1 平台触发 (PLATFORM_TRIGGER=0)](#31-平台触发-platform_trigger0)
  - [3.2 定时触发 (TIMER_TRIGGER=1)](#32-定时触发-timer_trigger1)
  - [3.3 事件触发 (EVENT_TRIGGER=4)](#33-事件触发-event_trigger4)
  - [3.4 轮询触发 (POLL_TRIGGER=5)](#34-轮询触发-poll_trigger5)
  - [3.5 触发类型对比表](#35-触发类型对比表)
- [4. 动作（Action）详解](#4-动作action详解)
  - [4.1 GPIO 输出类动作（0/1/13/14）](#41-gpio-输出类动作011314)
  - [4.2 PWM / 模拟输出类动作（2/3/4/5）](#42-pwm--模拟输出类动作2345)
  - [4.3 系统管理类动作（6/7/8/9）](#43-系统管理类动作6789)
  - [4.4 调用 / 脚本类动作（10/15）](#44-调用--脚本类动作1015)
  - [4.5 Modbus 类动作（16/17/18）](#45-modbus-类动作161718)
  - [4.6 数据 / 事件类动作（19/21）](#46-数据--事件类动作1921)
  - [4.7 规则控制类动作（22/23）](#47-规则控制类动作2223)
  - [4.8 显示屏类动作（24/25/26/27）](#48-显示屏类动作24252627)
  - [4.9 动作类型完整速查表](#49-动作类型完整速查表)
- [5. 模板与动态值](#5-模板与动态值)
- [6. 显示屏动作是否应合并？评估说明](#6-显示屏动作是否应合并评估说明)
- [7. 典型配置示例](#7-典型配置示例)
- [8. 最佳实践](#8-最佳实践)
- [9. 常见问题 FAQ](#9-常见问题-faq)

---

## 1. 什么是外设执行规则

外设执行规则（PeriphExec Rule）是 FastBee 设备的**本地自动化规则引擎**，实现"**当某个条件满足时，执行一组动作**"的逻辑闭环，无需依赖云端联动，即使断网也能运行。

一条规则由三部分组成：

```
┌─────────────┐     ┌────────────────┐     ┌─────────────┐
│ 触发器(多个) │ ──> │ 条件 / 接收值   │ ──> │ 动作(多个)   │
│  OR 关系    │     │  模板解析、转换 │     │  顺序执行    │
└─────────────┘     └────────────────┘     └─────────────┘
```

- **触发器（triggers）**：至多 3 个，任一命中即触发（OR 关系）。
- **动作（actions）**：至多 4 个，按数组顺序依次执行（可设每动作延时 `syncDelayMs`）。
- **执行模式（execMode）**：`0=异步`（默认，FreeRTOS 任务，不阻塞主循环）/ `1=同步`（阻塞执行，适合快速 IO）。

配置文件路径：`/config/periph_exec.json`，运行时由 `PeriphExecManager` 加载。

---

## 2. 规则结构总览

单条规则 JSON 结构（`data/config/periph_exec.json` 数组中的一项）：

![外设执行规则数据模型](../images/periph-exec-rule-data-model.svg)

阅读 JSON 时可以对照数据模型：顶层字段描述规则身份和执行策略，`triggers` 描述来源和条件，`actions` 描述目标外设和动作参数，`reportAfterExec` 决定执行后是否形成可观测反馈。

```json
{
  "id": "exec_1700000000001",
  "name": "温度过高告警",
  "enabled": true,
  "execMode": 0,

  "triggers": [
    {
      "triggerType": 5,
      "triggerPeriphId": "dht_01",
      "operatorType": 2,
      "compareValue": "30"
    }
  ],

  "actions": [
    { "targetPeriphId": "fan_01", "actionType": 0, "execMode": 1 },
    { "targetPeriphId": "lcd_01",    "actionType": 27, "actionValue": "# 高温报警\n温度: ${dht_01.temperature}℃", "syncDelayMs": 200 }
  ],

  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

### 2.1 顶层字段

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `id` | string | — | 唯一 ID（建议 `exec_` 前缀 + 时间戳） |
| `name` | string | — | 显示名称（用于 UI / 日志） |
| `enabled` | bool | `true` | 规则启用开关 |
| `execMode` | 0\|1 | `0` | 规则级执行模式，0=异步，1=同步；动作项若设了 `execMode` 则覆盖 |
| `triggers` | array | — | 触发器列表，**最多 3 个**，OR 关系 |
| `actions` | array | — | 动作列表，**最多 4 个**，顺序执行 |
| `protocolType` | uint8 | `0` | 数据上报协议偏好：0=MQTT, 1=ModbusRTU, 2=ModbusTCP, 3=HTTP, 4=CoAP, 5=TCP |
| `scriptContent` | string | `""` | 可选文本模板，用于执行后上报的数据脱壳（`${key}` 占位符） |
| `reportAfterExec` | bool | `true` | 执行完成后是否自动上报控制结果到已启用协议 |

> **配额限制**：单规则 `triggers` 不超过 3 条（`MAX_TRIGGERS_PER_RULE`），`actions` 不超过 4 条（`MAX_ACTIONS_PER_RULE`）。超出会在保存时被拒绝。

---

## 3. 触发器（Trigger）详解

`triggerType` 取值：`0=平台触发`、`1=定时触发`、`4=事件触发`、`5=轮询触发`。

> **注意**：历史值 `2=DATA_RECEIVE`、`3=DATA_REPORT` 已**弃用**，统一迁移到 `4=事件触发`（用 eventId 区分）。

### 3.1 平台触发 (PLATFORM_TRIGGER=0)

**用途**：响应 IoT 平台 MQTT 下发的**属性设置 / 功能调用**指令。

**使用字段**：
| 字段 | 含义 |
|---|---|
| `triggerPeriphId` | 必填。与 MQTT 消息 `item.id` 精确匹配（一般是外设 ID） |
| `operatorType` | 比较运算符，**语义特殊**见下 |
| `compareValue` | 比较值；当 `operatorType=1` 时表示"设置模式"不参与比较，任意值均匹配 |

**`operatorType` 特殊约定（仅平台触发）**：
- `0` = 精确匹配：必须 `item.value == compareValue` 才触发（例如只响应 `value="ON"` 的指令）
- `1` = 设置模式：不比较值，只要 `item.id == triggerPeriphId` 就触发，同时把 `item.value` 作为 `receivedValue` 注入后续动作

> 其他 `operatorType`（2=GT/3=LT/4=GTE/5=LTE/6=BETWEEN/7=NOT_BETWEEN/8=CONTAIN/9=NOT_CONTAIN）在**轮询触发**下才常用；平台触发一般只用 0/1。

**典型配置**：
```json
{ "triggerType": 0, "triggerPeriphId": "relay_01", "operatorType": 1, "compareValue": "" }
```
含义：收到平台下发 `{"id":"relay_01","value":"ON"}` 或 `{"id":"relay_01","value":"OFF"}` 都会触发，动作可用 `$value` 占位符读取下发值。

### 3.2 定时触发 (TIMER_TRIGGER=1)

**用途**：按间隔或每日时间点周期性执行。

**使用字段**：
| 字段 | 含义 |
|---|---|
| `timerMode` | `0=间隔`（每 N 秒） / `1=每日时间点`（HH:MM） |
| `intervalSec` | 间隔秒数（`timerMode=0` 时生效，最小 1s，建议 ≥5s 避免频繁唤醒） |
| `timePoint` | `"HH:MM"`（`timerMode=1` 时生效，24h 制；需 NTP 同步时间） |

**典型配置**：
```json
// 每 15 秒刷一次 OLED
{ "triggerType": 1, "timerMode": 0, "intervalSec": 15 }

// 每天早上 6:30 触发浇水
{ "triggerType": 1, "timerMode": 1, "timePoint": "06:30" }
```

> **每日时间点触发**依赖系统时间，强烈建议启用 NTP。时间未同步前（`getLocalTime()` 失败）不会触发。

### 3.3 事件触发 (EVENT_TRIGGER=4)

**用途**：响应系统事件（WiFi/MQTT/NTP/按键/OTA/外设执行完成/协议数据收发等）。

**使用字段**：
| 字段 | 含义 |
|---|---|
| `eventId` | 必填。事件 ID 字符串（见下表） |

**常用事件 ID 速查**（完整列表见 `include/core/PeripheralExecution.h` 中 `STATIC_EVENTS`）：

| 分类 | eventId | 含义 |
|---|---|---|
| WiFi | `wifi_connected` / `wifi_disconnected` / `wifi_conn_failed` | WiFi 连接状态 |
| MQTT | `mqtt_connected` / `mqtt_disconnected` / `mqtt_enabled` | MQTT 协议状态 |
| 网络模式 | `net_mode_ap` / `net_mode_sta` | AP/STA 切换 |
| 系统 | `ntp_synced` / `ota_start` / `ota_success` / `ota_failed` | 系统服务 |
| 系统状态 | `system_boot` / `system_ready` / `system_error` / `factory_reset` | 生命周期 |
| **按键** | `button_click` / `button_double_click` / `button_long_press_2s` / `button_long_press_5s` / `button_long_press_10s` / `button_press` / `button_release` | 来自数字输入上拉/下拉引脚的按键检测（消抖 50ms，双击窗口 200ms） |
| 规则 | `periph_exec_completed` | 任意外设执行规则完成时触发 |
| 数据 | `data_receive` / `data_report` | 协议数据收发（替代已弃用的 2/3） |
| 设备事件 | `break_down` / `restart` / `device_alarm` / `low_power` | 可由规则主动发射，用于 MQTT 上报 |
| **自定义事件** | 来自 `DEVICE_EVENT` 外设（type=60）的 ID | 用户自定义事件，可由 `ACTION_TRIGGER_EVENT` 发射 |
| **Modbus 控制事件** | `mc:<slaveId>:<regAddr>` | Modbus 子设备寄存器写入时的控制事件，可用 `compareValue` 匹配具体写入值 |

**典型配置**：
```json
// WiFi 连接成功后点亮 LED
{ "triggerType": 4, "eventId": "wifi_connected" }

// 按键双击切换继电器
{ "triggerType": 4, "eventId": "button_double_click", "triggerPeriphId": "btn_01" }
```

> **按键事件**额外支持 `triggerPeriphId` 过滤：只有来自该外设 ID 的按键事件才会命中。不填则匹配所有按键。

### 3.4 轮询触发 (POLL_TRIGGER=5)

**用途**：本地周期性读取传感器 / Modbus 子设备数据，当**读数满足条件**时触发。

**使用字段**：
| 字段 | 含义 |
|---|---|
| `triggerPeriphId` | 必填。数据源外设 ID（可以是 DHT/DS18B20/Modbus 子设备/ADC 等） |
| `operatorType` | 条件运算符（0=EQ / 1=NEQ / 2=GT / 3=LT / 4=GTE / 5=LTE / 6=BETWEEN / 7=NOT_BETWEEN / 8=CONTAIN / 9=NOT_CONTAIN） |
| `compareValue` | 比较阈值；`BETWEEN/NOT_BETWEEN` 用 `"min,max"` 格式（如 `"20,30"`） |
| `intervalSec` | 轮询周期秒数（建议 ≥5s；<5s 会被安全下限修正） |
| `pollResponseTimeout` | Modbus 响应超时（ms），默认 1000 |
| `pollMaxRetries` | Modbus 最大重试，默认 2 |
| `pollInterPollDelay` | 同总线多子设备间最小间隔（ms），默认 100 |

**典型配置**：
```json
// 温度 > 30°C 时触发（DHT11 每 30 秒轮询）
{
  "triggerType": 5,
  "triggerPeriphId": "dht_01",
  "operatorType": 2,
  "compareValue": "30",
  "intervalSec": 30
}

// Modbus 子设备寄存器值在 [1000, 2000] 之外时告警
{
  "triggerType": 5,
  "triggerPeriphId": "mb_slave_01",
  "operatorType": 7,
  "compareValue": "1000,2000",
  "intervalSec": 10,
  "pollResponseTimeout": 1500,
  "pollMaxRetries": 3
}
```

> **性能提示**：轮询触发会绑定硬件任务。活跃轮询任务数过多（>8）或间隔过小（<5s）会触发安全修正日志。Modbus 轮询还要注意**串口总线竞争**，多个子设备间建议 `pollInterPollDelay ≥ 100ms`。

### 3.5 触发类型对比表

| 维度 | 平台触发 (0) | 定时触发 (1) | 事件触发 (4) | 轮询触发 (5) |
|---|---|---|---|---|
| **触发来源** | IoT 平台 MQTT 下发 | 系统时钟 | 系统/按键/协议事件 | 本地传感器周期读取 |
| **能否离线工作** | ❌ 需 MQTT 在线 | ✅ 完全离线 | ✅ 本地事件均支持 | ✅ 完全离线 |
| **`receivedValue` 来源** | 平台下发值 | 无 | 事件附带数据（可空） | 本地读数 |
| **关键字段** | `triggerPeriphId` + `operatorType`+`compareValue` | `timerMode` + `intervalSec` / `timePoint` | `eventId`（可选 `triggerPeriphId`） | `triggerPeriphId` + `operatorType` + `compareValue` + `intervalSec` |
| **典型场景** | 远程控制、远程设置参数 | 定时浇水、定时刷新显示 | WiFi 断连告警、按键切换、OTA 完成提示 | 温控、超限告警、本地联动 |
| **建议周期下限** | — | ≥1s（推荐 ≥5s） | 事件驱动，无周期 | ≥5s（低于会被修正） |

---

## 4. 动作（Action）详解

所有动作均写入同一数组，按索引顺序执行。每个动作可带 `syncDelayMs`（执行前延时，最大 10000ms）用于编排时序。

### 4.1 GPIO 输出类动作（0/1/13/14）

| actionType | 名称 | actionValue | 适用目标 | 说明 |
|---|---|---|---|---|
| `0` | ACTION_HIGH | — | `GPIO_DIGITAL_OUTPUT` | 置高电平 |
| `1` | ACTION_LOW | — | `GPIO_DIGITAL_OUTPUT` | 置低电平 |
| `13` | ACTION_HIGH_INVERTED | — | `GPIO_DIGITAL_OUTPUT` | "语义高"但物理输出低（用于低电平有效的继电器） |
| `14` | ACTION_LOW_INVERTED | — | `GPIO_DIGITAL_OUTPUT` | "语义低"但物理输出高 |

> **反转动作的用途**：有些继电器模块是"低电平驱动"，直接用 `ACTION_HIGH` 反而关闭，用 `ACTION_HIGH_INVERTED` 可保持"高=开启"的语义直觉。

### 4.2 PWM / 模拟输出类动作（2/3/4/5）

| actionType | 名称 | actionValue 格式 | 适用目标 |
|---|---|---|---|
| `2` | ACTION_BLINK | 闪烁周期 ms（如 `"500"`） | PWM/GPIO 输出 |
| `3` | ACTION_BREATHE | 呼吸周期 ms（如 `"2000"`） | PWM 输出 |
| `4` | ACTION_SET_PWM | 占空比 0-255 或百分比 0-100 | `GPIO_PWM_OUTPUT` |
| `5` | ACTION_SET_DAC | DAC 值 0-255 | `DAC`（仅 ESP32 DAC 支持引脚） |

> 启用 `useReceivedValue=true` 时，`actionValue` 被**触发源的接收值**覆盖，适合平台下发亮度/占空比等。

### 4.3 系统管理类动作（6/7/8/9）

| actionType | 名称 | targetPeriphId | actionValue |
|---|---|---|---|
| `6` | ACTION_SYS_RESTART | 可留空 | — |
| `7` | ACTION_SYS_FACTORY_RESET | 可留空 | — |
| `8` | ACTION_SYS_NTP_SYNC | 可留空 | 可选 NTP 服务器 |
| `9` | ACTION_SYS_OTA | 可留空 | 固件 URL |

> 系统动作**不要求 `targetPeriphId`**，因为它作用于整机。放在动作列表末尾（避免后续动作因重启失败）。

### 4.4 调用 / 脚本类动作（10/15）

| actionType | 名称 | 说明 |
|---|---|---|
| `10` | ACTION_CALL_PERIPHERAL | 将 `actionValue` 作为命令转发给 `targetPeriphId` 外设；步进电机支持 JSON 命令 |
| `15` | ACTION_SCRIPT | 执行命令序列脚本，`actionValue` 为脚本内容。详见 [`script-guide.md`](./script-guide.md) |

步进电机（`STEPPER_MOTOR`，type=42）建议使用 JSON 格式，避免命令参数歧义：

| actionValue 示例 | 说明 |
|---|---|
| `{"periphId":"stepper","action":"forward"}` | 正转 |
| `{"periphId":"stepper","action":"reverse"}` | 反转 |
| `{"periphId":"stepper","action":"stop"}` | 停止并释放线圈 |
| `{"periphId":"stepper","action":"faster","value":"2"}` | 加速 2 RPM |
| `{"periphId":"stepper","action":"slower","value":"2"}` | 减速 2 RPM |
| `{"periphId":"stepper","action":"setSpeed","value":"12"}` | 设置转速为 12 RPM |

### 4.5 Modbus 类动作（16/17/18）

| actionType | 名称 | actionValue | 说明 |
|---|---|---|---|
| `16` | ACTION_MODBUS_COIL_WRITE | `"coilAddr:0/1"` 或单独 `"0/1"` | Modbus FC05 写线圈 |
| `17` | ACTION_MODBUS_REG_WRITE | `"regAddr:value"` 或单独 `"value"` | Modbus FC06 写寄存器 |
| `18` | ACTION_MODBUS_POLL | 无 | 主动触发一次子设备数据采集（由 PeriphExec 调度） |

> `targetPeriphId` 必须是已配置的 Modbus 子设备（`MODBUS_DEVICE`，type=51）。

### 4.6 数据 / 事件类动作（19/21）

| actionType | 名称 | actionValue | 适用目标 |
|---|---|---|---|
| `19` | ACTION_SENSOR_READ | — | 传感器（DHT/DS18B20/ADC/脉冲），用于主动触发一次采集并缓存 |
| `21` | ACTION_TRIGGER_EVENT | 事件额外数据（可空） | `targetPeriphId` = 事件 ID（系统内置或 `DEVICE_EVENT` 外设 ID） |

> `ACTION_TRIGGER_EVENT` 用于**规则间联动**：一条规则可以作为事件源唤醒其他以该事件为触发的规则。

### 4.7 规则控制类动作（22/23）

| actionType | 名称 | targetPeriphId | 说明 |
|---|---|---|---|
| `22` | ACTION_ENABLE_EXEC_RULE | 另一条规则的 `id` | 启用该规则 |
| `23` | ACTION_DISABLE_EXEC_RULE | 另一条规则的 `id` | 禁用该规则 |

> 用于**模式切换**：例如按键长按 5s 切换"白天模式"/"夜晚模式"（启用一组规则同时禁用另一组）。

### 4.8 显示屏类动作（24/25/26/27）

| actionType | 名称 | actionValue | 目标外设类型 | 编译开关 |
|---|---|---|---|---|
| `24` | ACTION_DISPLAY_NUMBER | 数字：`"12.34"` / `"12:34"` / `"1234"` / `"-12"`，支持 `${id.field}` | `SEVEN_SEGMENT_TM1637` (47) | `FASTBEE_ENABLE_SEVEN_SEGMENT` |
| `25` | ACTION_DISPLAY_TEXT | 文本（最多 4 字符、受限字码表），支持 `${id.field}` | `SEVEN_SEGMENT_TM1637` (47) | `FASTBEE_ENABLE_SEVEN_SEGMENT` |
| `26` | ACTION_DISPLAY_CLEAR | — | `SEVEN_SEGMENT_TM1637` (47) | `FASTBEE_ENABLE_SEVEN_SEGMENT` |
| `27` | ACTION_OLED_DISPLAY | 多行文本，支持 `${id.field}` + `$value`；首行 `#` 开头为居中标题+分隔线；`\n` 分行；最大 512 字符 | `LCD` (36) | `FASTBEE_ENABLE_LCD` |

> **注意**：`targetPeriphId` 必须是对应硬件类型；错配（如把 24 指给 LCD 外设）会被运行时拒绝并记录 `warning` 日志。详见 [第 6 章合并评估](#6-显示屏动作是否应合并评估说明)。

### 4.9 动作类型完整速查表

| ID | 名称 | 分类 | 关键参数 |
|---:|---|---|---|
| 0 | ACTION_HIGH | GPIO | — |
| 1 | ACTION_LOW | GPIO | — |
| 2 | ACTION_BLINK | PWM | 周期 ms |
| 3 | ACTION_BREATHE | PWM | 周期 ms |
| 4 | ACTION_SET_PWM | PWM | 0-255 / 0-100% |
| 5 | ACTION_SET_DAC | DAC | 0-255 |
| 6 | ACTION_SYS_RESTART | 系统 | — |
| 7 | ACTION_SYS_FACTORY_RESET | 系统 | — |
| 8 | ACTION_SYS_NTP_SYNC | 系统 | NTP 服务器 |
| 9 | ACTION_SYS_OTA | 系统 | 固件 URL |
| 10 | ACTION_CALL_PERIPHERAL | 通信 | 命令字符串 |
| 13 | ACTION_HIGH_INVERTED | GPIO | — |
| 14 | ACTION_LOW_INVERTED | GPIO | — |
| 15 | ACTION_SCRIPT | 脚本 | 脚本文本 |
| 16 | ACTION_MODBUS_COIL_WRITE | Modbus | `addr:val` |
| 17 | ACTION_MODBUS_REG_WRITE | Modbus | `addr:val` |
| 18 | ACTION_MODBUS_POLL | Modbus | — |
| 19 | ACTION_SENSOR_READ | 数据 | — |
| 20 | 保留位 | — | 旧版蜂鸣器预设动作已移除 |
| 21 | ACTION_TRIGGER_EVENT | 事件 | 事件数据 |
| 22 | ACTION_ENABLE_EXEC_RULE | 规则控制 | 规则 ID |
| 23 | ACTION_DISABLE_EXEC_RULE | 规则控制 | 规则 ID |
| 24 | ACTION_DISPLAY_NUMBER | 显示屏 | 数字串 |
| 25 | ACTION_DISPLAY_TEXT | 显示屏 | ≤4 字符 |
| 26 | ACTION_DISPLAY_CLEAR | 显示屏 | — |
| 27 | ACTION_OLED_DISPLAY | 显示屏 | 多行+模板 |

---

## 5. 模板与动态值

动作的 `actionValue` 支持三种动态取值机制：

### 5.1 `${periphId.field}` 传感器模板

从本地传感器数据缓存中读取最新值并替换。

**支持的 field**（按传感器类型）：
- DHT11 / DHT22：`temperature` / `humidity`
- DS18B20：`temperature`
- 模拟输入（ADC / GPIO_ANALOG_INPUT）：`value` / `voltage`
- 数字输入：`value`（0/1）
- Modbus 子设备：`reg_<N>` / `coil_<N>`

**示例**：
```text
"# 环境监控\n温度: ${dht_01.temperature}°C\n湿度: ${dht_01.humidity}%"
```

### 5.2 `$value` 接收值占位符（仅 OLED 动作 27）

替换为**触发源带入的原始值**：
- 平台触发：MQTT 下发的 `item.value`
- 轮询触发：外设当前读数
- 事件触发：事件附带数据

**示例**：
```text
"# 平台下发\n值: $value"
```
当 MQTT 下发 `{"id":"lcd_01","value":"Hello"}` 时，显示 `值: Hello`。

### 5.3 `useReceivedValue=true`（通用）

在 `ExecAction` 上设置此标志后，整个 `actionValue` 被"接收值"完全替换（适合 PWM/DAC/继电器直接使用平台下发数字）。

---

## 6. 显示屏动作是否应合并？评估说明

社区曾提议"将显示数字(24)/显示文本(25)/数码管清屏(26)/OLED 自定义显示(27) 合并为单个通用动作以简化配置"。经评估，**当前保持 4 个独立动作**，理由：

### 6.1 硬件本质差异

| 维度 | TM1637（24/25/26） | OLED/LCD（27） |
|---|---|---|
| 驱动层 | `SevenSegmentDriver` (bit-bang) | `LCDManager` (U8g2 图形库) |
| 显示能力 | 4 位数码管（有限字码表） | 128×64 图形点阵，多行文本 |
| 参数格式 | 简单字符串 | 多行+模板+`#` 标题+`\n` |
| 编译开关 | `FASTBEE_ENABLE_SEVEN_SEGMENT` | `FASTBEE_ENABLE_LCD` |
| UI 控件 | `<input>` | `<textarea rows=6 maxlength=512>` |

### 6.2 合并会带来的问题

- **参数校验复杂化**：合并后执行器必须靠 `target` 的外设类型隐式判断字段语义，错误提示难定位。
- **破坏兼容性**：已有配置中 `actionType=24/25/26/27` 的规则需要迁移脚本。
- **前端表单反而更复杂**：一个"万能显示"动作需要大量的条件渲染（target 类型→参数控件），比现在的 4 个独立动作更难维护。

### 6.3 当前已采用的折中方案

**"前端分类合并 + 后端保持独立"**：

- 前端 UI（`web-src/modules/runtime/periph-exec-form.js`）已将四个动作归入 **"显示屏"** 分组（`periph-exec-action-cat-display`），用户在下拉菜单中看到同一分类。
- 根据 `actionType` 自动切换参数控件：24/25 用 `input`，26 无参数，27 用 `textarea`。
- 后端枚举保持独立，日志清晰，运行时类型校验严格（外设类型错配直接拒绝）。

### 6.4 可选的进一步简化（未来优化）

若仍希望降低用户选择成本，推荐在**前端**按如下方式增强（不改后端）：

> 用户先选 `targetPeriphId` → 前端根据目标外设类型**只显示**对应的动作：
> - 选了 `SEVEN_SEGMENT_TM1637` → 只显示 24/25/26
> - 选了 `LCD` → 只显示 27

此方案在保持后端不变的前提下，用户视角已感觉"只有一种显示动作"，是推荐的渐进式优化路线。

---

## 7. 典型配置示例

### 7.1 定时刷新 OLED 环境监控

```json
{
  "id": "exec_oled_env",
  "name": "OLED 环境监控（15s）",
  "enabled": true,
  "triggers": [
    { "triggerType": 1, "timerMode": 0, "intervalSec": 15 }
  ],
  "actions": [
    {
      "targetPeriphId": "lcd_01",
      "actionType": 27,
      "actionValue": "# 环境监控\n温度: ${dht_01.temperature}°C\n湿度: ${dht_01.humidity}%\nIP: ${sys.ip}"
    }
  ]
}
```

### 7.2 按键双击切换继电器

```json
{
  "id": "exec_btn_relay_toggle",
  "name": "按键双击切换继电器",
  "enabled": true,
  "triggers": [
    { "triggerType": 4, "eventId": "button_double_click", "triggerPeriphId": "btn_01" }
  ],
  "actions": [
    { "targetPeriphId": "relay_01", "actionType": 15, "actionValue": "toggle relay_01" }
  ]
}
```

### 7.3 温度过高告警（风扇+OLED+MQTT 事件上报）

```json
{
  "id": "exec_high_temp_alarm",
  "name": "高温告警",
  "enabled": true,
  "triggers": [
    { "triggerType": 5, "triggerPeriphId": "dht_01", "operatorType": 2, "compareValue": "32", "intervalSec": 10 }
  ],
  "actions": [
    { "targetPeriphId": "fan_01", "actionType": 0, "execMode": 1 },
    { "targetPeriphId": "lcd_01", "actionType": 27, "actionValue": "# 高温告警\n温度: ${dht_01.temperature}°C\n请注意散热", "syncDelayMs": 200 },
    { "targetPeriphId": "device_alarm", "actionType": 21, "actionValue": "temp_high" }
  ]
}
```

### 7.4 平台下发控制 PWM 亮度

```json
{
  "id": "exec_platform_pwm",
  "name": "平台控制灯带亮度",
  "enabled": true,
  "triggers": [
    { "triggerType": 0, "triggerPeriphId": "pwm_led", "operatorType": 1, "compareValue": "" }
  ],
  "actions": [
    { "targetPeriphId": "pwm_led", "actionType": 4, "actionValue": "128", "useReceivedValue": true }
  ]
}
```
> 平台下发 `{"id":"pwm_led","value":"200"}` 时，PWM 占空比被设为 200（`useReceivedValue` 使 `actionValue` 被接收值覆盖）。

### 7.5 WiFi 断连告警 + OLED 显示

```json
{
  "id": "exec_wifi_lost",
  "name": "WiFi 断连告警",
  "enabled": true,
  "triggers": [
    { "triggerType": 4, "eventId": "wifi_disconnected" }
  ],
  "actions": [
    { "targetPeriphId": "status_led", "actionType": 2, "actionValue": "500" },
    { "targetPeriphId": "lcd_01", "actionType": 27, "actionValue": "# 网络异常\nWiFi 已断开\n系统将自动重连", "syncDelayMs": 300 }
  ]
}
```

### 7.6 定时数码管显示时间（TM1637）

```json
{
  "id": "exec_tm1637_time",
  "name": "数码管显示时间",
  "enabled": true,
  "triggers": [
    { "triggerType": 1, "timerMode": 0, "intervalSec": 30 }
  ],
  "actions": [
    { "targetPeriphId": "tm1637_01", "actionType": 24, "actionValue": "${sys.time_hhmm}" }
  ]
}
```

### 7.7 按键长按 5s 切换日/夜模式（规则联动）

```json
{
  "id": "exec_mode_switch",
  "name": "长按切模式",
  "enabled": true,
  "triggers": [
    { "triggerType": 4, "eventId": "button_long_press_5s", "triggerPeriphId": "btn_01" }
  ],
  "actions": [
    { "targetPeriphId": "exec_day_mode",   "actionType": 23 },
    { "targetPeriphId": "exec_night_mode", "actionType": 22 }
  ]
}
```

---

## 8. 最佳实践

### 8.1 触发类型选择

| 场景 | 推荐触发 |
|---|---|
| 平台远程控制 | 平台触发 (0) |
| 周期刷新显示 / 采集上报 | 定时触发 (1) |
| WiFi/MQTT/按键等系统状态变化 | 事件触发 (4) |
| 本地传感器阈值告警 | 轮询触发 (5) |

### 8.2 性能与资源

- **轮询间隔 ≥ 5 秒**：低于 5s 会被 `validateRuntimeConfig` 修正；Modbus 轮询建议 ≥ 10s。
- **活跃异步任务 ≤ 3**：`MAX_ASYNC_TASKS=3`；超出会排队。若规则密集，考虑改用 `execMode=1` 同步执行简单 GPIO 动作。
- **避免规则互相唤醒成环**：`ACTION_TRIGGER_EVENT` 配合 `EVENT_PERIPH_EXEC_COMPLETED` 时要加守卫条件，防止死循环。

### 8.3 编排与时序

- 多动作按数组顺序执行；用 `syncDelayMs` 做简单编排（如先打开风扇再刷新屏幕）。
- `syncDelayMs` 最大 10000ms；需要更长等待请拆成两条规则 + 事件桥接。

### 8.4 模板使用

- OLED 多行模板首行用 `#` 开头可得到居中标题+分隔线，提升可读性。
- 传感器数据字段名参考 [`peripheral-configuration-guide.md`](../peripherals/peripheral-configuration-guide.md) 的各传感器章节。
- `$value` 仅在 OLED 动作 27 中启用；其他动作如需接收值，用 `useReceivedValue=true`。

### 8.5 持久化与备份

- 配置文件 `/config/periph_exec.json` 位于 LittleFS；建议在 UI 的"导入/导出"功能中定期导出备份。
- 批量增删时注意单文件大小（长期观察 ≤ 32KB 比较稳妥，取决于 LittleFS 分区）。

---

## 9. 常见问题 FAQ

### Q1：为什么我的规则没有触发？

逐项排查：
1. 规则 `enabled` 是否为 `true`？
2. 触发类型字段是否匹配（如事件触发缺 `eventId`）？
3. 对应编译开关是否开启（如 Modbus 动作需 `FASTBEE_ENABLE_MODBUS_RTU/TCP`）？
4. 设备日志（`data/logs/system.log`）里是否有 `[PeriphExec]` 相关 warning？
5. 轮询触发下，传感器是否能正确读数（可在 UI 手动读取测试）？

### Q2：OLED 动作（27）指向了 TM1637 外设，会怎样？

运行时执行器会记录 `warning: "OLED_DISPLAY target '%s' is not LCD"` 并**直接返回失败**。UI 表单也会根据 `actionType` 过滤可选 `targetPeriphId`。务必让动作类型与目标外设类型匹配。

### Q3：MQTT 下发值怎么在 OLED 上显示？

两步：
1. 触发器用平台触发 `operatorType=1`（设置模式，不比较）。
2. 动作 27 的 `actionValue` 包含 `$value` 占位符。

下发 `{"id":"lcd_01","value":"Hello"}`，显示文本中 `$value` 会被替换为 `Hello`。

### Q4：轮询触发周期设置 1 秒可以吗？

不建议。系统会把 `<5s` 的周期修正到**绝对下限 5s**。Modbus 子设备轮询若总线上有多个设备，`intervalSec` 建议 ≥10s 并设置 `pollInterPollDelay ≥ 100ms` 避免总线拥堵。

### Q5：同一个传感器怎么同时做"本地告警"和"平台上报"？

两条规则：
- 规则 A：轮询触发温度 > 阈值 → 风扇+OLED（本地告警）
- 规则 B：定时触发每 60s → `ACTION_SENSOR_READ`（触发采集）+ `reportAfterExec=true`（上报 MQTT）

或一条规则的动作数组里串联：采集 → 告警 → 上报，启用 `reportAfterExec`。

### Q6：按键事件为什么没触发？

按键事件依赖 `GPIO_DIGITAL_INPUT_PULLUP` / `GPIO_DIGITAL_INPUT_PULLDOWN` 外设类型（见 `supportsButtonEvent`）。检查：
1. 外设类型是否正确（type=12 或 13）？
2. 消抖和双击窗口：按太快可能被识别为单次点击而非双击（双击窗口 200ms）。
3. `triggerPeriphId` 是否与按键外设 ID 匹配（不填则匹配所有按键）？

### Q7：`ACTION_TRIGGER_EVENT` 能发射哪些事件？

- **系统内置事件 ID**：例如 `break_down`/`device_alarm`/`low_power`/`restart` 等（见第 3.3 节）。
- **自定义事件**：先在外设管理中创建 `DEVICE_EVENT`（type=60）外设，其 `id` 即可作为 `targetPeriphId` 使用。

### Q8：一条规则执行会影响其他规则吗？

默认**异步执行**（独立 FreeRTOS 任务），互不阻塞。但底层硬件（如共享 I²C 总线、串口）仍需避免并发争用。规则数量多时优先使用异步，短小 GPIO 动作可考虑同步（`execMode=1`）减少任务创建开销。

### Q9：`reportAfterExec` 是什么？

执行完成后，把**动作执行结果**（含 `targetPeriphId`、`actualValue`、成功/失败原因）按 `protocolType` 偏好的协议上报平台。默认开启；若规则只做本地联动（如本地告警），可关闭以减少上行流量。

### Q10：怎么调试规则？

1. 打开串口日志（115200），关注 `[PeriphExec]` 前缀。
2. UI 的"规则测试"按钮（如有）可手动触发一次。
3. 配合 `ACTION_TRIGGER_EVENT` 发射 `device_alarm` 等事件，观察 MQTT 订阅端是否收到对应主题。
4. 临时把 `execMode` 改成同步，日志里可看到串行执行顺序，有助于定位失败步骤。

---

## 相关文档

- [外设配置指南](../peripherals/peripheral-configuration-guide.md) — 外设类型、引脚、编译开关
- [PeriphExec 模块深度文档](./periph_exec_flow.md) — 实现架构、任务调度、源码分析
- [OLED 使用指南](../peripherals/oled_usage_guide.md) — LCD/OLED 硬件连线与 U8g2 字库
- [Modbus 使用指南](../protocols/modbus_usage_guide.md) — Modbus RTU/TCP 子设备配置
- [脚本引擎指南](./script-guide.md) — `ACTION_SCRIPT` 动作使用的命令序列脚本
