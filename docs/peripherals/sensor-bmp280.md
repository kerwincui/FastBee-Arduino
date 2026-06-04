# BMP280 气压传感器

## 功能说明

BMP280 是一款高精度气压/温度传感器，通过 I2C 总线通信，可测量大气压强、环境温度和估算海拔高度。适用于气象站、无人机高度计、室内导航等场景。

> **固件要求**：仅 ESP32-S3 full 固件支持（需启用 `FASTBEE_ENABLE_I2C_SENSORS` 编译开关）。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| SENSOR | 38 | I2C 传感器（BMP280） |

## 硬件接线

| BMP280 引脚 | ESP32-S3 GPIO | 说明 |
|------------|--------------|------|
| VCC | 3.3V | 电源 |
| GND | GND | 地 |
| SDA | GPIO 21 | I2C 数据线 |
| SCL | GPIO 22 | I2C 时钟线 |

> 大部分 BMP280 模块自带上拉电阻，无需外部上拉。

## 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加BMP280外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `bmp280_1` | 唯一标识符 |
   | **名称** | `BMP280气压传感器` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | I2C传感器 |
   | **SDA引脚** | `21` | I2C数据引脚 |
   | **SCL引脚** | `22` | I2C时钟引脚 |
   | **驱动名称** | `BMP280` | 固定值 |
   | **I2C地址** | `0x76` | 0x76或0x77 |
   | **采集间隔** | `5000` | 5秒（≥2000ms） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 等待5秒后查看气压、温度、海拔数据

> ⚠️ **重要**：仅ESP32-S3 full固件支持，需启用I2C传感器编译开关

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

```json
{
  "id": "bmp280_1",
  "name": "BMP280气压传感器",
  "type": 38,
  "enabled": false,
  "pins": [21, 22],
  "params": {
    "driver": "BMP280",
    "i2cAddress": "0x76",
    "interval": 5000
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| driver | 驱动名称，固定为 `"BMP280"` |
| i2cAddress | I2C 地址，通常为 `0x76`（SDO 接 GND）或 `0x77`（SDO 接 VCC） |
| interval | 采样间隔（ms），建议 ≥ 2000 |

> pins[0] = SDA 引脚，pins[1] = SCL 引脚

## 数据上报格式

传感器数据通过 MQTT 上报，包含多个测量值：

```json
[
  {"id": "bmp280_1", "value": "temperature:25.3,pressure:101325,altitude:10.5"}
]
```

| 字段 | 说明 | 单位 |
|------|------|------|
| temperature | 环境温度 | °C |
| pressure | 大气压强 | Pa |
| altitude | 估算海拔 | m |

## 驱动注册机制

BMP280 使用 `ISensorDriver` + `DriverRegistry` 机制注册：

- 编译时通过 `FASTBEE_ENABLE_I2C_SENSORS` 宏开关启用
- 驱动自动注册到全局注册表
- 配置中的 `driver` 字段用于匹配具体驱动实现

## 与外设执行联动

### Web界面配置步骤

**创建气压数据显示规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置定时触发器：
   - 触发类型：**定时触发**
   - 执行间隔：**10** 秒
4. 添加动作：
   - 动作1：读取传感器数据
     - 动作类型：**传感器读取**
     - 目标外设：**bmp280_1**
   - 动作2：显示到OLED
     - 动作类型：**OLED显示**
     - 目标外设：**oled1**
     - 动作值：
     ```
     #气象站
     P:${bmp280_1.pressure}Pa
     Alt:${bmp280_1.altitude}m
     ```
5. 点击 **保存**

> 💡 **提示**：使用 `${外设ID.字段名}` 引用传感器数据

---

### JSON配置示例

### 温度超限报警

```json
{
  "name": "高温报警",
  "enabled": true,
  "triggers": [
    {
      "type": "poll",
      "params": {
        "periphId": "bmp280_1",
        "field": "temperature",
        "operator": ">",
        "threshold": 35
      }
    }
  ],
  "actions": [
    {
      "type": "gpio_write",
      "params": {
        "periphId": "buzzer1",
        "value": "1"
      }
    }
  ]
}
```

### 气压数据显示到 OLED

```json
{
  "name": "显示气压",
  "enabled": true,
  "triggers": [
    {
      "type": "timer",
      "params": { "interval": 5000 }
    }
  ],
  "actions": [
    {
      "type": "sensor_read",
      "params": { "periphId": "bmp280_1" }
    },
    {
      "type": "display_write",
      "params": {
        "periphId": "oled1",
        "template": "P:{bmp280_1.pressure}Pa\nAlt:{bmp280_1.altitude}m"
      }
    }
  ]
}
```

## 注意事项

1. **固件版本**：仅 ESP32-S3 full 固件包含 BMP280 驱动，slim 固件不支持
2. **I2C 地址冲突**：同一 I2C 总线上不能有两个相同地址的设备，多个 BMP280 需设置不同 SDO 电平
3. **海拔精度**：海拔为估算值，基于标准大气压（101325 Pa），实际使用需校准基准气压
4. **采样频率**：BMP280 内部转换需时间，interval 建议 ≥ 2000ms
5. **接线长度**：I2C 线缆不宜过长（建议 < 50cm），过长需降低时钟频率或加中继
