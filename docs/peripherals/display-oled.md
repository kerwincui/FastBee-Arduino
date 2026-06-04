# OLED 显示屏 (SSD1306/SH1106)

## 功能说明

OLED 显示屏通过 I2C 接口连接，用于本地显示设备状态、传感器数据、IP地址等信息。支持 SSD1306（128x64/128x32）和 SH1106 驱动芯片。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| LCD | 36 | LCD/OLED 显示设备 |

## 硬件接线（I2C）

| OLED 引脚 | 连接 | 说明 |
|-----------|------|------|
| VCC | 3.3V | 电源（部分模块支持5V） |
| GND | GND | 地 |
| SDA | GPIO 21（默认） | I2C 数据线 |
| SCL | GPIO 22（默认） | I2C 时钟线 |

> I2C 地址默认为 0x3C，部分模块为 0x3D

## 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加OLED显示外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `oled1` | 唯一标识符 |
   | **名称** | `OLED显示屏` | 显示名称 |
   | **外设类型** | **LCD显示** (type: 36) | OLED/LCD |
   | **引脚配置** | `21,22` | SDA,SCL（I2C接口） |
   | **屏幕宽度** | `128` | 像素 |
   | **屏幕高度** | `64` | 像素 |
   | **接口类型** | `2` | 2=I2C |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 屏幕应显示设备信息（IP、WiFi状态等）

> 💡 **提示**：I2C地址默认0x3C，部分模块为0x3D

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### 128x64 OLED

```json
{
  "id": "oled1",
  "name": "OLED显示屏",
  "type": 36,
  "enabled": false,
  "pins": [21, 22],
  "params": {
    "width": 128,
    "height": 64,
    "interface": 2
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| width | 屏幕宽度像素 |
| height | 屏幕高度像素 |
| interface | 接口类型：0=Parallel, 1=SPI, 2=I2C |

## 与外设执行联动

### Web界面配置步骤

**创建OLED显示规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置触发器（如定时触发，10秒间隔）
4. 添加动作：
   - 动作1：读取传感器数据
     - 动作类型：**传感器读取**
     - 目标外设：**dht1**
   - 动作2：显示到OLED
     - 动作类型：**OLED显示**
     - 目标外设：**oled1**
     - 动作值：
     ```
     #环境监测
     温度: ${dht1.temperature}°C
     湿度: ${dht1.humidity}%
     ```
5. 点击 **保存**

> 💡 **提示**：使用 `${外设ID.字段名}` 引用传感器数据，`#开头`为居中标题

---

### JSON配置示例

### OLED 自定义显示（ACTION_OLED_DISPLAY = 27）

`actionValue` 支持多行文本模板：

```json
{
  "targetPeriphId": "oled1",
  "actionType": 27,
  "actionValue": "#设备状态\nIP: ${sys.ip}\n温度: ${dht1.temperature}°C\n湿度: ${dht1.humidity}%",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

#### 模板语法

| 语法 | 说明 |
|------|------|
| `#标题` | 首行以 # 开头为居中标题 |
| `${id.field}` | 引用传感器缓存值（id=外设ID，field=字段名） |
| `$value` | 引用触发时接收到的值 |
| `\n` | 换行 |

### 显示传感器数据示例

```json
{
  "id": "exec_oled_status",
  "name": "OLED显示环境数据",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 0,
      "intervalSec": 10,
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
      "targetPeriphId": "oled1",
      "actionType": 27,
      "actionValue": "#环境监测\n温度: ${dht1.temperature}°C\n湿度: ${dht1.humidity}%\n更新: $value",
      "useReceivedValue": false,
      "syncDelayMs": 100,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

### 清屏动作

```json
{
  "targetPeriphId": "oled1",
  "actionType": 26,
  "actionValue": "",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

## 内置功能

系统启动后 OLED 自动显示：
- 设备名称
- WiFi 连接状态和 IP 地址
- MQTT 连接状态

通过外设执行规则可覆盖默认显示内容。

## 注意事项

1. **I2C 引脚**：ESP32 默认 I2C 为 SDA=21, SCL=22；其他芯片可能不同
2. **显示刷新**：频繁刷新（<100ms）可能导致闪烁，建议间隔 ≥1 秒
3. **字符限制**：128x64 屏幕约可显示 4-6 行文本（取决于字体大小）
4. **功耗**：OLED 自发光，全白屏幕功耗较高，深色背景更省电
5. **寿命**：OLED 有烧屏风险，避免长时间显示固定内容
