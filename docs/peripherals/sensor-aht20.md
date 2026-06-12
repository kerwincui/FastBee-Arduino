# AHT20 温湿度传感器

AHT20 是低成本 I2C 温湿度传感器，适合替代 DHT11/DHT22 做更稳定的室内环境采集。当前驱动为轻量实现，不依赖额外第三方库，精简 ESP32 固件可用。

## 接线

| AHT20 | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

默认地址为 `0x38`。

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。AHT20 保存前重点核对 I2C 总线、地址和采集间隔。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加AHT20外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `aht20_i2c` | 唯一标识符 |
   | **名称** | `AHT20温湿度` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | I2C传感器 |
   | **SDA引脚** | `21` | I2C数据引脚 |
   | **SCL引脚** | `22` | I2C时钟引脚 |
   | **I2C地址** | `0x38` | 默认地址 |
   | **采集间隔** | `5000` | 5秒 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 等待5秒后查看温度和湿度数据

> 💡 **提示**：AHT20是低成本替代DHT11/DHT22的I2C方案，精度更好

---

### 方式2：JSON配置文件导入

```json
{
  "id": "aht20_i2c",
  "name": "AHT20温湿度",
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

**创建湿度采集与控制规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**30** 秒
4. 添加动作：
   - 动作1：读取湿度
     - 动作类型：**传感器读取**
     - 目标外设：**aht20_i2c**
     - 数据字段：**humidity**
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建湿度过低打开加湿器规则**

1. 创建新规则
2. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件ID：**ds:aht20_i2c_humidity**
   - 比较操作：**小于**
   - 比较值：**40**（%）
3. 添加动作：
   - 动作类型：**高电平**
   - 目标外设：**relay_humidifier**（加湿器继电器）
4. 点击 **保存**

> 💡 **提示**：事件ID格式为 `ds:{外设ID}_{字段名}`

---

### JSON配置示例

```json
{
  "id": "exec_aht20_humidity_read",
  "name": "AHT20湿度采集",
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
      "targetPeriphId": "aht20_i2c",
      "actionType": 19,
      "actionValue": "{\"periphId\":\"aht20_i2c\",\"sensorCategory\":\"AHT20\",\"dataField\":\"humidity\",\"sensorLabel\":\"湿度\",\"unit\":\"%\",\"decimalPlaces\":1,\"driverParams\":{\"addr\":\"0x38\",\"sda\":21,\"scl\":22}}",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "reportAfterExec": true
}
```

## 湿度过低打开加湿器继电器

```json
{
  "id": "exec_aht20_dry_relay",
  "name": "AHT20低湿打开加湿器",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:aht20_i2c_humidity",
      "operatorType": 3,
      "compareValue": "40"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "relay_humidifier",
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
