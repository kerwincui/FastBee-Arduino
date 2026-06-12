# ADC 模拟输入

## 功能说明

ADC（模数转换器）用于采集模拟信号，如光照强度、烟雾浓度、火焰强度、土壤湿度等模拟传感器的输出电压。ESP32 内置 12 位 ADC，可将 0~3.3V 的模拟电压转换为 0~4095 的数字值。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| GPIO_ANALOG_INPUT | 15 | GPIO模拟输入（简单模式） |
| ADC | 26 | ADC专用模式（带衰减/分辨率参数） |

## ADC 参数说明

| 参数 | 说明 | 可选值 |
|------|------|--------|
| attenuation | 衰减系数 | 0=0dB(1.1V), 1=2.5dB(1.5V), 2=6dB(2.2V), 3=11dB(3.3V) |
| resolution | 分辨率（位） | 9, 10, 11, 12 |
| sampleRate | 采样率 | 0=默认 |

> 常用配置：`attenuation=3`（满量程3.3V）+ `resolution=12`（0~4095）

![GPIO、ADC、PWM 接线校验图](../images/gpio-adc-pwm-wiring-check.svg)

ADC 接入前先确认输入电压、衰减、分辨率和 WiFi 对 ADC2 的影响；现场排查时建议同时保存原始值、换算值和采样间隔。

## 可用引脚

- **ESP32 ADC1**: GPIO 32-39（推荐，WiFi开启时仍可用）
- **ESP32 ADC2**: GPIO 0, 2, 4, 12-15, 25-27（WiFi开启时不可用）
- **ESP32-C3**: GPIO 0-4（ADC1）
- **ESP32-S3**: GPIO 1-10（ADC1），GPIO 11-20（ADC2）

> **重要**：使用 WiFi 时只能使用 ADC1 通道的引脚！

## 典型传感器应用

| 传感器 | 输出范围 | 说明 |
|--------|----------|------|
| 光敏电阻 | 0~3.3V | 光照越强电压越低 |
| MQ-2 烟雾 | 0~3.3V | 浓度越高电压越高 |
| 火焰传感器 | 0~3.3V | 火焰越近电压越低 |
| 雨滴传感器 | 0~3.3V | 雨量越大电压越低 |
| 声音传感器 | 0~3.3V | 声音越大电压越高 |
| 土壤湿度 | 0~3.3V | 湿度越大电压越低 |

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。ADC 类外设保存前重点核对输入引脚、量程换算参数和是否已做分压保护。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加ADC模拟输入外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `light_sensor` 或 `smoke_sensor` | 唯一标识符 |
   | **名称** | `光照强度` 或 `MQ-2烟雾传感器` | 显示名称 |
   | **外设类型** | **GPIO模拟输入** (type: 15) 或 **ADC** (type: 26) | 简单模式或专用模式 |
   | **引脚配置** | `36` 或 `34` | 必须使用ADC1引脚 |
   | **衰减系数** | `3` | 3=11dB(0-3.3V满量程) |
   | **分辨率** | `12` | 12位=0-4095 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 查看实时ADC值

> ⚠️ **重要**：使用WiFi时必须使用ADC1引脚（GPIO 32-39）

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### 光敏传感器

```json
{
  "id": "light_sensor",
  "name": "光照强度",
  "type": 15,
  "enabled": false,
  "pins": [36],
  "params": {
    "initialState": 0,
    "pwmChannel": 0,
    "pwmFrequency": 1000,
    "pwmResolution": 8,
    "defaultDuty": 0
  }
}
```

### ADC 专用配置（烟雾传感器）

```json
{
  "id": "smoke_sensor",
  "name": "MQ-2烟雾传感器",
  "type": 26,
  "enabled": false,
  "pins": [34],
  "params": {
    "attenuation": 3,
    "resolution": 12,
    "sampleRate": 0
  }
}
```

### 多路 ADC 采集

```json
{
  "id": "adc_multi",
  "name": "环境监测ADC",
  "type": 26,
  "enabled": false,
  "pins": [36, 39, 34, 35],
  "params": {
    "attenuation": 3,
    "resolution": 12,
    "sampleRate": 0
  }
}
```

## 与外设执行联动

### 作为轮询触发数据源

ADC 值可被外设执行规则通过 `ACTION_SENSOR_READ(19)` 周期采集：

```json
{
  "targetPeriphId": "smoke_sensor",
  "actionType": 19,
  "actionValue": "",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

### 作为平台触发条件

采集的 ADC 值通过 MQTT 上报后，可用于平台触发条件判断：

```json
{
  "triggerType": 0,
  "triggerPeriphId": "smoke_sensor",
  "operatorType": 2,
  "compareValue": "2000"
}
```

> 含义：当烟雾传感器 ADC 值 > 2000 时触发规则

## 电压/电流传感器

对于需要将 ADC 值转换为实际物理量的场景，请参考：
- [电流型传感器](sensor-current.md) — ACS712 等电流测量
- [电压型传感器](sensor-voltage.md) — 分压器电压测量

## 注意事项

1. **WiFi 与 ADC2 冲突**：WiFi 开启后 ADC2 不可用，生产环境务必使用 ADC1 引脚
2. **ESP32 ADC 非线性**：ESP32 原生 ADC 在两端（接近 0V 和 3.3V）存在非线性，中间段精度较好
3. **参考电压**：ESP32 内部参考电压约 1.1V，通过衰减系数扩展量程
4. **多路采集**：同一 ADC 类型外设可配置多个引脚，系统按顺序轮询采集
5. **采样噪声**：建议对 ADC 读数进行多次采样取平均（系统对电流/电压传感器已内置 10 次平均）
