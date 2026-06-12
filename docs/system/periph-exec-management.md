# 外设执行管理完整指南

## 一、功能概述

外设执行（Periph-Exec）是 FastBee 系统的**自动化规则引擎**，通过配置**触发器（Trigger）**和**动作（Action）**实现设备联动。配置存储在 `/config/periph_exec.json` 中。

出厂默认规则均为 `enabled: false`，不会在未知接线环境中自动执行。新增或导入规则后，建议先保存为禁用状态，确认目标外设、引脚和动作参数正确后再启用。

### 核心概念

| 概念 | 说明 | 类比 |
|------|------|------|
| **规则（Rule）** | 完整的自动化逻辑单元 | IF-THEN语句 |
| **触发器（Trigger）** | 何时执行规则（条件） | IF部分 |
| **动作（Action）** | 执行什么操作（结果） | THEN部分 |
| **执行模式（execMode）** | 动作执行策略 | 同步/异步 |
| **上报数据（reportAfterExec）** | 执行后是否上报状态 | 反馈机制 |

### 规则结构

```json
{
  "id": "exec_1717392000",
  "name": "高温报警",
  "enabled": true,
  "execMode": 0,
  "triggers": [
    { /* 触发器配置 */ }
  ],
  "actions": [
    { /* 动作配置 */ }
  ],
  "reportAfterExec": true
}
```

---

## 二、Web界面操作指南

### 2.1 访问外设执行页面

#### 方式1：独立页面（推荐）

1. 点击左侧菜单 **外设配置**
2. 页面自动切换到 **外设执行管理** 标签
3. 显示规则列表表格

#### 方式2：从外设联动进入

1. 在外设列表中点击某外设
2. 查看关联的规则

---

### 2.2 创建新规则

#### 操作步骤

1. **点击新增按钮**
   - 右上角 **<i class="fas fa-plus"></i> 新增规则**

2. **填写基础配置**

   | 字段 | 必填 | 说明 | 示例 |
   |------|------|------|------|
   | **规则名称** | ✅ | 有业务含义的名称 | `高温报警` |
   | **上报数据** | - | 执行后上报状态到云平台 | ✅ 默认启用 |
   | **启用** | - | 规则是否生效 | 首次建议关闭，确认后启用 |

3. **配置触发器**（第二部分）

   - 点击 **添加触发** 按钮
   - 选择触发类型（平台/事件/定时/轮询）
   - 填写对应参数

4. **配置动作**（第三部分）

   - 点击 **添加动作** 按钮
   - 选择动作类型（GPIO/PWM/脚本等）
   - 选择目标外设
   - 填写动作参数

5. **保存规则**
   - 点击 **保存** 按钮
   - 系统验证配置
   - 写入 `periph_exec.json`

#### 界面布局

```
┌────────────────────────────────────────────────────┐
│  新增外设执行                                [×]   │
├────────────────────────────────────────────────────┤
│  【基础配置】                                       │
│  规则名称: [高温报警_____________]                  │
│  ☑ 上报数据    ☑ 启用                              │
├────────────────────────────────────────────────────┤
│  触发配置                    [+ 添加触发]           │
│  ┌──────────────────────────────────────────────┐ │
│  │ 1  [×] 删除                                  │ │
│  │ 触发类型: [轮询触发          ▼]               │ │
│  │ 目标外设: [dht_room          ▼]               │ │
│  │ 轮询间隔: [5   ] 秒    超时: [1000] ms        │ │
│  │ 重试次数: [2   ] 次    延迟: [100 ] ms        │ │
│  │ 比较操作: [大于              ▼]  比较值: [30] │ │
│  └──────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────┤
│  执行动作配置                [+ 添加动作]           │
│  ┌──────────────────────────────────────────────┐ │
│  │ 1  [×] 删除                                  │ │
│  │ 动作类型: [高电平            ▼]               │ │
│  │ 目标外设: [relay_fan         ▼]               │ │
│  │ 执行模式: [同步              ▼]               │ │
│  └──────────────────────────────────────────────┘ │
│  ┌──────────────────────────────────────────────┐ │
│  │ 2  [×] 删除                                  │ │
│  │ 动作类型: [触发事件          ▼]               │ │
│  │ 事件ID:   [alarm_high_temp___]                │ │
│  └──────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────┤
│                  [取消]    [保存]                  │
└────────────────────────────────────────────────────┘
```

---

### 2.3 触发器配置详解

#### 触发器类型对比

| 类型 | 触发时机 | 适用场景 | 配置难度 |
|------|---------|---------|---------|
| **平台触发** | 收到云平台指令 | IoT平台远程控制 | ⭐⭐ |
| **事件触发** | 系统/传感器事件 | 按键按下/WiFi连接 | ⭐⭐⭐ |
| **定时触发** | 时间到达 | 周期采集/定时开关 | ⭐ |
| **轮询触发** | 轮询检测条件满足 | 传感器值判断 | ⭐⭐ |

---

#### 类型1：平台触发（triggerType=0）

**触发条件**：收到云平台下发的MQTT/Modbus指令

##### 配置参数

| 参数 | 必填 | 说明 | 示例 |
|------|------|------|------|
| **目标外设** | ✅ | 接收指令的外设 | `uart_modbus` |
| **操作符** | ✅ | 匹配/设置模式 | 匹配 |
| **比较值** | 条件 | 匹配的指令内容 | `{"cmd":"on"}` |

##### 操作符说明

| 操作符值 | 名称 | 说明 |
|---------|------|------|
| 0 | 匹配模式 | 接收数据等于比较值时触发 |
| 1 | 设置模式 | 接收数据直接作为动作参数 |

##### 配置示例

```json
{
  "triggerType": 0,
  "triggerPeriphId": "uart_modbus",
  "operatorType": 0,
  "compareValue": "{\"cmd\":\"fan_on\"}"
}
```

**触发流程**：
```
云平台发送: {"cmd":"fan_on"}
        ↓
UART接收数据
        ↓
匹配 compareValue
        ↓
触发规则执行动作
```

---

#### 类型2：事件触发（triggerType=4）

**触发条件**：系统事件、传感器数据事件、按键事件

##### 事件分类

| 事件类别 | 事件ID前缀 | 说明 |
|---------|-----------|------|
| 系统事件 | `sys:` | WiFi/MQTT/NTP状态变化 |
| 传感器数据 | `ds:` | 传感器读取数据 |
| 按键事件 | `button_` | 物理按键按下 |
| Modbus控制 | `mc:` | Modbus子设备事件 |
| 自定义事件 | 无前缀 | 用户自定义事件 |

##### 配置步骤

1. **选择事件类别**
   - 从下拉框选择（系统/传感器/按键等）

2. **选择具体事件**
   - 根据类别自动加载事件列表

3. **配置条件判断**（传感器事件）
   - 比较操作符：等于/大于/小于/区间等
   - 比较值：阈值

##### 配置示例（温度>30℃）

```json
{
  "triggerType": 4,
  "eventId": "ds:dht_room.temperature",
  "operatorType": 2,
  "compareValue": "30"
}
```

##### 常用系统事件

| 事件ID | 触发时机 |
|--------|---------|
| `sys:wifi.connected` | WiFi连接成功 |
| `sys:wifi.disconnected` | WiFi断开 |
| `sys:mqtt.connected` | MQTT连接成功 |
| `sys:mqtt.disconnected` | MQTT断开 |
| `sys:boot.completed` | 系统启动完成 |

##### 按键事件

外设需配置为 **GPIO_INTERRUPT** 类型（type=18/19/20），自动生成事件：

```
按键按下: button_{periphId}.pressed
按键长按: button_{periphId}.long_press
```

---

#### 类型3：定时触发（triggerType=1）

**触发条件**：时间到达指定间隔或时间点

##### 定时模式

| 模式 | timerMode | 说明 | 适用场景 |
|------|-----------|------|---------|
| **间隔定时** | 0 | 每隔X秒执行 | 周期采集 |
| **每日定时** | 1 | 每天固定时间执行 | 定时开关 |

##### 间隔定时配置

| 参数 | 必填 | 说明 | 示例 |
|------|------|------|------|
| **间隔秒数** | ✅ | 执行间隔（1~86400秒） | `60`（1分钟） |

##### 每日定时配置

| 参数 | 必填 | 说明 | 示例 |
|------|------|------|------|
| **时间点** | ✅ | 执行时间（HH:MM格式） | `08:30` |

##### 配置示例

```json
// 间隔定时：每5分钟执行
{
  "triggerType": 1,
  "timerMode": 0,
  "intervalSec": 300
}

// 每日定时：每天早上8:30执行
{
  "triggerType": 1,
  "timerMode": 1,
  "timePoint": "08:30"
}
```

---

#### 类型4：轮询触发（triggerType=5）

**触发条件**：主动轮询传感器数据并判断条件

##### 与事件触发的区别

| 特性 | 事件触发 | 轮询触发 |
|------|---------|---------|
| **数据来源** | 被动接收事件 | 主动读取传感器 |
| **实时性** | 高（事件驱动） | 中（依赖轮询间隔） |
| **适用传感器** | 所有类型 | 本地传感器 |
| **配置复杂度** | 高（需配置事件） | 低（直接选外设） |

##### 配置参数

| 参数 | 必填 | 默认值 | 取值范围 | 说明 |
|------|------|--------|---------|------|
| **目标外设** | ✅ | - | - | 要轮询的传感器 |
| **轮询间隔** | ✅ | 60秒 | 5~86400秒 | 读取间隔 |
| **响应超时** | - | 1000ms | 100~5000ms | 读取超时时间 |
| **重试次数** | - | 2次 | 0~3次 | 失败重试 |
| **轮询延迟** | - | 100ms | 20~1000ms | 两次读取间隔 |
| **比较操作** | ✅ | - | 0~9 | 条件判断 |
| **比较值** | 条件 | - | - | 阈值 |

##### 比较操作符

| operatorType | 名称 | 示例 | 说明 |
|:------------:|------|------|------|
| 0 | 等于 | `value == 30` | 精确匹配 |
| 1 | 不等于 | `value != 0` | 非零判断 |
| 2 | 大于 | `value > 30` | 上限报警 |
| 3 | 小于 | `value < 10` | 下限报警 |
| 4 | 大于等于 | `value >= 30` | 包含边界 |
| 5 | 小于等于 | `value <= 10` | 包含边界 |
| 6 | 区间内 | `compareValue="20,30"` | 正常范围 |
| 7 | 区间外 | `compareValue="20,30"` | 异常范围 |
| 8 | 包含 | `value.contains("ON")` | 字符串匹配 |
| 9 | 不包含 | `!value.contains("OFF")` | 字符串匹配 |

##### 配置示例（温度>30℃报警）

```json
{
  "triggerType": 5,
  "triggerPeriphId": "dht_room",
  "intervalSec": 5,
  "pollResponseTimeout": 1000,
  "pollMaxRetries": 2,
  "pollInterPollDelay": 100,
  "operatorType": 2,
  "compareValue": "30"
}
```

**执行流程**：
```
每隔5秒
    ↓
读取 dht_room 传感器
    ↓
获取 temperature 值
    ↓
判断: temperature > 30 ?
    ↓ 是
触发规则执行动作
```

---

### 2.4 动作配置详解

#### 动作类型速查表

| actionType | 名称 | 参数 | 目标外设 | 说明 |
|:----------:|------|------|---------|------|
| 0 | 高电平 | 无 | ✅ GPIO | 输出HIGH |
| 1 | 低电平 | 无 | ✅ GPIO | 输出LOW |
| 2 | 闪烁 | actionValue=间隔ms | ✅ GPIO | 周期性闪烁 |
| 3 | 呼吸灯 | actionValue=周期ms | ✅ PWM | 渐变效果 |
| 4 | 设置PWM | actionValue=占空比 | ✅ PWM | 0~4095 |
| 5 | 设置DAC | actionValue=0~255 | ✅ DAC | 模拟输出 |
| 6 | 系统重启 | 无 | ❌ | 重启设备 |
| 7 | 恢复出厂 | 无 | ✖ | 重置配置为默认值 |
| 8 | NTP同步 | 无 | ❌ | 同步时间 |
| 9 | OTA升级 | actionValue=URL | ❌ | 远程升级 |
| 10 | 调用外设 | actionValue=方法 | ✅ 任意 | 调用外设方法 |
| 15 | 命令脚本 | actionValue=脚本 | ❌ | PERIPH/DELAY序列 |
| 16 | Modbus线圈 | actionValue=0/1 | ✅ Modbus | 控制继电器 |
| 17 | Modbus寄存器 | actionValue=值 | ✅ Modbus | 写寄存器 |
| 18 | Modbus轮询 | 自动配置 | ✅ Modbus | 读取数据 |
| 19 | 传感器读取 | actionValue=配置 | ✅ 传感器 | 采集数据 |
| 21 | 触发事件 | actionValue=事件ID | ❌ | 发射事件 |
| 22 | 启用规则 | targetPeriphId=规则ID | ❌ | 动态启用 |
| 23 | 禁用规则 | targetPeriphId=规则ID | ❌ | 动态禁用 |
| 24 | 显示数字 | actionValue="12.34" | ✅ 数码管 | 7段显示 |
| 25 | 显示文本 | actionValue="PLAY" | ✅ 数码管 | 字符显示 |
| 26 | 清屏 | 无 | ✅ 显示屏 | 清空内容 |
| 27 | OLED显示 | actionValue=模板 | ✅ OLED | 多行文本 |

---

#### 动作1~0/1：GPIO高/低电平

**最基础的控制动作**

##### 配置参数

| 参数 | 必填 | 说明 |
|------|------|------|
| **目标外设** | ✅ | GPIO数字输出外设ID |

##### 配置示例

```json
{
  "actionType": 0,
  "targetPeriphId": "relay_fan"
}
```

---

#### 动作2：闪烁

##### 配置参数

| 参数 | 必填 | 说明 | 示例 |
|------|------|------|------|
| **目标外设** | ✅ | GPIO外设 | `led_status` |
| **闪烁间隔** | ✅ | 亮/灭时间（ms） | `500` |

##### 配置示例

```json
{
  "actionType": 2,
  "targetPeriphId": "led_alarm",
  "actionValue": "500"
}
```

**效果**：LED每500ms翻转一次（1秒周期）

---

#### 动作15：命令脚本

**灵活的多步控制**

##### 支持的命令

| 命令 | 格式 | 说明 |
|------|------|------|
| PERIPH | `PERIPH <id> <动作> [值]` | 控制外设 |
| DELAY | `DELAY <毫秒>` | 延时 |
| LOG | `LOG <消息>` | 打印日志 |
| MQTT | `MQTT <qos> <payload>` | 发送消息 |

##### PERIPH子命令

| 子命令 | 格式 | 说明 |
|--------|------|------|
| HIGH | `PERIPH led1 HIGH` | 高电平 |
| LOW | `PERIPH led1 LOW` | 低电平 |
| PWM | `PERIPH motor PWM 2048` | 设置PWM |
| BLINK | `PERIPH led BLINK 500` | 闪烁 |
| COLOR | `PERIPH strip COLOR #FF0000` | NeoPixel颜色 |

##### 配置示例（流水灯）

```json
{
  "actionType": 15,
  "actionValue": "PERIPH led1 LOW\nDELAY 300\nPERIPH led1 HIGH\nPERIPH led2 LOW\nDELAY 300\nPERIPH led2 HIGH\nPERIPH led3 LOW\nDELAY 300\nPERIPH led3 HIGH\nLOG 流水灯一轮完成"
}
```

**执行流程**：
```
点亮led1 → 延时300ms → 熄灭led1
    ↓
点亮led2 → 延时300ms → 熄灭led2
    ↓
点亮led3 → 延时300ms → 熄灭led3
    ↓
打印日志
```

---

#### 动作19：传感器读取

##### 配置参数

```json
{
  "actionType": 19,
  "targetPeriphId": "dht_room",
  "actionValue": "{\"dataField\":\"temperature\",\"decimalPlaces\":1}"
}
```

##### actionValue字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| **dataField** | 字符串 | "temperature" | 要读取的字段 |
| **decimalPlaces** | 整数 | 2 | 小数位数 |
| **scaleFactor** | 浮点 | 1.0 | 缩放系数 |
| **offset** | 浮点 | 0 | 偏移量 |

##### 可用dataField

| 传感器类型 | dataField | 单位 |
|-----------|-----------|------|
| DHT | temperature, humidity | ℃, % |
| DS18B20 | temperature | ℃ |
| HC-SR04 | distance | cm |
| BH1750 | light | lux |
| BMP280 | temperature, pressure | ℃, hPa |

---

#### 动作27：OLED显示

##### 配置参数

```json
{
  "actionType": 27,
  "targetPeriphId": "oled_ctrl",
  "actionValue": "温度: ${temperature}℃\n湿度: ${humidity}%"
}
```

##### 模板变量

- `${temperature}` - 从触发器数据中获取
- `${humidity}` - 从触发器数据中获取
- `${value}` - 当前轮询值

##### 显示效果

```
┌─────────────────┐
│ 温度: 28.5℃     │
│ 湿度: 65%       │
└─────────────────┘
```

---

### 2.5 编辑规则

#### 操作步骤

1. 在规则列表找到目标规则
2. 点击 **编辑** 按钮
3. 模态窗自动填充当前配置
4. 修改触发器或动作
5. 点击 **保存** 确认

#### 注意事项

- ⚠️ **触发器/动作可增删**：点击删除按钮移除
- ⚠️ **修改后立即生效**：保存后系统自动重新加载
- ⚠️ **引用检查**：修改目标外设ID需确认外设存在

---

### 2.6 启用/禁用规则

#### 操作方式

| 方式 | 操作 | 效果 |
|------|------|------|
| **列表快捷操作** | 点击启用/禁用按钮 | 立即切换 |
| **编辑模态窗** | 切换启用开关 | 保存后生效 |

#### 使用场景

- **调试**：禁用规则观察系统行为
- **临时停用**：保留配置但不执行
- **测试**：逐个启用规则排查问题

---

### 2.7 删除规则

#### 操作步骤

1. 点击 **删除** 按钮（红色）
2. 确认对话框
3. 从 `periph_exec.json` 移除

#### ⚠️ 警告

- ❌ **删除不可恢复**
- 💡 **建议先禁用观察**
- 🔗 **检查依赖**：是否有其他规则引用

---

## 三、配置实战演练

### 3.1 场景1：智能温控系统

#### 需求

- 每5秒读取DHT22温度
- 温度>30℃时启动风扇
- 温度<25℃时关闭风扇
- OLED显示当前温度

#### 步骤1：配置外设

（参考外设配置文档，添加dht_room、relay_fan、oled_ctrl）

#### 步骤2：创建高温启动规则

1. **点击新增规则**
2. **基础配置**
   - 名称：`高温启动风扇`
   - 上报数据：✅
   - 启用：✅

3. **添加触发器**
   - 触发类型：轮询触发
   - 目标外设：`dht_room`
   - 轮询间隔：5秒
   - 比较操作：大于
   - 比较值：30

4. **添加动作1**
   - 动作类型：高电平
   - 目标外设：`relay_fan`

5. **添加动作2**
   - 动作类型：OLED显示
   - 目标外设：`oled_ctrl`
   - 动作值：`温度: ${temperature}℃\n状态: 风扇启动`

6. **保存**

#### 完整JSON配置

```json
{
  "id": "exec_high_temp_start",
  "name": "高温启动风扇",
  "enabled": true,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 5,
      "triggerPeriphId": "dht_room",
      "intervalSec": 5,
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100,
      "operatorType": 2,
      "compareValue": "30"
    }
  ],
  "actions": [
    {
      "actionType": 0,
      "targetPeriphId": "relay_fan",
      "execMode": 0
    },
    {
      "actionType": 27,
      "targetPeriphId": "oled_ctrl",
      "actionValue": "温度: ${temperature}℃\n状态: 风扇启动",
      "execMode": 0
    }
  ],
  "reportAfterExec": true
}
```

#### 步骤3：创建低温关闭规则

（类似步骤2，比较操作改为"小于"，比较值改为25，动作为低电平）

---

### 3.2 场景2：按键控制灯光

#### 需求

- 短按按键：切换LED开关
- 长按按键：启动呼吸灯模式

#### 配置外设

```json
{
  "peripherals": [
    {
      "id": "btn_ctrl",
      "name": "控制按键",
      "type": 18,
      "enabled": true,
      "pins": [0]
    },
    {
      "id": "led_main",
      "name": "主LED",
      "type": 12,
      "enabled": true,
      "pins": [15],
      "params": { "initialState": 0 }
    }
  ]
}
```

#### 规则1：短按切换

```json
{
  "id": "exec_btn_short",
  "name": "短按切换LED",
  "enabled": true,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "button_btn_ctrl.pressed"
    }
  ],
  "actions": [
    {
      "actionType": 10,
      "targetPeriphId": "led_main",
      "actionValue": "toggle",
      "execMode": 0
    }
  ]
}
```

#### 规则2：长按呼吸灯

```json
{
  "id": "exec_btn_long",
  "name": "长按呼吸灯",
  "enabled": true,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "button_btn_ctrl.long_press"
    }
  ],
  "actions": [
    {
      "actionType": 3,
      "targetPeriphId": "led_main",
      "actionValue": "3000",
      "execMode": 0
    }
  ]
}
```

---

### 3.3 场景3：定时上报传感器数据

#### 需求

- 每1分钟读取温湿度
- 通过MQTT上报到云平台

#### 规则配置

```json
{
  "id": "exec_periodic_report",
  "name": "定时数据上报",
  "enabled": true,
  "triggers": [
    {
      "triggerType": 1,
      "timerMode": 0,
      "intervalSec": 60
    }
  ],
  "actions": [
    {
      "actionType": 19,
      "targetPeriphId": "dht_room",
      "actionValue": "{\"dataField\":\"temperature\"}",
      "execMode": 0
    },
    {
      "actionType": 15,
      "actionValue": "MQTT 0 [{\"id\":\"temperature\",\"value\":\"${temperature}\"},{\"id\":\"humidity\",\"value\":\"${humidity}\"}]"
    }
  ]
}
```

---

## 四、执行模式详解

### 4.1 同步模式（execMode=0）

**特点**：
- 动作按顺序执行
- 前一个动作完成后才执行下一个
- 适合有依赖关系的动作

**示例**：
```
动作1: 读取传感器
    ↓ （等待完成）
动作2: 判断数值
    ↓ （等待完成）
动作3: 控制继电器
```

---

### 4.2 异步模式（execMode=1）

**特点**：
- 动作立即返回，不等待完成
- 多个动作可并行执行
- 适合独立动作

**示例**：
```
动作1: 打开LED     ─┐
动作2: 打开蜂鸣器   ├─ 同时执行
动作3: 发送MQTT    ─┘
```

---

### 4.3 选择建议

| 场景 | 推荐模式 | 原因 |
|------|---------|------|
| 传感器读取→控制 | 同步 | 需要等待数据 |
| 多设备同时控制 | 异步 | 提高响应速度 |
| 命令脚本 | 同步 | 命令有顺序依赖 |
| 日志记录 | 异步 | 不影响主流程 |

---

## 五、常见错误与解决方案

### 5.1 配置错误

| 错误 | 原因 | 解决方案 |
|------|------|---------|
| **规则不触发** | enabled=false | 启用规则 |
| **触发条件不满足** | 比较值错误 | 检查传感器数据范围 |
| **动作无效果** | 目标外设未启用 | 确认外设enabled=true |
| **脚本报错** | 语法错误 | 查看日志排查 |
| **定时不准** | NTP未同步 | 确保网络连通 |
| **轮询超时** | 传感器响应慢 | 增加pollResponseTimeout |
| **事件不触发** | 事件ID错误 | 检查前缀和拼写 |

### 5.2 排查步骤

1. **检查启用状态**
   - 规则enabled: true
   - 外设enabled: true

2. **查看统计信息**
   - 列表显示触发次数
   - 0次表示未触发

3. **查看日志**
   - Web界面日志
   - 串口日志

4. **简化测试**
   - 先测试单个触发器
   - 先测试单个动作

5. **手动触发**
   - 使用API触发规则
   - 验证动作是否正常

6. **检查数据源**
   - 传感器是否正常读取
   - 事件是否正确发射

---

## 六、高级技巧

### 6.1 多触发器OR逻辑

一个规则可以有多个触发器，**任一触发**即执行：

```json
{
  "triggers": [
    { "triggerType": 4, "eventId": "button_btn1.pressed" },
    { "triggerType": 4, "eventId": "button_btn2.pressed" },
    { "triggerType": 1, "timerMode": 0, "intervalSec": 3600 }
  ]
}
```

**触发条件**：按键1 **或** 按键2 **或** 每小时定时

---

### 6.2 规则联动

规则可以启用/禁用其他规则：

```json
// 规则1：启用规则2
{
  "actions": [
    {
      "actionType": 22,
      "targetPeriphId": "exec_rule2_id"
    }
  ]
}

// 规则2：禁用自己
{
  "actions": [
    {
      "actionType": 23,
      "targetPeriphId": "exec_rule2_id"
    }
  ]
}
```

---

### 6.3 使用系统事件

监听WiFi/MQTT状态变化：

```json
{
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "sys:wifi.connected"
    }
  ],
  "actions": [
    {
      "actionType": 15,
      "actionValue": "LOG WiFi已连接，IP: ${ip}\nMQTT 0 {\"event\":\"wifi_connected\",\"ip\":\"${ip}\"}"
    }
  ]
}
```

---

### 6.4 性能优化

| 优化项 | 方法 | 效果 |
|--------|------|------|
| 降低轮询频率 | 增加intervalSec | 减少CPU占用 |
| 使用事件触发 | 代替轮询 | 实时响应 |
| 禁用未使用规则 | enabled=false | 节省资源 |
| 合并相似规则 | 多触发器OR | 简化配置 |
| 异步执行独立动作 | execMode=1 | 提高响应 |

---

## 七、API参考

### 7.1 查询规则列表

```
GET /api/periph-exec/rules
```

**响应**：
```json
{
  "success": true,
  "data": [
    {
      "id": "exec_xxx",
      "name": "高温报警",
      "enabled": true,
      "triggerCount": 2,
      "actionCount": 3
    }
  ]
}
```

---

### 7.2 手动触发规则

```
POST /api/periph-exec/rules/{ruleId}/trigger
```

---

### 7.3 启用/禁用规则

```
POST /api/periph-exec/rules/{ruleId}/toggle
```

---

### 7.4 删除规则

```
DELETE /api/periph-exec/rules/{ruleId}
```

---

## 八、相关文档

- [外设配置指南](./peripheral-management.md) - 外设添加和配置
- [触发器详细文档](../periph-exec/triggers/) - 各触发类型详解
- [动作详细文档](../periph-exec/actions/) - 各动作类型详解
- [完整场景示例](../periph-exec/scenarios/) - 实战配置案例
- [命令脚本流水灯](../examples/03b-led-flowing-command-script.md) - 脚本动作示例

---

**提示**：外设执行是FastBee系统的核心功能，建议先从简单的定时触发+GPIO控制开始，逐步学习事件触发和脚本动作。
