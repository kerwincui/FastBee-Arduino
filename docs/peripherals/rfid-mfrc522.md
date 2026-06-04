# MFRC522 RFID 射频卡读取器

## 功能说明

MFRC522 是一款 13.56MHz RFID 读卡器模块，通过 SPI 总线通信，支持读取 Mifare S50/S70 等 IC 卡的 UID。适用于门禁系统、考勤打卡、身份识别等场景。

> **固件要求**：仅 ESP32-S3 full 固件支持（需启用 `FASTBEE_ENABLE_RFID` 编译开关）。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| SENSOR | 38 | SPI 传感器（MFRC522） |

## 事件编号

| 事件 | 编号 | 说明 |
|------|------|------|
| EVENT_RFID_CARD_DETECTED | 120 | 检测到新卡片 |
| EVENT_RFID_CARD_REMOVED | 121 | 卡片移除 |

## 硬件接线

| MFRC522 引脚 | ESP32-S3 GPIO | 说明 |
|-------------|--------------|------|
| SDA (SS) | GPIO 5 | SPI 片选 |
| SCK | GPIO 18 | SPI 时钟 |
| MOSI | GPIO 23 | SPI 主出从入 |
| MISO | GPIO 19 | SPI 主入从出 |
| RST | GPIO 4 | 复位引脚 |
| VCC | 3.3V | 电源 |
| GND | GND | 地 |

> MFRC522 工作电压为 3.3V，勿接 5V！

## 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加RFID读卡器外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `rfid1` | 唯一标识符 |
   | **名称** | `RFID读卡器` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | RFID驱动 |
   | **SS引脚** | `5` | SPI片选 |
   | **RST引脚** | `4` | 复位引脚 |
   | **驱动名称** | `MFRC522` | 固定值 |
   | **检测间隔** | `500` | 500ms（200-500） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 刷卡查看上报的UID

> ⚠️ **重要**：MFRC522必须使用3.3V供电，5V会损坏模块！仅ESP32-S3 full固件支持

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

```json
{
  "id": "rfid1",
  "name": "RFID读卡器",
  "type": 38,
  "enabled": false,
  "pins": [5, 4],
  "params": {
    "driver": "MFRC522",
    "ssPin": 5,
    "rstPin": 4,
    "interval": 500
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| driver | 驱动名称，固定为 `"MFRC522"` |
| ssPin | SPI 片选引脚（与 pins[0] 一致） |
| rstPin | 复位引脚（与 pins[1] 一致） |
| interval | 轮询检测间隔（ms），建议 200-500 |

> SPI 总线引脚（SCK/MOSI/MISO）使用 ESP32 默认 SPI 引脚，无需在配置中指定。

## 数据上报格式

检测到卡片时通过 MQTT 上报 UID：

```json
[{"id": "rfid1", "value": "uid:A1B2C3D4"}]
```

卡片移除时上报：

```json
[{"id": "rfid1", "value": "uid:removed"}]
```

## 事件触发机制

RFID 模块检测到卡片时会触发系统事件：

- `EVENT_RFID_CARD_DETECTED`（120）：新卡靠近
- `EVENT_RFID_CARD_REMOVED`（121）：卡片离开

这些事件可在 periph_exec 规则中作为触发条件使用。

## 与外设执行联动

### Web界面配置步骤

**创建刷卡开门规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件源：**rfid1**
   - 事件编号：**120**（检测到卡片）
4. 添加动作：
   - 动作1：舵机开门
     - 动作类型：**设置PWM**
     - 目标外设：**servo1**
     - 动作值：**180**
   - 动作2：延时3秒
     - 动作类型：**延时**
     - 延时值：**3000**（毫秒）
   - 动作3：舵机关门
     - 动作类型：**设置PWM**
     - 目标外设：**servo1**
     - 动作值：**0**
5. 点击 **保存**

> 💡 **提示**：可使用事件120（刷卡）和121（卡片移除）

---

### JSON配置示例

### 刷卡开门

```json
{
  "name": "刷卡开门",
  "enabled": true,
  "triggers": [
    {
      "type": "event",
      "params": {
        "periphId": "rfid1",
        "eventCode": 120
      }
    }
  ],
  "actions": [
    {
      "type": "servo_write",
      "params": {
        "periphId": "servo1",
        "value": "180"
      }
    },
    { "type": "delay", "params": { "ms": 3000 } },
    {
      "type": "servo_write",
      "params": {
        "periphId": "servo1",
        "value": "0"
      }
    }
  ]
}
```

### 刷卡显示 UID

```json
{
  "name": "显示卡号",
  "enabled": true,
  "triggers": [
    {
      "type": "event",
      "params": {
        "periphId": "rfid1",
        "eventCode": 120
      }
    }
  ],
  "actions": [
    {
      "type": "display_write",
      "params": {
        "periphId": "oled1",
        "template": "Card:\n{rfid1.uid}"
      }
    }
  ]
}
```

## 注意事项

1. **固件版本**：仅 ESP32-S3 full 固件包含 RFID 驱动，slim 固件不支持
2. **电压要求**：MFRC522 模块必须使用 3.3V 供电，5V 会损坏模块
3. **SPI 冲突**：如果同时使用 SD 卡等 SPI 设备，需使用不同的 CS 引脚
4. **读取距离**：典型读取距离 2-5cm，受卡片类型和天线质量影响
5. **多卡处理**：同时有多张卡时只能读取一张，需逐张操作
6. **UID 唯一性**：部分低价卡片 UID 可能重复，安全场景建议验证扇区数据
