# GPIO 数字输出

## 功能说明

GPIO 数字输出用于控制继电器、LED、蜂鸣器、直流电机等执行器件。输出高/低电平驱动外部负载。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| GPIO_DIGITAL_OUTPUT | 12 | 标准数字输出 |

## 引脚说明

- **ESP32**: GPIO 0-5, 12-33（部分引脚有限制）
- **ESP32-C3**: GPIO 0-10, 18-21
- **ESP32-S3**: GPIO 0-21, 35-48

> 注意：GPIO 6-11（ESP32）为内部Flash使用，不可配置为输出。

## 典型应用

| 应用 | 接线方式 | 说明 |
|------|----------|------|
| LED | GPIO → 电阻(220Ω) → LED → GND | 高电平点亮 |
| 继电器 | GPIO → 继电器模块IN | 多数模块低电平触发 |
| 蜂鸣器 | GPIO → 有源蜂鸣器+ | 高电平发声 |
| 直流电机 | GPIO → 电机驱动模块 | 需外部驱动电路 |

## 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加GPIO数字输出外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `led1` 或 `relay1` | 唯一标识符 |
   | **名称** | `状态指示灯` 或 `风扇继电器` | 显示名称 |
   | **外设类型** | **GPIO数字输出** (type: 12) | 标准数字输出 |
   | **引脚配置** | `2` 或 `15` | 根据实际接线填写 |
   | **初始状态** | `0` | 0=低电平，1=高电平 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 使用控制面板测试输出状态

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### LED 输出

```json
{
  "id": "led1",
  "name": "状态指示灯",
  "type": 12,
  "enabled": false,
  "pins": [2],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### 继电器输出（低电平触发）

```json
{
  "id": "relay1",
  "name": "继电器-风扇",
  "type": 12,
  "enabled": false,
  "pins": [15],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### 蜂鸣器

```json
{
  "id": "buzzer",
  "name": "报警蜂鸣器",
  "type": 12,
  "enabled": false,
  "pins": [4],
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

GPIO 数字输出可作为外设执行规则的**动作目标**：

| 动作类型 | actionType | 说明 |
|----------|------------|------|
| ACTION_HIGH | 0 | 输出高电平 |
| ACTION_LOW | 1 | 输出低电平 |
| ACTION_HIGH_INVERTED | 13 | 逻辑高（物理低电平） |
| ACTION_LOW_INVERTED | 14 | 逻辑低（物理高电平） |
| ACTION_BLINK | 2 | 闪烁（actionValue=间隔ms） |
| ACTION_BREATHE | 3 | 呼吸灯效果 |

### Web界面配置步骤

**步骤1：创建执行规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 填写基础配置并配置触发器

**步骤2：添加GPIO动作**

1. 点击 **添加动作** 按钮
2. 填写动作配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **动作类型** | 选择 **高电平** 或 **低电平** | 根据需求选择 |
   | **目标外设** | 选择 `relay1` | GPIO输出外设 |
   | **执行延时** | `0` | 立即执行 |

3. 点击 **保存**

> 💡 **提示**：低电平触发继电器模块请使用“逻辑高（反转）”或“逻辑低（反转）”

---

### JSON配置示例

```json
{
  "targetPeriphId": "relay1",
  "actionType": 0,
  "actionValue": "",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

## 注意事项

1. **初始状态**：`initialState` 为 0 表示上电默认低电平，1 表示高电平
2. **低电平触发继电器**：使用 `ACTION_HIGH_INVERTED(13)` / `ACTION_LOW_INVERTED(14)` 实现逻辑反转
3. **电流限制**：ESP32 单引脚最大输出电流 40mA，驱动大功率负载需外接驱动电路
4. **PWM 参数**：即使是数字输出类型，params 中的 PWM 参数仍需保留（兼容框架设计），实际不影响数字输出行为
5. **平台下发控制**：平台可通过 MQTT 下发 `[{"id":"relay1","value":"1"}]` 控制输出状态
