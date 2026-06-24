---
title: Peripheral Execution
order: 50
---

# Peripheral Execution Rules

> This document is the **authoritative source** for trigger types and action types. Other documents reference this page via links for consistency.

The Peripheral Execution Engine (PeriphExecManager + PeriphExecExecutor) supports configuring automation rules via the Web interface — no coding required.

## Trigger Types

| Trigger | Type ID | Description | Configuration |
|--------|---------|------|---------|
| **Schedule** | `SCHEDULE` | Cron-based timed execution | `cronExpr`: Cron expression (sec min hour day month weekday) |
| **Event** | `EVENT` | Respond to device events | `eventType`: `boot`/`wifi_connect`/`mqtt_connect`/`ota_start` |
| **MQTT** | `MQTT` | Trigger on MQTT message | `topic`: subscription topic, `payload`: match content (optional) |
| **Condition** | `CONDITION` | Trigger when sensor data meets condition | `sensorId` + `operator` + `threshold` |

### Cron Expression Format

```
┌─ Second (0-59)
│ ┌─ Minute (0-59)
│ │ ┌─ Hour (0-23)
│ │ │ ┌─ Day (1-31)
│ │ │ │ ┌─ Month (1-12)
│ │ │ │ │ ┌─ Weekday (0-7, 0 and 7 = Sunday)
│ │ │ │ │ │
* * * * * *
```

**Common Examples**:
| Expression | Meaning |
|--------|------|
| `0 */5 * * * *` | Every 5 minutes |
| `0 0 8 * * *` | Daily at 8:00 AM |
| `0 0 18 * * 1-5` | Weekdays at 18:00 |
| `0 */30 9-17 * * 1-5` | Every 30 minutes, 9-5 on weekdays |

## Condition Operators

Used in condition triggers (`CONDITION`) to compare sensor readings against thresholds:

| Operator | Symbol | Description |
|--------|------|------|
| Greater Than | `>` | Sensor value greater than threshold |
| Less Than | `<` | Sensor value less than threshold |
| Greater or Equal | `>=` | Sensor value greater than or equal to threshold |
| Less or Equal | `<=` | Sensor value less than or equal to threshold |
| Equal | `==` | Sensor value equals threshold |
| Not Equal | `!=` | Sensor value not equal to threshold |

## Action Types

| Action | Type ID | Description | Target Peripheral |
|------|---------|------|---------|
| **GPIO Control** | | | |
| Set GPIO | `SET_GPIO` | Set pin high/low level | Digital Output |
| Toggle GPIO | `TOGGLE_GPIO` | Toggle pin level | Digital Output |
| PWM Output | `SET_PWM` | Set PWM duty cycle | PWM Output |
| **Delay Control** | | | |
| Delay | `DELAY` | Wait specified milliseconds | (none) |
| Set After Delay | `SET_GPIO_AFTER` | Set GPIO after delay | Digital Output |
| **Sensors** | | | |
| Read Sensor | `READ_SENSOR` | Trigger sensor reading | Sensor |
| Report Sensor | `REPORT_SENSOR` | Report sensor data via MQTT | Sensor |
| **Display Devices** | | | |
| LCD Print | `LCD_PRINT` | Display text on OLED/LCD | LCD |
| LCD Clear | `LCD_CLEAR` | Clear LCD display | LCD |
| NeoPixel Set | `NEOPIXEL_SET` | Set LED color/brightness | NeoPixel |
| NeoPixel Animate | `NEOPIXEL_ANIMATE` | Play preset animation | NeoPixel |
| Segment Display | `SEGMENT_DISPLAY` | Set 7-segment value | 7-Segment |
| LED Matrix Show | `LED_MATRIX_SHOW` | Display pattern/text on matrix | LED Matrix |
| **Communication** | | | |
| MQTT Publish | `MQTT_PUBLISH` | Publish message to MQTT topic | (none) |
| MQTT Request | `MQTT_REQUEST` | Publish request and wait response | (none) |
| HTTP Request | `HTTP_REQUEST` | Send HTTP request | (none) |
| Modbus Read | `MODBUS_READ` | Read Modbus registers | Modbus Sub-device |
| Modbus Write | `MODBUS_WRITE` | Write Modbus registers | Modbus Sub-device |
| **Advanced** | | | |
| If Condition | `IF_CONDITION` | Execute different actions based on condition | (none) |
| Parallel | `PARALLEL` | Execute multiple actions simultaneously | (none) |
| Sequence | `SEQUENCE` | Execute multiple actions in order | (none) |
| Repeat | `REPEAT` | Repeat action N times | (none) |
| Execute Rule | `EXECUTE_RULE` | Trigger another rule | (none) |
| Device Restart | `DEVICE_RESTART` | Restart device | (none) |
| Send Notification | `SEND_NOTIFICATION` | Send notification message | (none) |
| RFID Operation | `RFID_OPERATION` | RFID read/write card | RFID |

## Examples

### Example 1: Timed Light Control

```json
{
  "name": "Turn on light at 18:00",
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

### Example 2: High Temperature Auto Fan

```json
{
  "name": "High temp fan on",
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
        "payload": "{\"msg\":\"High temp alert: {{value}}°C\"}"
      }
    }
  ]
}
```

### Example 3: MQTT Remote Control

```json
{
  "name": "MQTT control pump",
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
