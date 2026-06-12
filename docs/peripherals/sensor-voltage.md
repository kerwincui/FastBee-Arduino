# 电压型传感器配置指南（分压器 ADC）

## 1. 功能说明

电压型传感器通过电阻分压网络将高于 ESP32 ADC 量程（3.3V）的被测电压降低到安全范围，再由 ADC 采集并通过分压比还原实际电压值。

### 适用场景
- 电池电压监测（3.7V/7.4V/12V/24V 锂电池）
- 太阳能板电压检测
- 电源供电电压监控
- 工业传感器 0~5V/0~10V 信号采集

### 工作原理
```
Vin ─── R1 ──┬── R2 ─── GND
              │
              └── ADC Pin (Vout)

Vout = Vin × R2 / (R1 + R2)
Vin = Vout × (R1 + R2) / R2 = Vout × ratio
ratio = (R1 + R2) / R2
```

### 常用分压比

| R1 | R2 | ratio | 最大可测电压 (ADC满量程3.3V) |
|----|----|-------|---------------------------|
| 30KΩ | 7.5KΩ | 5.0 | 16.5V |
| 100KΩ | 10KΩ | 11.0 | 36.3V |
| 47KΩ | 10KΩ | 5.7 | 18.8V |
| 10KΩ | 10KΩ | 2.0 | 6.6V |

### 特性
- 10 次多采样平均，减少 ADC 噪声
- 200ms 最小读取间隔缓存
- 惰性初始化，首次调用自动配置 ADC（12位/11dB衰减）
- 支持自定义分压比和参考电压参数

![GPIO、ADC、PWM 接线校验图](../images/gpio-adc-pwm-wiring-check.svg)

电压采集属于 ADC 输入，接线前必须确认分压后电压不超过 ESP32 ADC 量程；调试时同时记录原始 ADC 值和换算后的实际电压。

## 2. 接线说明

```
被测电压源 (Vin)
    │
    ├── R1 (如 30KΩ)
    │
    ├──────────────── GPIO34 (ESP32 ADC引脚)
    │
    ├── R2 (如 7.5KΩ)
    │
    └── GND ─────── ESP32 GND

注意：
- R1 + R2 组成分压器，Vout = Vin × R2/(R1+R2)
- 确保 Vout ≤ 3.3V，否则会损坏 ESP32
- 推荐在 ADC 引脚和 GND 之间并联 100nF 电容滤波
- 推荐使用 GPIO34/35/36/39（仅输入，ADC精度更好）
```

### 商用电压检测模块

常见的 DC 0-25V 电压检测模块已内置分压电路（通常 ratio=5.0）：
```
电压检测模块             ESP32
┌─────────────┐
│  VCC (+)    │───────── 被测电压正极 (≤25V)
│  GND (-)    │───────── 被测电压负极 + ESP32 GND
│  S (Signal) │───────── GPIO34 (ADC引脚)
└─────────────┘
```

## 3. 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。电压传感器保存前重点核对 ADC 引脚、分压比例和输入电压上限。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加电压传感器外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `voltage_01` 或 `battery_12v` | 唯一标识符 |
   | **名称** | `电压检测(0-25V)` 或 `12V电池电压` | 显示名称 |
   | **外设类型** | **GPIO模拟输入** (type: 15) | ADC采集 |
   | **引脚配置** | `34` 或 `35` | ADC引脚（推荐34-39） |
   | **衰减系数** | `3` | 11dB(0-3.3V) |
   | **分辨率** | `12` | 12位(0-4095) |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 查看实时电压数据

> 💡 **提示**：分压比根据实际电阻计算，商用模块通常ratio=5.0

---

### 方式2：JSON配置文件导入

```json
{
  "id": "voltage_01",
  "name": "电压检测(0-25V)",
  "type": 15,
  "enabled": false,
  "pinCount": 1,
  "pins": [34, 255, 255, 255, 255, 255, 255, 255],
  "params": {
    "attenuation": 3,
    "resolution": 12,
    "sampleRate": 5
  }
}
```

### 12V 电池监测配置

```json
{
  "id": "battery_12v",
  "name": "12V电池电压",
  "type": 15,
  "enabled": false,
  "pinCount": 1,
  "pins": [35, 255, 255, 255, 255, 255, 255, 255],
  "params": {
    "attenuation": 3,
    "resolution": 12,
    "sampleRate": 5
  }
}
```

## 4. 外设执行联动

### Web界面配置步骤

**创建电压采集规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**30** 秒
4. 添加动作：
   - 动作类型：**传感器读取**
   - 目标外设：**voltage_01**
   - 数据字段：**voltage**
   - 分压比：**5.0**（根据实际调整）
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建电池欠压报警规则**

1. 创建新规则
2. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件ID：**ds:battery_12v_voltage**
   - 比较操作：**小于**
   - 比较值：**10.5**（V）
3. 添加动作：
   - 动作1：触发低电量事件
     - 动作类型：**触发事件**
     - 事件ID：**low_battery**
   - 动作2：关闭负载
     - 动作类型：**低电平**
     - 目标外设：**load_relay**
4. 点击 **保存**

> 💡 **提示**：12V锂电池欠压保护阈值通常为10.5V

---

### JSON配置示例

### 电压定时采集规则

```json
{
  "id": "exec_voltage_read",
  "name": "电压定时采集",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 1,
      "timerMode": 0,
      "intervalSec": 30
    }
  ],
  "actions": [
    {
      "targetPeriphId": "voltage_01",
      "actionType": 19,
      "actionValue": "{\"periphId\":\"voltage_01\",\"sensorCategory\":\"voltage\",\"dataField\":\"voltage\",\"sensorLabel\":\"电压\",\"unit\":\"V\",\"decimalPlaces\":2,\"ratio\":5.0,\"vRef\":3.3,\"adcMax\":4095}"
    }
  ],
  "reportAfterExec": true
}
```

### actionValue 格式

电压传感器的 `actionValue` 使用 JSON 字符串传递读取目标和校准参数：

```json
{
  "periphId": "voltage_01",
  "sensorCategory": "voltage",
  "dataField": "voltage",
  "sensorLabel": "电压",
  "unit": "V",
  "decimalPlaces": 2,
  "ratio": 5.0,
  "vRef": 3.3,
  "adcMax": 4095
}
```

| 参数 | 说明 | 示例 |
|------|------|------|
| ratio | 分压比 (R1+R2)/R2 | 5.0 (30K/7.5K) |
| vRef | ADC参考电压 (V) | 3.3 |
| adcMax | ADC最大值 | 4095 |

### 低电压报警规则（电池欠压保护）

```json
{
  "id": "exec_low_voltage",
  "name": "电池欠压报警",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:battery_12v_voltage",
      "operatorType": 3,
      "compareValue": "10.5"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "",
      "actionType": 21,
      "actionValue": "low_battery"
    },
    {
      "targetPeriphId": "load_relay",
      "actionType": 1,
      "actionValue": ""
    }
  ],
  "reportAfterExec": true
}
```

## 5. 校准方法

### 方法一：已知分压电阻值
直接计算：`ratio = (R1 + R2) / R2`

### 方法二：实测校准
1. 接入已知电压（如用万用表测量）
2. 读取 ESP32 的 ADC 原始值
3. `实测Vout = ADC * 3.3 / 4095`
4. `ratio = Vin_known / 实测Vout`

### 精度优化建议
- 使用 1% 精度电阻（金属膜电阻）
- 在分压输出端并联 100nF 陶瓷电容
- ESP32 ADC 存在非线性，建议分段校准或使用查找表
- 多次采样平均（驱动已内置 10 次平均）

## 6. 注意事项

1. **过压保护**：确保分压后电压 ≤ 3.3V，建议留出 20% 余量
2. **输入阻抗**：R1+R2 总阻值建议 10K~100K，太小浪费电流，太大受 ADC 输入阻抗影响
3. **隔离安全**：高压检测（如市电）必须使用隔离型电压互感器，不可直接分压
4. **温度漂移**：金属膜电阻温度系数小（±50ppm/°C），碳膜电阻温度系数大
5. **引脚选择**：GPIO34-39 为仅输入引脚，ADC 特性最稳定
6. **接地共参考**：被测电压的 GND 必须与 ESP32 GND 连接

## 7. 常见问题

**Q: 读数偏差较大？**
- ESP32 ADC 非线性误差可达 ±3%，建议实测校准 ratio
- 检查电阻是否精确（使用万用表测量实际阻值）
- 确保 ADC 输入无浮空（始终有分压器连接到 GND）

**Q: 读数跳动明显？**
- 在分压输出端增加 100nF 滤波电容
- 增大采样次数（软件已内置 10 次平均）
- 检查电源是否稳定

**Q: 如何检测负电压？**
- ESP32 ADC 不支持负电压输入
- 需使用运算放大器将信号偏移到正电压范围
- 或使用差分 ADC 模块（如 ADS1115）

**Q: 0~10V 工业信号如何接入？**
- 使用 ratio=3.33（如 R1=23.3K, R2=10K）
- 确保最大输入 10V 时 Vout = 10/3.33 = 3.0V < 3.3V
