# DHT11/DHT22 温湿度传感器配置指南

## 1. 功能说明

DHT11 和 DHT22(AM2302) 是常用的数字温湿度传感器，通过单总线协议与 ESP32 通信。FastBee 通过 `SensorDriver` 驱动实现自动初始化、缓存读取和异步采集。

### 传感器对比

| 参数 | DHT11 | DHT22/AM2302 |
|------|-------|-------------|
| 温度范围 | 0~50°C | -40~80°C |
| 温度精度 | ±2°C | ±0.5°C |
| 湿度范围 | 20~80% RH | 0~100% RH |
| 湿度精度 | ±5% | ±2~5% |
| 采样周期 | ≥1s | ≥2s |
| 工作电压 | 3.3~5.5V | 3.3~5.5V |

### 工作原理
- 单总线数字信号输出，无需 ADC
- 首次读取时自动初始化（惰性初始化）
- 内置 2 秒最小读取间隔缓存，避免频繁读取导致数据错误
- 读取失败时 10 秒内返回旧缓存值（优雅降级）

## 2. 接线说明

```
DHT11/DHT22 模块        ESP32
┌─────────────┐
│  VCC (1)    │───────── 3.3V 或 5V
│  DATA (2)   │───────── GPIO (如 GPIO4)
│  GND (3/4)  │───────── GND
└─────────────┘

注意：部分模块已内置 10K 上拉电阻；
      若使用裸传感器，需在 DATA 和 VCC 之间接 4.7K~10K 上拉电阻。
```

**推荐引脚**（ESP32）：GPIO4, GPIO5, GPIO13, GPIO14, GPIO15, GPIO25, GPIO26, GPIO27

## 3. 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加DHT传感器外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `dht11_01` 或 `dht22_01` | 唯一标识符 |
   | **名称** | `DHT11温湿度` 或 `DHT22温湿度` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | DHT驱动 |
   | **引脚配置** | `4` | DATA引脚 |
   | **传感器类别** | `DHT` | DHT系列 |
   | **传感器型号** | `DHT11` 或 `DHT22` | 根据实际型号 |
   | **采集间隔** | `5000` | 5秒（≥2000ms） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 等待5秒后查看温度和湿度数据

> 💡 **提示**：DHT22精度更高（±0.5°C），推荐用于精密测量

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

DHT 传感器在外设配置中使用 `SENSOR`(类型ID=38) 类型，通过 `params.sensor.sensorType` 区分 DHT11(3) 和 DHT22(4)。

### DHT11 配置示例

```json
{
  "id": "dht11_01",
  "name": "DHT11温湿度",
  "type": 38,
  "enabled": false,
  "pinCount": 1,
  "pins": [4, 255, 255, 255, 255, 255, 255, 255],
  "params": {
    "sensor": {
      "sensorType": 3,
      "sampleInterval": 5000
    }
  }
}
```

### DHT22 配置示例

```json
{
  "id": "dht22_01",
  "name": "DHT22温湿度",
  "type": 38,
  "enabled": false,
  "pinCount": 1,
  "pins": [4, 255, 255, 255, 255, 255, 255, 255],
  "params": {
    "sensor": {
      "sensorType": 4,
      "sampleInterval": 5000
    }
  }
}
```

### 字段说明

| 字段 | 含义 | 取值 |
|------|------|------|
| type | 外设类型 | 38 (SENSOR) |
| pins[0] | 数据引脚 | 有效 GPIO 号 |
| params.sensor.sensorType | 传感器类别 | 3=DHT11, 4=DHT22 |
| params.sensor.sampleInterval | 采样间隔(ms) | ≥2000 |

## 4. 外设执行联动

DHT 传感器的数据通过外设执行规则的 `ACTION_SENSOR_READ`(actionType=19) 动作进行采集，采集后的数据缓存为数据源，可被其他规则作为事件触发源使用。

### Web界面配置步骤

**创建定时采集规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**30** 秒
4. 添加动作：
   - 动作1：读取温度
     - 动作类型：**传感器读取**
     - 目标外设：**dht11_01**
     - 数据字段：**temperature**
   - 动作2：读取湿度
     - 动作类型：**传感器读取**
     - 目标外设：**dht11_01**
     - 数据字段：**humidity**
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建温度超限报警规则**

1. 创建新规则
2. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件ID：**ds:dht11_01_temperature**
   - 比较操作：**大于**
   - 比较值：**35**
3. 添加动作：
   - 动作类型：**高电平**
   - 目标外设：**relay_01**（风扇继电器）
4. 点击 **保存**

> 💡 **提示**：事件ID格式为 `ds:{外设ID}_{字段名}`

---

### JSON配置示例

### 传感器采集规则配置

```json
{
  "id": "exec_dht_read",
  "name": "DHT11定时采集",
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
      "targetPeriphId": "dht11_01",
      "actionType": 19,
      "actionValue": "temperature"
    },
    {
      "targetPeriphId": "dht11_01",
      "actionType": 19,
      "actionValue": "humidity"
    }
  ],
  "reportAfterExec": true
}
```

### 温度超限报警规则

```json
{
  "id": "exec_dht_alarm",
  "name": "温度超限报警",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:dht11_01_temperature",
      "operatorType": 2,
      "compareValue": "35"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "relay_01",
      "actionType": 0,
      "actionValue": ""
    }
  ],
  "reportAfterExec": true
}
```

## 5. 注意事项

1. **最小读取间隔**：DHT11/DHT22 硬件限制最小 2 秒，配置 sampleInterval 应 ≥2000ms
2. **引脚选择**：避免使用 GPIO6-11（Flash引脚）、GPIO34-39（仅输入，无内部上拉）
3. **上拉电阻**：裸传感器需外接 4.7K~10K 上拉电阻，模块版一般已内置
4. **数据线长度**：建议 ≤20m，超过时增加上拉电阻值或使用屏蔽线
5. **首次读取延迟**：传感器上电后需 1~2 秒稳定期，首次读取可能返回 NAN
6. **Flash占用**：DHT库约 3KB，通过 `FASTBEE_ENABLE_SENSOR_DRIVER=1` 编译开关控制

## 6. 常见问题

**Q: 读取始终返回 NAN？**
- 检查接线是否正确（VCC/GND/DATA）
- 确认上拉电阻是否存在
- 确认引脚号配置正确
- 传感器上电后等待 2 秒再首次读取

**Q: 数据跳变很大？**
- DHT11 精度为 ±2°C，正常波动
- 建议使用 DHT22 获得更高精度
- 可在外设执行中配置多次采样取平均

**Q: 多个 DHT 传感器如何配置？**
- 每个传感器使用不同引脚，配置不同 id（如 dht11_01、dht11_02）
- SensorDriver 内部按引脚缓存实例，互不干扰
