# PWM 输出

## 功能说明

PWM（脉冲宽度调制）输出用于控制LED亮度、舵机角度、电机调速等。通过改变占空比实现模拟控制效果。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| GPIO_PWM_OUTPUT | 17 | 通用PWM输出（LED调光/电机调速） |
| PWM_SERVO | 41 | 舵机控制（50Hz固定频率） |

## 参数说明

### PWM 输出参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| pwmChannel | PWM通道（0-15） | 0 |
| pwmFrequency | PWM频率（Hz） | 1000 |
| pwmResolution | 分辨率（位数，1-16） | 8 |
| defaultDuty | 默认占空比 | 0 |

### Servo 参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| frequency | PWM频率 | 50 |
| minPulse | 最小脉宽(μs) | 544 |
| maxPulse | 最大脉宽(μs) | 2400 |

![GPIO、ADC、PWM 接线校验图](../images/gpio-adc-pwm-wiring-check.svg)

PWM 调试先从低占空比或中位角度开始，确认电源和负载响应正常后，再接入自动规则；舵机和电机建议使用独立供电并共地。

## 占空比范围

占空比范围由 `pwmResolution` 决定：
- 8位分辨率：0 ~ 255
- 10位分辨率：0 ~ 1023
- 12位分辨率：0 ~ 4095

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。PWM 输出保存前重点核对引脚、频率、分辨率和初始占空比。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加PWM输出外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `led_pwm` 或 `motor_pwm` | 唯一标识符 |
   | **名称** | `LED调光灯` 或 `直流电机调速` | 显示名称 |
   | **外设类型** | **GPIO PWM输出** (type: 17) | 通用PWM |
   | **引脚配置** | `5` 或 `18` | 根据实际接线填写 |
   | **PWM频率** | `1000` | LED用1000Hz，电机用5000Hz |
   | **PWM分辨率** | `8` | 8位=0-255，12位=0-4095 |
   | **默认占空比** | `0` | 初始亮度/速度 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 使用控制面板调整PWM值

> 💡 **提示**：舵机请选择 **PWM舵机** (type: 41)，频率固定50Hz

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### LED 调光（8位 PWM）

```json
{
  "id": "led_pwm",
  "name": "LED调光灯",
  "type": 17,
  "enabled": false,
  "pins": [5],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### 电机调速（12位 PWM）

```json
{
  "id": "motor_pwm",
  "name": "直流电机调速",
  "type": 17,
  "enabled": false,
  "pins": [18],
  "params": {
    "initialState": 0,
    "pwmChannel": 1,
    "pwmFrequency": 5000,
    "pwmResolution": 12,
    "defaultDuty": 0
  }
}
```

### SG90 舵机

```json
{
  "id": "servo1",
  "name": "SG90舵机",
  "type": 41,
  "enabled": false,
  "pins": [16],
  "params": {
    "frequency": 50,
    "minPulse": 544,
    "maxPulse": 2400
  }
}
```

## 与外设执行联动

### 设置PWM占空比

```json
{
  "targetPeriphId": "led_pwm",
  "actionType": 4,
  "actionValue": "128",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

### Web界面配置步骤

**创建PWM控制规则**

1. 切换到 **外设执行管理** 标签
2. 创建规则并配置触发器（如定时触发、平台触发）
3. 添加PWM动作：
   - 动作类型：**设置PWM**
   - 目标外设：**led_pwm**
   - 动作值：**128**（50%占空比，8位分辨率）
4. 点击 **保存**

> 💡 **提示**：呼吸灯效果可选择“呼吸灯”动作类型，actionValue为周期毫秒数

---

### JSON配置示例

### 呼吸灯效果

```json
{
  "targetPeriphId": "led_pwm",
  "actionType": 3,
  "actionValue": "2000",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

> `actionValue` 为呼吸周期（毫秒）

### 舵机角度控制

舵机通过 `ACTION_SET_PWM(4)` 控制，actionValue 为角度对应的脉宽值。

## 脚本联动示例

使用命令脚本实现 PWM 渐变：

```
PERIPH led_pwm PWM 0
DELAY 500
PERIPH led_pwm PWM 1000
DELAY 500
PERIPH led_pwm PWM 2000
DELAY 500
PERIPH led_pwm PWM 4095
```

## 注意事项

1. **通道冲突**：同一 PWM 通道不可被多个外设共用，配置时确保 `pwmChannel` 唯一
2. **频率选择**：LED调光建议 1000Hz，电机驱动建议 5000-25000Hz，舵机固定 50Hz
3. **分辨率与频率**：高频率下可用分辨率降低（ESP32 限制：freq × 2^resolution ≤ 80MHz）
4. **舵机供电**：舵机需独立供电，不要直接从ESP32 3.3V引脚取电
5. **平台下发**：平台可通过 `[{"id":"led_pwm","value":"128"}]` 设置占空比
