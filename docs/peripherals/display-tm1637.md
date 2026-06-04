# TM1637 数码管

## 功能说明

TM1637 是一款 4 位 7 段数码管驱动芯片，通过 2 线串行接口（CLK + DIO）通信。适用于显示数字、时间、简单文本等。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| SEVEN_SEGMENT_TM1637 | 47 | TM1637 4位数码管 |

## 硬件接线

| TM1637 引脚 | 连接 | 说明 |
|-------------|------|------|
| VCC | 3.3V / 5V | 电源 |
| GND | GND | 地 |
| CLK | GPIO（任意） | 时钟线 |
| DIO | GPIO（任意） | 数据线 |

## 配置方式

### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加TM1637数码管外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `tm1637_1` | 唯一标识符 |
   | **名称** | `4位数码管` | 显示名称 |
   | **外设类型** | **数码管显示** (type: 47) | TM1637驱动 |
   | **CLK引脚** | `18` | 时钟引脚 |
   | **DIO引脚** | `19` | 数据引脚 |
   | **亮度** | `4` | 0-7（0最暗，7最亮） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 数码管应显示默认内容

> 💡 **提示**：建议使用5V供电以获得最佳亮度

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

```json
{
  "id": "tm1637_1",
  "name": "4位数码管",
  "type": 47,
  "enabled": false,
  "pins": [18, 19],
  "params": {
    "brightness": 4
  }
}
```

### 参数说明

| 参数 | 说明 | 范围 |
|------|------|------|
| brightness | 显示亮度 | 0-7（0最暗，7最亮） |

> pins[0] = CLK 引脚，pins[1] = DIO 引脚

## 与外设执行联动

### Web界面配置步骤

**创建数码管显示规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置触发器（如定时触发，5秒间隔）
4. 添加动作：
   - 动作1：读取传感器数据
     - 动作类型：**传感器读取**
     - 目标外设：**dht1**
   - 动作2：显示到数码管
     - 动作类型：**显示数字**
     - 目标外设：**tm1637_1**
     - 开启 **使用接收值**（显示温度值）
5. 点击 **保存**

> 💡 **提示**：可使用“显示数字”、“显示文本”或“清屏”动作

---

### JSON配置示例

### 显示数字（ACTION_DISPLAY_NUMBER = 24）

```json
{
  "targetPeriphId": "tm1637_1",
  "actionType": 24,
  "actionValue": "12.34",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

支持的数字格式：
- `"1234"` — 显示 1234
- `"12.34"` — 显示 12.34（带小数点）
- `"12:34"` — 显示 12:34（带冒号，适合时钟）

### 显示文本（ACTION_DISPLAY_TEXT = 25）

```json
{
  "targetPeriphId": "tm1637_1",
  "actionType": 25,
  "actionValue": "PLAY",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

> 仅支持部分可显示字符：0-9, A-F, H, L, P, U, b, d, o, n, r, t, -

### 清屏（ACTION_DISPLAY_CLEAR = 26）

```json
{
  "targetPeriphId": "tm1637_1",
  "actionType": 26,
  "actionValue": "",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

### 使用接收值显示

当 `useReceivedValue: true` 时，将触发时接收到的传感器数据直接显示：

```json
{
  "id": "exec_show_temp",
  "name": "数码管显示温度",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 0,
      "intervalSec": 5,
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
      "targetPeriphId": "tm1637_1",
      "actionType": 24,
      "actionValue": "",
      "useReceivedValue": true,
      "syncDelayMs": 100,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": false
}
```

## 注意事项

1. **引脚顺序**：配置中 pins[0] 为 CLK，pins[1] 为 DIO，不可颠倒
2. **字符限制**：仅 4 位显示，超过 4 位的数字/文本会被截断
3. **亮度调节**：亮度 0-7 级可调，通过 Web 界面或配置修改
4. **供电**：建议使用 5V 供电以获得最佳亮度，3.3V 也可工作但偏暗
5. **刷新频率**：显示内容变化后立即更新，无需关注刷新率
