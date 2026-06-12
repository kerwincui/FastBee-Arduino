# GPIO 数字输入

## 功能说明

GPIO 数字输入用于读取按键、开关、传感器数字信号等。支持上拉/下拉模式，部分类型支持中断和按键事件检测。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| GPIO_DIGITAL_INPUT | 11 | 浮空输入 |
| GPIO_DIGITAL_INPUT_PULLUP | 13 | 内部上拉输入 |
| GPIO_DIGITAL_INPUT_PULLDOWN | 14 | 内部下拉输入 |
| GPIO_INTERRUPT_RISING | 18 | 上升沿中断 |
| GPIO_INTERRUPT_FALLING | 19 | 下降沿中断 |
| GPIO_INTERRUPT_CHANGE | 20 | 电平变化中断 |
| GPIO_TOUCH | 21 | 触摸输入 |

## 按键事件检测

**仅 PULLUP(13) 和 PULLDOWN(14) 类型**支持内置按键事件检测：

| 事件 | eventId | 说明 |
|------|---------|------|
| 单击 | button_click | 短按后释放 |
| 双击 | button_double_click | 连续两次短按 |
| 长按2秒 | button_long_press_2s | 持续按住2秒 |
| 长按5秒 | button_long_press_5s | 持续按住5秒 |
| 长按10秒 | button_long_press_10s | 持续按住10秒 |
| 按下 | button_press | 按键按下瞬间 |
| 释放 | button_release | 按键释放瞬间 |

![GPIO、ADC、PWM 接线校验图](../images/gpio-adc-pwm-wiring-check.svg)

数字输入排查优先看上拉/下拉和默认电平；按键、门磁、PIR 等模块建议先用日志确认事件，再接入外设执行规则。

![按键与事件触发链路](../images/event-trigger-flow.svg)

事件触发不生效时，先确认输入是否产生 `eventId`，再检查规则是否启用、`triggerPeriphId` 是否匹配、动作是否能手动执行。

## 典型应用

| 应用 | 类型 | 接线方式 |
|------|------|----------|
| 按键 | PULLUP(13) | 按键一端接GPIO，另一端接GND |
| 红外避障 | PULLUP(13) | DO → GPIO（有障碍输出低电平） |
| PIR人体红外 | PULLDOWN(14) | OUT → GPIO（检测到人输出高电平） |
| 干簧管 | PULLUP(13) | 信号端 → GPIO |
| 对射光电 | PULLUP(13) | DO → GPIO |

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。GPIO 输入类外设重点核对输入模式、引脚号和是否需要上拉/下拉。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加GPIO数字输入外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `btn1` 或 `pir1` | 唯一标识符 |
   | **名称** | `功能按键` 或 `人体红外检测` | 显示名称 |
   | **外设类型** | **GPIO数字输入上拉** (type: 13) | 按键推荐 |
   | **引脚配置** | `0` 或 `27` | 根据实际接线填写 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 查看实时状态值

> 💡 **提示**：PULLUP(13) 和 PULLDOWN(14) 类型支持按键事件检测（单击、双击、长按）

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### 按键输入（上拉）

```json
{
  "id": "btn1",
  "name": "功能按键",
  "type": 13,
  "enabled": false,
  "pins": [0],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### PIR 人体红外传感器（下拉）

```json
{
  "id": "pir1",
  "name": "人体红外检测",
  "type": 14,
  "enabled": false,
  "pins": [27],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### 中断输入（上升沿）

```json
{
  "id": "interrupt1",
  "name": "计数中断",
  "type": 18,
  "enabled": false,
  "pins": [34],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

## 与外设执行联动

### 作为平台触发数据源

数字输入的状态变化会通过 MQTT 上报，可用作平台触发的条件：

```json
{
  "triggerType": 0,
  "triggerPeriphId": "btn1",
  "operatorType": 0,
  "compareValue": "0"
}
```

### 作为事件触发源

按键事件可直接触发外设执行规则：

```json
{
  "triggerType": 4,
  "triggerPeriphId": "btn1",
  "eventId": "button_click"
}
```

> `triggerPeriphId` 为空时，任意按键的对应事件都会触发该规则。

### Web界面配置步骤

**创建事件触发规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件源：**btn1**
   - 事件类型：**button_click**（单击）
4. 添加动作（如控制LED或继电器）
5. 点击 **保存**

## 注意事项

1. **上拉/下拉选择**：按键通常使用上拉（按下接GND），PIR等有源传感器根据输出极性选择
2. **消抖处理**：系统内置软件消抖（~50ms），无需外部消抖电路
3. **中断引脚**：ESP32 所有 GPIO 均可配置中断，但中断回调中避免耗时操作
4. **触摸引脚**：ESP32 仅 GPIO 0/2/4/12/13/14/15/27/32/33 支持 TOUCH 功能
5. **GPIO34-39**：ESP32 的 GPIO 34-39 仅支持输入，无内部上拉/下拉
