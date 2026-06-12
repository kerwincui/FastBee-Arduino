# HC-SR04 超声波测距传感器配置指南

## 1. 功能说明

HC-SR04 是一款常用的超声波测距模块，通过发射 40KHz 超声波脉冲并接收回波，测量目标距离。FastBee 通过 `SensorDriver::readUltrasonic()` 方法实现自动初始化、脉冲收发和距离计算。

### 技术参数

| 参数 | 值 |
|------|------|
| 工作电压 | 5V (部分模块支持3.3V) |
| 工作频率 | 40KHz |
| 测距范围 | 2cm ~ 400cm |
| 测量精度 | ±3mm |
| 触发信号 | 10μs TTL 脉冲 |
| 回波信号 | 高电平时间正比于距离 |
| 最小读取间隔 | 100ms |

### 工作原理
1. Trig 引脚发送 ≥10μs 高电平触发脉冲
2. 模块自动发射 8 个 40KHz 超声波脉冲
3. Echo 引脚输出高电平，持续时间 = 声波往返时间
4. 距离计算：`distance(cm) = duration(μs) × 0.034 / 2`

![HC-SR04 超声波测距几何](../images/ultrasonic-ranging-geometry.svg)

如果读数始终超时或跳变，先按图检查物理条件：Trig/Echo 是否接反，Echo 电平是否已转换到 3.3V，目标距离是否超过 2cm 盲区，反射面是否足够平整。

### 特性
- 100ms 最小读取间隔缓存
- 超时保护（30ms，约 510cm）
- 范围校验（2~400cm 有效）
- 读取失败时 10 秒内返回旧缓存值

## 2. 接线说明

```
HC-SR04 模块              ESP32
┌─────────────┐
│  VCC        │───────── 5V
│  Trig       │───────── GPIO4 (任意输出引脚)
│  Echo       │───────── GPIO27 (任意输入引脚)
│  GND        │───────── GND
└─────────────┘

注意：
- HC-SR04 需 5V 供电，Echo 输出为 5V 电平
- ESP32 GPIO 容忍 3.3V，若使用 5V 版模块需在 Echo 线加分压器：
  Echo ── 1KΩ ──┬── 2KΩ ── GND
                 │
                 └── ESP32 GPIO (约 3.3V)
- 部分新版 HC-SR04 模块（如 HC-SR04P）支持 3.3V 供电和 3.3V 逻辑电平
- Trig 和 Echo 不能使用同一引脚
```

**推荐引脚**：
- Trig: GPIO4, GPIO5, GPIO16, GPIO17（输出引脚）
- Echo: GPIO27, GPIO26, GPIO25, GPIO33（输入引脚）

## 3. 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。HC-SR04 保存前重点核对 Trig/Echo 引脚，Echo 回传电平需要符合 ESP32 输入电压范围。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加超声波传感器外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `ultrasonic_01` | 唯一标识符 |
   | **名称** | `超声波测距` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | 超声波驱动 |
   | **Trig引脚** | `4` | 触发引脚（输出） |
   | **Echo引脚** | `27` | 回波引脚（输入） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 查看实时距离数据（cm）

> ⚠️ **重要**：5V模块的Echo引脚需加分压器（5V→3.3V）

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

超声波传感器使用 `SENSOR`(类型ID=38) 类型，pinCount=2，pins[0]=Trig，pins[1]=Echo。

```json
{
  "id": "ultrasonic_01",
  "name": "超声波测距",
  "type": 38,
  "enabled": false,
  "pinCount": 2,
  "pins": [4, 27, 255, 255, 255, 255, 255, 255],
  "params": {}
}
```

### 字段说明

| 字段 | 含义 | 取值 |
|------|------|------|
| type | 外设类型 | 38 (SENSOR) |
| pins[0] | Trig 触发引脚 | 有效输出 GPIO |
| pins[1] | Echo 回波引脚 | 有效输入 GPIO |
| params | 当前无需专用参数 | 采样间隔由外设执行规则控制，建议 ≥100ms |

## 4. 外设执行联动

### Web界面配置步骤

**创建定时采集规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**5** 秒
4. 添加动作：
   - 动作类型：**传感器读取**
   - 目标外设：**ultrasonic_01**
   - 数据字段：**distance**（距离）
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建距离过近报警规则**

1. 创建新规则
2. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件ID：**ds:ultrasonic_01_distance**
   - 比较操作：**小于**
   - 比较值：**20**（cm）
3. 添加动作：
   - 动作1：蜂鸣器报警
     - 动作类型：**高电平**
     - 目标外设：**buzzer_01**
   - 动作2：触发设备事件
     - 动作类型：**触发事件**
     - 事件ID：**obstacle_detected**
4. 点击 **保存**

> 💡 **提示**：事件ID格式为 `ds:{外设ID}_{字段名}`

---

### JSON配置示例

### 距离定时采集规则

```json
{
  "id": "exec_ultrasonic_read",
  "name": "超声波定时采集",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 1,
      "timerMode": 0,
      "intervalSec": 5
    }
  ],
  "actions": [
    {
      "targetPeriphId": "ultrasonic_01",
      "actionType": 19,
      "actionValue": "{\"periphId\":\"ultrasonic_01\",\"sensorCategory\":\"ultrasonic\",\"dataField\":\"distance\",\"sensorLabel\":\"距离\",\"unit\":\"cm\",\"decimalPlaces\":1}"
    }
  ],
  "reportAfterExec": true
}
```

### 距离过近报警规则

```json
{
  "id": "exec_distance_alarm",
  "name": "距离过近报警",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:ultrasonic_01_distance",
      "operatorType": 3,
      "compareValue": "20"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "buzzer_01",
      "actionType": 0,
      "actionValue": ""
    },
    {
      "targetPeriphId": "",
      "actionType": 21,
      "actionValue": "obstacle_detected"
    }
  ],
  "reportAfterExec": true
}
```

### 水位监测场景（安装在容器顶部朝下测量）

```json
{
  "id": "exec_water_level",
  "name": "水位超限报警",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:ultrasonic_01_distance",
      "operatorType": 3,
      "compareValue": "10"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "pump_relay",
      "actionType": 0,
      "actionValue": ""
    }
  ],
  "reportAfterExec": true
}
```

## 5. 应用场景

| 场景 | 安装方式 | 判断逻辑 |
|------|---------|---------|
| 障碍物检测 | 水平朝前安装 | 距离 < 阈值 → 报警 |
| 水位监测 | 垂直朝下安装在容器顶部 | 距离 < 阈值 → 水位高 |
| 料位检测 | 垂直朝下安装在料仓顶部 | 距离 > 阈值 → 料位低 |
| 车位检测 | 垂直朝下安装在车位上方 | 距离 < 阈值 → 有车 |

## 6. 注意事项

1. **测量盲区**：HC-SR04 在 2cm 以内无法测量，请确保目标距离 > 2cm
2. **反射面要求**：目标面应较平整，粗糙面或角度过大会导致回波丢失
3. **温度补偿**：声速随温度变化（20°C 约 343m/s，0°C 约 331m/s），精确测量需补偿
4. **电平适配**：5V 版 Echo 输出 5V，需分压或选用 3.3V 版模块
5. **串扰避免**：多个超声波模块需交替触发，避免互相干扰（间隔 ≥60ms）
6. **软物体**：棉花、布料等软物体吸声能力强，可能无法检测

水位、料位等垂直安装场景建议预留机械余量：传感器到满位液面的距离不要落入 2cm 盲区，容器边缘和液面泡沫会造成回波干扰，必要时使用多次采样中位数或加长采样周期。

## 7. 常见问题

**Q: 读取始终返回 NAN（超时）？**
- 检查 VCC 是否为 5V（3.3V 供电可能驱动力不足）
- 确认 Trig/Echo 引脚接线未反接
- 确认目标物在 2~400cm 有效范围内
- Echo 引脚需要接分压器（5V→3.3V）

**Q: 读数跳动较大？**
- 超声波受环境影响大（风、温度变化、反射面角度）
- 建议软件取多次中值滤波
- 减小采样间隔可提高数据密度

**Q: 如何同时使用多个超声波模块？**
- 不同模块使用不同的 Trig/Echo 引脚对
- 配置不同 id（如 ultrasonic_01、ultrasonic_02）
- 建议采集间隔错开，避免超声波互相干扰
