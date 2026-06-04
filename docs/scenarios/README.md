# 应用场景文档

完整的 FastBee-Arduino 应用场景配置示例，展示如何组合外设配置与外设执行规则实现实际项目需求。

## 场景列表

| 场景 | 说明 | 涉及外设 |
|------|------|----------|
| [Modbus 传感器设备](modbus-sensor-devices.md) | 接入温湿度/空气质量/土壤等 Modbus 传感器 | MODBUS_DEVICE |
| [Modbus 控制设备](modbus-control-devices.md) | 接入继电器/电机控制器等 Modbus 执行设备 | MODBUS_DEVICE |
| [OLED 显示温湿度](display-temperature-oled.md) | DHT传感器读取 → OLED实时显示 | SENSOR + LCD(SSD1306) |
| [TM1637 显示温湿度](display-temperature-tm1637.md) | DHT传感器读取 → 数码管实时显示 | SENSOR + TM1637 |

## 场景特点

每个场景文档包含：
1. **场景描述** — 实际应用需求说明
2. **硬件清单** — 所需硬件模块和接线
3. **外设配置** — 完整的 peripherals.json 配置
4. **执行规则** — 完整的 periph_exec.json 配置
5. **运行验证** — 预期效果和调试方法

## 适用环境

| 场景 | ESP32 (slim) | ESP32-S3 (full) |
|------|:---:|:---:|
| Modbus 传感器设备 | ✅ | ✅ |
| Modbus 控制设备 | ✅ | ✅ |
| OLED 显示温湿度 | ✅ | ✅ |
| TM1637 显示温湿度 | ✅ | ✅ |

## 相关文档

- [外设管理](../system/peripheral-management.md) — 外设添加与配置
- [外设执行管理](../system/periph-exec-management.md) — 规则编排
- [外设文档](../peripherals/README.md) — 各外设类型详细说明
