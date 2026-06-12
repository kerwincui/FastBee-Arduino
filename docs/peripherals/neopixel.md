# WS2812B NeoPixel 灯珠

## 功能说明

WS2812B（NeoPixel）是一种可寻址 RGB LED 灯珠，通过单根数据线级联控制多颗灯珠，每颗灯珠可独立设置颜色。适用于氛围灯、状态指示、灯带等应用。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| NEO_PIXEL | 45 | WS2812B/WS2811 可寻址LED |

## 硬件接线

| NeoPixel 引脚 | 连接 | 说明 |
|--------------|------|------|
| VCC | 5V | 电源（每颗满亮约60mA） |
| GND | GND | 地 |
| DIN | GPIO | 数据输入（建议串联330Ω电阻） |

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。NeoPixel 保存前重点核对数据引脚、灯珠数量、颜色顺序和独立供电。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加NeoPixel灯带外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `neopixel1` | 唯一标识符 |
   | **名称** | `RGB灯带` | 显示名称 |
   | **外设类型** | **NeoPixel灯珠** (type: 45) | WS2812B驱动 |
   | **引脚配置** | `16` | DIN数据引脚 |
   | **灯珠数量** | `8` | 1-300颗 |
   | **全局亮度** | `50` | 0-255 |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 使用控制面板发送颜色值（如#FF0000红色）

> ⚠️ **重要**：超过10颗灯珠需独立5V供电，每颗满亮约60mA

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

```json
{
  "id": "neopixel1",
  "name": "RGB灯带",
  "type": 45,
  "enabled": false,
  "pins": [16],
  "params": {
    "count": 8,
    "brightness": 50
  }
}
```

### 参数说明

| 参数 | 说明 | 范围 |
|------|------|------|
| count | 灯珠数量 | 1-300 |
| brightness | 全局亮度 | 0-255 |

## 与外设执行联动

NeoPixel 通过脚本动作（ACTION_SCRIPT = 15）或 CALL_PERIPHERAL（10）控制。

### Web界面配置步骤

**创建RGB流水灯效果**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置触发器（如定时触发、事件触发）
4. 添加脚本动作：
   - 动作类型：**命令脚本**
   - 脚本内容：
   ```
   PERIPH neopixel1 COLOR #FF0000
   DELAY 500
   PERIPH neopixel1 COLOR #00FF00
   DELAY 500
   PERIPH neopixel1 COLOR #0000FF
   ```
5. 点击 **保存**

> 💡 **提示**：使用PERIPH命令控制颜色，DELAY控制延时

---

### JSON配置示例

### 脚本控制示例

```json
{
  "targetPeriphId": "",
  "actionType": 15,
  "actionValue": "PERIPH neopixel1 COLOR #FF0000\nDELAY 1000\nPERIPH neopixel1 COLOR #00FF00\nDELAY 1000\nPERIPH neopixel1 COLOR #0000FF",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

### 平台下发控制

通过 MQTT 下发颜色值：
```json
[{"id": "neopixel1", "value": "#FF5500"}]
```

## 注意事项

1. **供电**：每颗灯珠满亮最大 60mA，8颗 = 480mA，超过 10 颗需独立供电
2. **电平转换**：ESP32 GPIO 为 3.3V，WS2812B 要求 5V 逻辑，短距离可直连，长线建议加电平转换
3. **串联电阻**：数据线建议串联 330Ω 电阻保护 GPIO
4. **电容滤波**：电源端建议并联 100μF 电容防止电压波动
5. **最大数量**：受内存和刷新时间限制，建议单条最多 300 颗
