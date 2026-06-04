# 传感器读取动作

## 动作类型

| 动作 | actionType | 说明 |
|------|------------|------|
| ACTION_SENSOR_READ | 19 | 读取传感器数据 |

## 概述

传感器读取动作触发指定传感器外设进行一次数据采集。采集结果存入传感器缓存，并在规则 `reportAfterExec: true` 时通过 MQTT 上报。

## 支持的传感器类型

| SensorCategory | 值 | 读取方式 | 输出 |
|----------------|-----|----------|------|
| SENSOR_ANALOG | 0 | ADC 读取 | 原始 ADC 值 |
| SENSOR_DIGITAL | 1 | GPIO 读取 | 0/1 |
| SENSOR_DHT11 | 3 | DHT 协议 | temperature, humidity |
| SENSOR_DHT22 | 4 | DHT 协议 | temperature, humidity |
| SENSOR_DS18B20 | 5 | OneWire | temperature |
| SENSOR_ULTRASONIC | 6 | Trig+Echo | distance_cm |
| SENSOR_CURRENT | 7 | ADC+校准 | current_A |
| SENSOR_VOLTAGE | 8 | ADC+校准 | voltage_V |
| SHT31 | driver registry | I2C | temperature, humidity |
| AHT20 | driver registry | I2C | temperature, humidity |
| BH1750 | driver registry | I2C | illuminance |
| BMP280 | driver registry | I2C(S3-full) | temperature, pressure, altitude |
| MPU6050 | driver registry | I2C(S3-full) | accelX/Y/Z, temperature, gyroX/Y/Z |

## 参数说明

| 字段 | 说明 |
|------|------|
| targetPeriphId | 目标传感器外设 ID |
| actionType | 19 |
| actionValue | JSON 字符串，描述 `periphId`、`sensorCategory`、`dataField` 和校准/驱动参数 |

## actionValue 推荐格式

```json
{
  "periphId": "sht31_i2c",
  "sensorCategory": "SHT31",
  "dataField": "temperature",
  "sensorLabel": "温度",
  "unit": "℃",
  "decimalPlaces": 1,
  "driverParams": {
    "addr": "0x44",
    "sda": 21,
    "scl": 22
  }
}
```

ADC 电流/电压类传感器可以在同一 JSON 中增加校准参数：

```json
{
  "periphId": "current_01",
  "sensorCategory": "current",
  "dataField": "current",
  "sensorLabel": "电流",
  "unit": "A",
  "decimalPlaces": 3,
  "sensitivity": 0.100,
  "zeroOffset": 1.65,
  "vRef": 3.3,
  "adcMax": 4095
}
```

## 配置示例

### 方式1：Web界面配置（推荐）

#### 示例1：读取 DHT11 温湿度

**场景**：读取DHT11传感器的温度和湿度数据

**配置步骤**：

1. 在外设执行管理页面编辑规则
2. 点击 **添加动作** 按钮
3. 填写动作配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **动作类型** | 选择 **传感器读取** | 读取传感器 |
   | **目标外设** | 选择 `dht1` | DHT11外设ID |
   | **数据字段** | `temperature` 或 `humidity` | 读取的字段 |
   | **显示名称** | `温度` 或 `湿度` | 上报时的名称 |
   | **单位** | `℃` 或 `%` | 数据单位 |
   | **小数位数** | `1` | 保留1位小数 |

4. 点击 **保存** 按钮

> 💡 **提示**：DHT11会同时读取温度和湿度两个字段

---

#### 示例2：读取超声波距离

**场景**：读取HC-SR04超声波传感器的距离数据

**配置步骤**：

1. 编辑规则，添加动作
2. 填写：
   - **动作类型**：选择 **传感器读取**
   - **目标外设**：选择 `ultrasonic1`
   - **数据字段**：`distance`
   - **显示名称**：`距离`
   - **单位**：`cm`
   - **小数位数**：`1`

3. 点击 **保存**

---

#### 示例3：读取电流传感器（带校准）

**场景**：读取ACS712电流传感器数据

**配置步骤**：

1. 编辑规则，添加动作
2. 填写：
   - **动作类型**：选择 **传感器读取**
   - **目标外设**：选择 `current_sensor`
   - **数据字段**：`current`
   - **显示名称**：`电流`
   - **单位**：`A`
   - **小数位数**：`3`
   - **灵敏度**：`0.100`（根据模块填写）
   - **零点偏移**：`1.65`（VCC/2）
   - **参考电压**：`3.3`
   - **ADC最大值**：`4095`（ESP32为12位）

3. 点击 **保存**

> 💡 **提示**：电流/电压传感器需要校准参数才能准确转换

---

### 方式2：JSON配置文件导入

## 数据上报格式

传感器读取结果通过 MQTT 上报的格式：

```json
[
  {"id": "dht1.temperature", "value": "25.6"},
  {"id": "dht1.humidity", "value": "65.2"}
]
```

## 传感器缓存

读取的数据会自动存入传感器缓存（SensorReadCache），可被 OLED 显示模板 `${id.field}` 引用。

## 完整规则示例

### 定时采集并上报

```json
{
  "id": "exec_sensor_poll",
  "name": "定时采集传感器",
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
      "targetPeriphId": "dht1",
      "actionType": 19,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    },
    {
      "targetPeriphId": "ultrasonic1",
      "actionType": 19,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 200,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

## 注意事项

1. **采样间隔**：传感器有最小采样间隔保护（DHT: 2000ms, 超声波: 100ms, ADC: 200ms）
2. **缓存机制**：间隔内重复读取返回缓存值，不会实际触发硬件读取
3. **错误处理**：读取失败不会中断规则其他动作的执行
4. **上报筛选**：仅 SENSOR_READ 和 MODBUS_POLL 的结果会被精准上报
