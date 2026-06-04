# 设备事件虚拟外设

## 功能说明

设备事件（DEVICE_EVENT）是一种**虚拟外设**，不对应物理硬件，仅作为事件发射源。通过外设执行规则中的 `ACTION_TRIGGER_EVENT(21)` 动作触发事件，事件可通过 MQTT DEVICE_EVENT 主题上报至平台，也可被其他规则订阅实现链式联动。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| DEVICE_EVENT | 60 | 设备事件虚拟外设 |

## 无需硬件

- 不使用 GPIO 引脚（pinCount = 0）
- 纯逻辑层面的事件源定义
- 通过规则引擎触发和响应

## JSON 配置示例

### 设备故障事件

```json
{
  "id": "evt_fault",
  "name": "设备故障事件",
  "type": 60,
  "enabled": false,
  "pins": [],
  "params": {}
}
```

### 自定义报警事件

```json
{
  "id": "evt_alarm",
  "name": "自定义报警",
  "type": 60,
  "enabled": false,
  "pins": [],
  "params": {}
}
```

## 与外设执行联动

### 触发设备事件（ACTION_TRIGGER_EVENT = 21）

在规则动作中触发事件：

```json
{
  "targetPeriphId": "evt_alarm",
  "actionType": 21,
  "actionValue": "温度超限",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

触发后：
1. 事件以规则 ID 为事件源分发给其他规则
2. 若规则 `reportAfterExec: true`，通过 MQTT DEVICE_EVENT 主题上报

### 订阅事件（作为触发源）

其他规则可通过事件触发器订阅该事件：

```json
{
  "triggerType": 4,
  "triggerPeriphId": "",
  "eventId": "exec_1234567"
}
```

> `eventId` 为触发事件的规则 ID，实现规则间的链式联动。

### 完整链式联动示例

**规则A**：温度超限 → 触发报警事件

```json
{
  "id": "exec_temp_check",
  "name": "温度超限检测",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 0,
      "triggerPeriphId": "dht1",
      "operatorType": 2,
      "compareValue": "40",
      "timerMode": 0,
      "intervalSec": 60,
      "timePoint": "",
      "eventId": "",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "evt_alarm",
      "actionType": 21,
      "actionValue": "高温报警",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": true
}
```

**规则B**：响应报警事件 → 执行蜂鸣器

```json
{
  "id": "exec_alarm_action",
  "name": "报警响应",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 4,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 0,
      "intervalSec": 60,
      "timePoint": "",
      "eventId": "exec_temp_check",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "buzzer",
      "actionType": 2,
      "actionValue": "500",
      "useReceivedValue": false,
      "syncDelayMs": 0,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": false
}
```

## 系统内置设备事件

系统预定义了以下设备事件 ID（无需手动配置 DEVICE_EVENT 外设）：

| 事件ID | EventType | 说明 |
|--------|-----------|------|
| sys_breakdown | 110 | 设备故障 |
| sys_restart | 111 | 设备重启 |
| device_alarm | 112 | 设备告警 |
| low_power | 113 | 低电量预警 |

## 注意事项

1. **无物理操作**：DEVICE_EVENT 不控制任何硬件，仅用于逻辑事件传递
2. **链式限制**：避免创建循环触发链（A触发B → B触发A），可能导致无限循环
3. **MQTT 上报**：事件通过 DEVICE_EVENT 主题上报时会携带事件名称和数据
4. **去重保护**：同一事件最小触发间隔 500ms，防止快速重复触发
