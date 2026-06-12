# BH1750 光照传感器

BH1750 是常用 I2C 光照强度传感器，直接输出照度值，适合自动照明、农业补光、遮阳控制和环境记录。当前驱动为轻量实现，不依赖额外第三方库，精简 ESP32 固件可用。

## 接线

| BH1750 | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

默认地址为 `0x23`，ADDR 引脚改变时常见地址为 `0x5C`。

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。BH1750 保存前重点核对 I2C 地址、采集模式和光照量程。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加BH1750外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `bh1750_i2c` | 唯一标识符 |
   | **名称** | `BH1750光照` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | I2C传感器 |
   | **SDA引脚** | `21` | I2C数据引脚 |
   | **SCL引脚** | `22` | I2C时钟引脚 |
   | **I2C地址** | `0x23` | 0x23或0x5C |
   | **采集间隔** | `5000` | 5秒 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 等待5秒后查看光照强度数据（lx）

> 💡 **提示**：BH1750直接输出照度值，无需ADC转换

---

### 方式2：JSON配置文件导入

```json
{
  "id": "bh1750_i2c",
  "name": "BH1750光照",
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

**创建光照采集与自动开灯规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**20** 秒
4. 添加动作：
   - 动作1：读取光照强度
     - 动作类型：**传感器读取**
     - 目标外设：**bh1750_i2c**
     - 数据字段：**illuminance**
5. 开启 **执行后上报数据**
6. 点击 **保存**

**创建光照过低开灯规则**

1. 创建新规则
2. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件ID：**ds:bh1750_i2c_illuminance**
   - 比较操作：**小于**
   - 比较值：**80**（lx）
3. 添加动作：
   - 动作类型：**高电平**
   - 目标外设：**relay_light**（照明继电器）
4. 点击 **保存**

> 💡 **提示**：事件ID格式为 `ds:{外设ID}_{字段名}`

---

### JSON配置示例

```json
{
  "id": "exec_bh1750_read",
  "name": "BH1750光照采集",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 1,
      "timerMode": 0,
      "intervalSec": 20
    }
  ],
  "actions": [
    {
      "targetPeriphId": "bh1750_i2c",
      "actionType": 19,
      "actionValue": "{\"periphId\":\"bh1750_i2c\",\"sensorCategory\":\"BH1750\",\"dataField\":\"illuminance\",\"sensorLabel\":\"光照\",\"unit\":\"lx\",\"decimalPlaces\":0,\"driverParams\":{\"addr\":\"0x23\",\"sda\":21,\"scl\":22}}",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "reportAfterExec": true
}
```

## 光照过低开灯

```json
{
  "id": "exec_bh1750_light_on",
  "name": "低照度开灯",
  "enabled": false,
  "triggers": [
    {
      "triggerType": 4,
      "eventId": "ds:bh1750_i2c_illuminance",
      "operatorType": 3,
      "compareValue": "80"
    }
  ],
  "actions": [
    {
      "targetPeriphId": "relay_light",
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
| `illuminance` | 光照强度 | lx |
