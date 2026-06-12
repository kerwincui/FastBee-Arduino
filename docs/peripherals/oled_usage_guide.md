# FastBee-Arduino OLED 显示屏使用指南

## 概述

FastBee-Arduino 现已支持 OLED/LCD 显示屏，通过简单的配置即可实现数据显示功能。

当前精简版默认保留 OLED/LCD 和 TM1637 数码管能力。外设执行页面中，显示相关动作已按“显示屏”类别聚合展示，用户只需要在“显示数字、显示文本、数码管清屏、OLED 自定义显示”四类动作中选择即可。若硬件项目不需要显示屏，可在构建配置中关闭 `FASTBEE_ENABLE_LCD` 或 `FASTBEE_ENABLE_SEVEN_SEGMENT` 进一步节省资源。

OLED 使用流程是先在外设配置中添加显示屏对象，再在外设执行中选择显示动作，把固定文本、传感器变量或模板内容写到屏幕。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

![外设执行规则列表](../system/images/periph-exec-management.png)

![显示输出管道](../images/display-output-pipeline.svg)

显示调试建议先跑静态文本，再接入传感器变量，最后通过外设执行规则做定时刷新、事件刷新或告警文本。

## 支持的显示屏

FastBee 底层使用 `U8g2` 库驱动显示屏，通过 `DisplayController` 枚举选择控制器。当前支持的控制器如下：

| 控制器枚举 | 编号 | 典型尺寸/分辨率 | 接口 | 说明 |
|-----------|------|---------------|------|------|
| `SSD1306` | 0 | 0.96"/1.3" · 128x64 / 128x32 | I2C / SPI | 最常见的单色 OLED，默认选项 |
| `SH1106` | 1 | 1.3" · 128x64 | I2C / SPI | 1.3 寸 OLED，SSD1306 的兼容型号 |
| `SSD1309` | 2 | 2.42" · 128x64 | I2C / SPI | 大尺寸 OLED |
| `ST7567` | 3 | 128x64 LCD | I2C / SPI | 单色 LCD |
| `ST7920` | 4 | 128x64 LCD | SPI | 带内置中文字库的 LCD |
| `PCD8544` | 5 | 84x48 LCD | SPI | Nokia 5110 经典屏幕 |

**接口类型** (`DisplayInterface`)：

| 枚举 | 编号 | 说明 |
|------|------|------|
| `PARALLEL` | 0 | 并行接口（目前预留） |
| `SPI_MODE` | 1 | SPI 串行接口 |
| `I2C_MODE` | 2 | I2C 串行接口（默认推荐） |

**推荐配置**：I2C 接口的 SSD1306 128x64 OLED，驱动成本低、资源占用少。

## 硬件连接

### I2C OLED (SSD1306) 接线

```
OLED显示屏    ESP32开发板
----------------------------------
VCC     →     3.3V
GND     →     GND
SCL     →     GPIO22 (或自定义)
SDA     →     GPIO21 (或自定义)
```

## 配置方法

### 方法一：通过 Web 界面配置（推荐）

1. 打开 Web 管理界面
2. 进入"外设管理"页面
3. 点击"添加外设"
4. 选择类型：LCD
5. 配置参数：
   - 名称：OLED显示屏
   - 宽度：128
   - 高度：64
   - 接口：I2C
   - SDA引脚：21
   - SCL引脚：22
6. 保存配置

### 方法二：通过配置文件

编辑 `/config/peripherals.json`：

```json
{
  "peripherals": [
    {
      "id": "oled_01",
      "name": "OLED显示屏",
      "type": 36,
      "enabled": true,
      "pinCount": 2,
      "pins": [21, 22],
      "params": {
        "lcd": {
          "width": 128,
          "height": 64,
          "interface": 2
        }
      }
    }
  ]
}
```

### 方法三：代码中创建

```cpp
#include "peripherals/LCDManager.h"
#include "core/PeripheralConfig.h"

void setup() {
    PeripheralConfig config;
    config.id = "oled_display";
    config.name = "OLED显示屏";
    config.type = PeripheralType::LCD;
    config.enabled = true;
    config.pinCount = 2;
    config.pins[0] = 21;  // SDA
    config.pins[1] = 22;  // SCL
    config.params.lcd.width = 128;
    config.params.lcd.height = 64;
    config.params.lcd.interface = 2;  // I2C
    
    LCDManager::getInstance().initialize(config);
}
```

## 使用方法

### 1. 显示文本

#### API 调用

```bash
# 显示文本（坐标模式）
curl -X POST http://192.168.x.x/api/lcd/text \
  -d "text=Hello FastBee" \
  -d "x=0" \
  -d "y=10" \
  -d "align=1"

# 显示文本（行号模式）
curl -X POST http://192.168.x.x/api/lcd/text \
  -d "text=温度: 25.5°C" \
  -d "line=0"
```

#### 代码调用

```cpp
LCDManager& lcd = LCDManager::getInstance();

// 显示单行
lcd.printLine("Hello FastBee", 0);

// 显示多行
String lines[] = {
    "FastBee IoT",
    "IP: 192.168.1.100",
    "Temp: 25.5 C",
    "Humidity: 60%"
};
lcd.printLines(lines, 4);
```

### 2. 显示传感器数据

```cpp
LCDManager& lcd = LCDManager::getInstance();

// 显示传感器数据（自动格式化）
lcd.showSensorData("温度", 25.5, "°C", 0);
lcd.showSensorData("湿度", 60.0, "%", 1);
lcd.showSensorData("气压", 1013.2, "hPa", 2);

lcd.refresh();
```

### 3. 显示系统信息

```bash
# API 调用
curl -X POST http://192.168.x.x/api/lcd/info
```

```cpp
// 代码调用
LCDManager::getInstance().showSystemInfo();
```

显示内容：
- 项目名称
- IP 地址
- 内存使用率
- 运行时间

### 4. 清屏

```bash
# API 调用
curl -X POST http://192.168.x.x/api/lcd/clear
```

```cpp
// 代码调用
LCDManager::getInstance().clear();
LCDManager::getInstance().refresh();
```

### 5. 设置字体

```bash
# API 调用
curl -X POST http://192.168.x.x/api/lcd/font -d "font=1"
```

```cpp
// 代码调用
LCDManager& lcd = LCDManager::getInstance();
lcd.setFont(0);  // 小字体
lcd.setFont(1);  // 中字体（默认）
lcd.setFont(2);  // 大字体
```

### 6. 查询状态

```bash
# API 调用
curl http://192.168.x.x/api/lcd/status
```

响应：
```json
{
  "success": true,
  "data": {
    "initialized": true,
    "width": 128,
    "height": 64,
    "maxLines": 6,
    "fontHeight": 10
  }
}
```

## 完整示例

### 示例1：温度监测器

```cpp
#include "peripherals/LCDManager.h"

void displayTemperature(float temp, float humidity) {
    LCDManager& lcd = LCDManager::getInstance();
    
    lcd.clear();
    lcd.printLine("Weather Monitor", 0);
    lcd.showSensorData("Temp", temp, "C", 2);
    lcd.showSensorData("Humidity", humidity, "%", 3);
    lcd.refresh();
}

void loop() {
    float temp = readTemperature();
    float humidity = readHumidity();
    displayTemperature(temp, humidity);
    delay(5000);
}
```

### 示例2：设备状态显示

```cpp
void displayDeviceStatus() {
    LCDManager& lcd = LCDManager::getInstance();
    
    String status[] = {
        "Device Status:",
        WiFi.status() == WL_CONNECTED ? "WiFi: OK" : "WiFi: OFF",
        "Free: " + String(ESP.getFreeHeap() / 1024) + "KB",
        "Uptime: " + String(millis() / 60000) + "min"
    };
    
    lcd.printLines(status, 4);
}
```

## 性能优化

### 1. 减少刷新频率

```cpp
// 建议：不超过 10Hz（每100ms一次）
unsigned long lastUpdate = 0;
const unsigned long updateInterval = 100;  // ms

void loop() {
    if (millis() - lastUpdate >= updateInterval) {
        updateDisplay();
        lastUpdate = millis();
    }
}
```

### 2. 按需刷新

```cpp
// 只在数据变化时刷新
float lastTemp = 0;

void updateTemperature(float temp) {
    if (temp != lastTemp) {
        lcd.showSensorData("Temp", temp, "C", 0);
        lcd.refresh();
        lastTemp = temp;
    }
}
```

### 3. 局部更新

```cpp
// 只更新变化的区域（减少传输数据）
lcd.print("Temp: 25.5C", 0, 20);  // 只更新温度区域
lcd.refresh();  // 发送整个缓冲区（OLED 特性）
```

## 常见问题

### Q1: 显示屏不亮？

**检查**：
1. 接线是否正确（VCC、GND、SDA、SCL）
2. I2C 地址是否正确（默认 0x3C，部分模块为 0x3D）
3. 是否调用了 `initialize()` 方法

### Q2: 显示乱码？

**原因**：
- 字体不支持某些字符
- 中文字符需要特殊字体支持

**解决**：
```cpp
// 使用支持中文的字体（需额外配置）
// 或仅显示英文和数字
```

### Q3: 内存不足？

**症状**：ESP32 重启或显示异常

**解决**：
- 使用 SSD1306 而非大尺寸 TFT
- 减少显示缓存
- 使用 ESP32-WROVER（带 PSRAM）

### Q4: 刷新太慢？

**优化**：
- 降低刷新频率（10Hz 足够）
- 只在必要时刷新
- 避免在循环中频繁调用 `clear()`

## API 参考

### REST API

| 端点 | 方法 | 参数 | 说明 |
|------|------|------|------|
| `/api/lcd/text` | POST | text, x, y, line, align | 显示文本 |
| `/api/lcd/clear` | POST | - | 清屏 |
| `/api/lcd/info` | POST | - | 显示系统信息 |
| `/api/lcd/font` | POST | font(0-2) | 设置字体 |
| `/api/lcd/status` | GET | - | 获取状态 |

### C++ API

#### 初始化与状态

| 方法 | 说明 |
|------|------|
| `initialize(config)` | 根据外设配置初始化显示屏 |
| `deinitialize()` | 释放显示屏资源 |
| `isInitialized()` | 查询是否已初始化 |
| `getWidth()` / `getHeight()` | 返回屏幕像素宽高 |
| `getFontHeight()` | 当前字体的行高 |
| `getMaxLines()` | 当前字体下可显示的最大行数 |

#### 基础显示

| 方法 | 说明 |
|------|------|
| `clear()` | 清空缓冲区 |
| `refresh()` | 将缓冲区推送到屏幕（含防拖油 50ms 间隔） |
| `print(text, x, y, align)` | 按坐标显示文本 |
| `printLine(text, line)` | 按行号显示文本 |
| `printLines(lines[], count)` | 一次性显示多行 |
| `showCustomText(content)` | 解析 `\n` 多行文本，首行 `#` 开头自动识别为居中标题并绘分隔线 |
| `showSystemInfo()` | 显示 IP/WiFi/内存/运行时间等系统信息 |

#### 传感器数据显示模块（自动轮播）

LCDManager 内置通用的传感器数据表（最多 16 条）和自动分页轮播机制，配合外设执行可实现多传感器数据的轮换展示。

| 方法 | 说明 |
|------|------|
| `updateSensorEntry(id, label, value, unit, decimals)` | 注册或更新一条传感器数据 |
| `invalidateSensorEntry(id)` | 将传感器条目标记为无效（采集失败时调用） |
| `showSensorPage(page=-1)` | 显示指定页或自动轮播 |
| `autoRefreshSensorDisplay(intervalMs)` | 在主循环内调用，按指定间隔自动翻页 |
| `getSensorEntryCount()` | 当前注册的传感器条数 |
| `getSensorPageCount()` | 当前数据的总页数 |
| `showSensorData(name, value, unit, line)` | 单条简单显示（不入表） |

#### 图形绘制

| 方法 | 说明 |
|------|------|
| `drawLine(x1, y1, x2, y2)` | 绘制直线 |
| `drawRect(x, y, w, h)` | 绘制空心矩形 |
| `drawBox(x, y, w, h)` | 绘制填充矩形 |
| `drawCircle(x, y, r)` | 绘制空心圆 |
| `drawDisc(x, y, r)` | 绘制填充圆 |

#### 外观与字体

| 方法 | 说明 |
|------|------|
| `setFont(index)` | 设置字体（0=小，1=中（默认），2=大） |
| `setContrast(value)` | 设置对比度 (0-255) |
| `setFlip(flip)` | 翻转显示（折叠安装时使用） |
| `setDisplayOn(on)` | 开/关显示 |

## 资源占用

| 项目 | 占用 |
|------|------|
| Flash | +50KB（u8g2 库） |
| RAM | +1KB（128x64 OLED 缓冲区） |
| CPU | < 3%（10Hz 刷新） |

**结论**：对 ESP32 性能影响极小，可放心使用。

---

**维护者**：FastBee 开发团队
