# SHT31 温湿度传感器

SHT31 是常用 I2C 温湿度传感器，精度和稳定性优于 DHT11，适合环境监测、温控和恒湿场景。当前驱动为轻量实现，不依赖额外第三方库，`esp32`、`esp32c3`、`esp32s3` 和 `esp32s3-full` 均可使用。

## 接线

| SHT31 | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

默认地址为 `0x44`，部分模块可切换到 `0x45`。

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。SHT31 保存前重点核对 I2C 地址、采样间隔和温湿度字段。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加SHT31外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `sht31_i2c` | 唯一标识符 |
   | **名称** | `SHT31温湿度` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | I2C传感器 |
   | **SDA引脚** | `21` | I2C数据引脚 |
   | **SCL引脚** | `22` | I2C时钟引脚 |
   | **I2C地址** | `0x44` | 0x44或0x45 |
   | **采集间隔** | `5000` | 5秒 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 等待5秒后查看温度和湿度数据

> 💡 **提示**：SHT31精度和稳定性优于DHT11，所有ESP32型号均支持

---

### 方式2：JSON配置文件导入

```json
{
  "id": "sht31_i2c",
  "name": "SHT31温湿度",
  "type": 2,
  "enabled": false,
  "pinCount": 2,
  "pins": [21, 22, 255, 255, 255, 255, 255, 255],
  "params": {
    "frequency": 100000,
    "address": 0,
    "isMaster": true
  }
}
```

## 外设执行联动

### Web界面配置步骤

**创建温度采集与控制规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**30** 秒
4. 添加动作：
   - 动作1：读取温度
     - 动作类型：**传感器读取**
     - 目标外设：**sht31_i2c**
     - 数据字段：**temperature**
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建温度过高打开继电器规则**

1. 创建新规则
2. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件ID：**ds:sht31_i2c_temperature**
   - 比较操作：**大于**
   - 比较值：**35**（℃）
3. 添加动作：
   - 动作类型：**高电平**
   - 目标外设：**relay_01**（风扇继电器）
4. 点击 **保存**

> 💡 **提示**：SHT31适用于精密温控、恒湿场景

---

### JSON配置示例

```json
{
  "id": "exec_sht31_temperature_read",
  "name": "SHT31温度采集",
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
      "targetPeriphId": "sht31_i2c",
      "actionType": 19,
      "actionValue": "{\"periphId\":\"sht31_i2c\",\"sensorCategory\":\"SHT31\",\"dataField\":\"temperature\",\"sensorLabel\":\"温度\",\"unit\":\"℃\",\"decimalPlaces\":1,\"driverParams\":{\"addr\":\"0x44\",\"sda\":21,\"scl\":22}}",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "reportAfterExec": true
}
```

## 温度过高打开继电器

```json
{
  "id": "exec_sht31_hot_relay",
  "name": "SHT31高温打开继电器",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:sht31_i2c_temperature",
      "operatorType": 2,
      "compareValue": "35"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "relay_01",
      "actionType": 0,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "reportAfterExec": true
}
```

## 可采集字段

| dataField | 含义 | 单位 |
|---|---|---|
| `temperature` | 温度 | ℃ |
| `humidity` | 相对湿度 | % |
