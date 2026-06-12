# LCD1602(IIC) 字符屏

LCD1602 是 16 列 x 2 行字符液晶屏，常见 I2C 转接板为 PCF8574，地址通常是 `0x27` 或 `0x3F`。

## 当前支持状态

当前 FastBee 的 `LCD` 外设类型（type 36）运行时驱动基于 U8g2，已覆盖 SSD1306/SH1106 这类图形 OLED；尚未内置 HD44780/PCF8574 字符屏驱动。因此 LCD1602 目前可以保留为配置占位和后续扩展文档，导入后不能直接通过 `ACTION_OLED_DISPLAY(27)` 驱动显示。

可直接使用的替代方案：

- 需要多行文本和传感器变量显示：使用 SSD1306/SH1106 OLED。
- 只显示 4 位数字：使用 TM1637 数码管。
- 必须使用 LCD1602：在 `esp32s3-full` 或自定义固件中补 HD44780/PCF8574 驱动。

当前 LCD1602 建议作为占位配置记录在外设页；真正需要显示动作时，优先参考 OLED 或 TM1637 的可用页面入口。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

![外设执行规则列表](../system/images/periph-exec-management.png)

截图要点：LCD1602 当前更适合作为兼容计划和配置占位来说明。配置截图用于标注 I2C 地址、SDA/SCL 和背光控制需求；若项目需要立即落地显示功能，建议优先选择已有动作链更完整的 OLED 或 TM1637 页面。

## 硬件接线

| LCD1602 I2C 引脚 | ESP32 引脚 | 说明 |
| --- | --- | --- |
| VCC | 5V 或 3.3V | 按模块要求供电 |
| GND | GND | 共地 |
| SDA | GPIO21 | I2C 数据 |
| SCL | GPIO22 | I2C 时钟 |

## 配置占位示例

示例默认禁用，仅用于记录接线和预留扩展。导入后不要启用，除非固件已补 LCD1602 驱动。

```json
{
  "id": "lcd1602_01",
  "name": "LCD1602-IIC字符屏",
  "type": 36,
  "enabled": false,
  "pinCount": 2,
  "pins": [21, 22, 255, 255, 255, 255, 255, 255],
  "params": {
    "width": 16,
    "height": 2,
    "interface": 2
  }
}
```

## 建议的驱动扩展

1. 在 `LCDManager` 增加字符屏模式识别：`width=16`、`height=2`、`interface=2`。
2. 增加 PCF8574 I2C 写入时序，支持清屏、定位、写字符串、背光控制。
3. 在外设执行中复用 `ACTION_OLED_DISPLAY(27)`，把多行文本拆成 2 行，每行 16 字符。
4. 文档和 UI 增加 `address` 参数，默认 `0x27`，可选 `0x3F`。

## 注意事项

- LCD1602 字符集有限，通常不支持中文。
- I2C 转接板需要通过电位器调对比度，否则会看起来像没有显示。
- 5V 供电模块的 I2C 上拉可能也是 5V，ESP32 需要确认电平安全。
