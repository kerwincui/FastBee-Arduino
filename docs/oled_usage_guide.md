# FastBee-Arduino OLED 显示屏使用指南

## 概述

FastBee-Arduino 现已支持 OLED/LCD 显示屏，通过简单的配置即可实现数据显示功能。

## 支持的显示屏

| 型号 | 尺寸 | 分辨率 | 接口 | 状态 |
|------|------|--------|------|------|
| SSD1306 | 0.96寸 | 128x64 | I2C | ✅ 完全支持 |
| SSD1306 | 0.91寸 | 128x32 | I2C | ✅ 完全支持 |
| SH1106 | 1.3寸 | 128x64 | I2C | ✅ 完全支持 |
| 其他 | - | - | SPI | ⚠️ 预留接口 |

**推荐使用 I2C 接口的 SSD1306 OLED**，性能最优。

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

| 方法 | 说明 |
|------|------|
| `initialize(config)` | 初始化显示屏 |
| `clear()` | 清空缓冲区 |
| `print(text, x, y, align)` | 显示文本（坐标模式） |
| `printLine(text, line)` | 显示文本（行号模式） |
| `printLines(lines[], count)` | 显示多行文本 |
| `showSensorData(name, value, unit, line)` | 显示传感器数据 |
| `showSystemInfo()` | 显示系统信息 |
| `refresh()` | 刷新显示（发送缓冲区） |
| `setFont(index)` | 设置字体 (0-2) |
| `setContrast(value)` | 设置对比度 (0-255) |
| `setDisplayOn(bool)` | 开关显示 |

## 资源占用

| 项目 | 占用 |
|------|------|
| Flash | +50KB (u8g2库) |
| RAM | +1KB (128x64 OLED缓冲区) |
| CPU | < 3% (10Hz刷新) |

**结论**：对 ESP32 性能影响极小，可放心使用。

## 下一步

- [ ] 添加中文支持
- [ ] 支持更多显示屏型号
- [ ] 添加图形绘制API
- [ ] 支持SPI接口

---

**版本**：v1.0  
**更新日期**：2025-04-15  
**维护者**：FastBee开发团队
