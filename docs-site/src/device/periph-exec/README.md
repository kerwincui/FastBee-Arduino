---
title: 外设执行
order: 50
---

# 外设执行规则

> 本文档是触发器类型和动作类型的**权威来源**。其他文档通过链接引用此处，保持一致性。

外设执行引擎（PeriphExecManager + PeriphExecExecutor）支持通过 Web 页面配置自动化规则，无需编写代码。

## 触发器类型

| 触发器 | 类型标识 | 说明 | 配置参数 |
|--------|---------|------|---------|
| **定时触发** | `SCHEDULE` | 按 Cron 表达式定时执行 | `cronExpr`：Cron 表达式（秒 分 时 日 月 周） |
| **事件触发** | `EVENT` | 响应设备事件 | `eventType`：`boot`/`wifi_connect`/`mqtt_connect`/`ota_start` |
| **MQTT 触发** | `MQTT` | 接收 MQTT 消息触发 | `topic`：订阅主题，`payload`：匹配内容（可选） |
| **条件触发** | `CONDITION` | 传感器数据满足条件时触发 | `sensorId` + `operator` + `threshold` |

### Cron 表达式说明

```
┌─ 秒 (0-59)
│ ┌─ 分钟 (0-59)
│ │ ┌─ 小时 (0-23)
│ │ │ ┌─ 日 (1-31)
│ │ │ │ ┌─ 月 (1-12)
│ │ │ │ │ ┌─ 星期 (0-7, 0和7都是周日)
│ │ │ │ │ │
* * * * * *
```

**常用示例**：
| 表达式 | 含义 |
|--------|------|
| `0 */5 * * * *` | 每 5 分钟 |
| `0 0 8 * * *` | 每天早上 8:00 |
| `0 0 18 * * 1-5` | 工作日 18:00 |
| `0 */30 9-17 * * 1-5` | 工作日 9-17 点每 30 分钟 |

## 条件运算符

用于条件触发器（`CONDITION`）中比较传感器读数与阈值：

| 运算符 | 符号 | 说明 |
|--------|------|------|
| 大于 | `>` | 传感器值大于阈值 |
| 小于 | `<` | 传感器值小于阈值 |
| 大于等于 | `>=` | 传感器值大于等于阈值 |
| 小于等于 | `<=` | 传感器值小于等于阈值 |
| 等于 | `==` | 传感器值等于阈值 |
| 不等于 | `!=` | 传感器值不等于阈值 |

## 动作类型

| 动作 | 类型标识 | 说明 | 目标外设 |
|------|---------|------|---------|
| **GPIO 控制** | | | |
| 设置 GPIO | `SET_GPIO` | 设置引脚高低电平 | 数字输出 |
| 翻转 GPIO | `TOGGLE_GPIO` | 翻转引脚电平 | 数字输出 |
| PWM 输出 | `SET_PWM` | 设置 PWM 占空比 | PWM 输出 |
| **延时控制** | | | |
| 延时 | `DELAY` | 等待指定毫秒数 | (无) |
| 延时后设置 | `SET_GPIO_AFTER` | 延迟后设置 GPIO | 数字输出 |
| **传感器** | | | |
| 读取传感器 | `READ_SENSOR` | 触发传感器读取 | 传感器 |
| 上报传感器 | `REPORT_SENSOR` | 上报传感器数据到 MQTT | 传感器 |
| **显示设备** | | | |
| LCD 显示 | `LCD_PRINT` | 在 OLED/LCD 显示文本 | LCD |
| LCD 清除 | `LCD_CLEAR` | 清除 LCD 显示 | LCD |
| NeoPixel 控制 | `NEOPIXEL_SET` | 设置 LED 颜色/亮度 | NeoPixel |
| NeoPixel 动画 | `NEOPIXEL_ANIMATE` | 播放预设动画 | NeoPixel |
| 数码管显示 | `SEGMENT_DISPLAY` | 设置数码管数值 | 数码管 |
| LED 点阵 | `LED_MATRIX_SHOW` | 点阵屏显示图案/文字 | LED 点阵 |
| **通信** | | | |
| MQTT 发布 | `MQTT_PUBLISH` | 发布消息到 MQTT 主题 | (无) |
| MQTT 请求 | `MQTT_REQUEST` | 发布请求并等待响应 | (无) |
| HTTP 请求 | `HTTP_REQUEST` | 发送 HTTP 请求 | (无) |
| Modbus 读取 | `MODBUS_READ` | 读取 Modbus 寄存器 | Modbus 子设备 |
| Modbus 写入 | `MODBUS_WRITE` | 写入 Modbus 寄存器 | Modbus 子设备 |
| **高级** | | | |
| 条件分支 | `IF_CONDITION` | 根据条件执行不同动作 | (无) |
| 并行执行 | `PARALLEL` | 同时执行多个动作 | (无) |
| 序列执行 | `SEQUENCE` | 按顺序执行多个动作 | (无) |
| 重复执行 | `REPEAT` | 重复执行动作 N 次 | (无) |
| 执行规则 | `EXECUTE_RULE` | 触发另一个规则 | (无) |
| 设备重启 | `DEVICE_RESTART` | 重启设备 | (无) |
| 发通知 | `SEND_NOTIFICATION` | 发送通知消息 | (无) |
| RFID 操作 | `RFID_OPERATION` | RFID 读卡/写卡 | RFID |

## 示例

### 示例 1：定时开关灯

```json
{
  "name": "每天18点开灯",
  "enabled": true,
  "trigger": {
    "type": "SCHEDULE",
    "config": { "cronExpr": "0 0 18 * * *" }
  },
  "actions": [
    {
      "type": "SET_GPIO",
      "targetId": "relay-001",
      "config": { "value": 1 }
    }
  ]
}
```

### 示例 2：温度过高自动开风扇

```json
{
  "name": "高温开风扇",
  "enabled": true,
  "trigger": {
    "type": "CONDITION",
    "config": {
      "sensorId": "dht22-001",
      "measurement": "temperature",
      "operator": ">=",
      "threshold": 30
    }
  },
  "actions": [
    {
      "type": "SET_GPIO",
      "targetId": "fan-relay",
      "config": { "value": 1 }
    },
    {
      "type": "MQTT_PUBLISH",
      "config": {
        "topic": "device/alarm",
        "payload": "{\"msg\":\"高温预警: {{value}}°C\"}"
      }
    }
  ]
}
```

### 示例 3：MQTT 远程控制

```json
{
  "name": "MQTT控制水泵",
  "enabled": true,
  "trigger": {
    "type": "MQTT",
    "config": {
      "topic": "cmd/pump",
      "payload": "ON"
    }
  },
  "actions": [
    {
      "type": "SET_GPIO",
      "targetId": "pump-relay",
      "config": { "value": 1 }
    }
  ]
}
```
