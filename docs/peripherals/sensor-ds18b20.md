# DS18B20 数字温度传感器

## 功能说明

DS18B20 是一款单总线（OneWire）数字温度传感器，测量范围 -55°C ~ +125°C，精度 ±0.5°C。支持多个传感器挂载在同一总线上。

## 支持的外设类型

| 类型 | type值 | 传感器分类 |
|------|--------|-----------|
| SENSOR | 38 | SENSOR_DS18B20 (5) |

## 硬件接线

| DS18B20 引脚 | 连接 | 说明 |
|-------------|------|------|
| VCC | 3.3V / 5V | 电源 |
| GND | GND | 地 |
| DQ | GPIO + 4.7KΩ上拉至VCC | 数据线 |

> 数据线必须外接 4.7KΩ 上拉电阻到 VCC，否则通信不稳定。

## 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加DS18B20外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `ds18b20_1` | 唯一标识符 |
   | **名称** | `水温传感器` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | DS18B20驱动 |
   | **引脚配置** | `4` | DQ数据引脚 |
   | **传感器类别** | `DS18B20` | 温度传感器 |
   | **采集间隔** | `2000` | 2秒（≥750ms） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 等待2秒后查看温度数据

> ⚠️ **重要**：数据线必须外接4.7KΩ上拉电阻到VCC

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### 单个 DS18B20

```json
{
  "id": "ds18b20_1",
  "name": "水温传感器",
  "type": 38,
  "enabled": false,
  "pins": [4],
  "params": {
    "sensorType": 5,
    "sampleInterval": 2000
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| sensorType | 传感器分类：5 = DS18B20 |
| sampleInterval | 采样间隔（ms），最小 750ms（转换时间） |

## 与外设执行联动

### Web界面配置步骤

**创建定时采集规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**60** 秒
4. 添加动作：
   - 动作类型：**传感器读取**
   - 目标外设：**ds18b20_1**
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建温度超限报警规则**

1. 创建新规则
2. 配置平台触发器：
   - 触发类型：**平台触发**
   - 数据源：**ds18b20_1**
   - 比较操作：**大于**
   - 比较值：**60**
3. 添加动作：
   - 动作类型：**高电平**
   - 目标外设：**buzzer**（蜂鸣器）
4. 点击 **保存**

---

### JSON配置示例

### 定时采集温度

```json
{
  "id": "exec_ds18b20_read",
  "name": "定时采集水温",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 0,
      "intervalSec": 60,
      "timePoint": "",
      "eventId": "",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "ds18b20_1",
      "actionType": 19,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

### 温度超限报警

```json
{
  "id": "exec_ds18b20_alarm",
  "name": "水温超限报警",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 0,
      "triggerPeriphId": "ds18b20_1",
      "operatorType": 2,
      "compareValue": "60",
      "timerMode": 0,
      "intervalSec": 60,
      "timePoint": "",
      "eventId": "",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "buzzer",
      "actionType": 0,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

## 与 DHT 传感器对比

| 特性 | DS18B20 | DHT11/DHT22 |
|------|---------|-------------|
| 测量参数 | 仅温度 | 温度+湿度 |
| 精度 | ±0.5°C | ±2°C / ±0.5°C |
| 范围 | -55~125°C | 0~50°C / -40~80°C |
| 接口 | OneWire | 单总线(自定义协议) |
| 多传感器 | 支持一线挂多个 | 每个需独立引脚 |
| 防水 | 有防水封装 | 不防水 |

## 注意事项

1. **上拉电阻**：4.7KΩ 上拉电阻必不可少，长线传输建议使用更小的上拉（如 2.2KΩ）
2. **转换时间**：12位精度下转换时间约 750ms，采样间隔不应小于此值
3. **供电模式**：推荐外部供电模式（VCC接3.3V/5V），避免使用寄生供电
4. **总线长度**：标准布线可达 100m，但需注意信号质量
5. **多传感器**：同一引脚可挂多个 DS18B20，通过 ROM 地址区分
