# Modbus 步进电机控制

## 实验概述

通过 Modbus RTU 协议控制 RS485 步进电机驱动器，实现远程精确定位、速度调节和方向控制。适用于工业自动化、智能农业（卷帘/灌溉阀门）、精密定位等场景。

## 硬件接线

### RS485 通信接线

| ESP32 引脚 | TTL转RS485模块 | 说明 |
|-----------|---------------|------|
| GPIO 16 | TXD | 串口发送 |
| GPIO 17 | RXD | 串口接收 |
| GPIO 4（可选） | DE/RE | 方向控制 |
| 3.3V | VCC | 电源 |
| GND | GND | 地 |

### RS485 到步进电机驱动器

| RS485 模块 | 步进电机驱动器 | 说明 |
|-----------|-------------|------|
| A+ | RS485-A | 差分信号正 |
| B- | RS485-B | 差分信号负 |

### 步进电机驱动器到电机

| 驱动器 | 步进电机 | 说明 |
|-------|---------|------|
| A+ | A 相正 | 电机线圈 A |
| A- | A 相负 | 电机线圈 A |
| B+ | B 相正 | 电机线圈 B |
| B- | B 相负 | 电机线圈 B |
| VCC | 外部电源 | 12-48V DC |

> 步进电机驱动器（如 DM542、TB6600 RS485 版）需独立供电，电压根据电机型号选择。

## FastBee 外设配置

1. 进入 **外设管理** 页面
2. 点击 **添加外设**
3. 类型选择 `UART`（type: 1）
4. 配置 Modbus RTU 参数

## JSON 配置示例

### 状态读取配置

```json
{
  "id": "modbus_stepper",
  "name": "Modbus步进电机",
  "type": 1,
  "enabled": false,
  "pins": [16, 17],
  "params": {
    "baudRate": 9600,
    "dataBits": 8,
    "parity": "none",
    "stopBits": 1,
    "dePin": 4,
    "protocol": "modbus_rtu",
    "slaveId": 1,
    "function": 3,
    "startRegister": 0,
    "registerCount": 4,
    "interval": 2000,
    "dataMap": [
      { "register": 0, "name": "position", "scale": 1, "unit": "steps" },
      { "register": 1, "name": "speed", "scale": 1, "unit": "RPM" },
      { "register": 2, "name": "status", "scale": 1, "unit": "" },
      { "register": 3, "name": "error", "scale": 1, "unit": "" }
    ]
  }
}
```

### 控制写入配置

```json
{
  "id": "modbus_stepper_ctrl",
  "name": "步进电机控制",
  "type": 1,
  "enabled": false,
  "pins": [16, 17],
  "params": {
    "protocol": "modbus_rtu",
    "slaveId": 1,
    "function": 6,
    "startRegister": 100,
    "registerCount": 3,
    "interval": 0,
    "controlMap": [
      { "register": 100, "name": "target_position", "min": -32768, "max": 32767 },
      { "register": 101, "name": "target_speed", "min": 0, "max": 3000 },
      { "register": 102, "name": "command", "values": { "start": 1, "stop": 2, "home": 3, "reset": 4 } }
    ]
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| slaveId | 驱动器从站地址（拨码设置） |
| function | 3=读寄存器，6=写单寄存器，16=写多寄存器 |
| controlMap | 控制寄存器映射，定义可写字段 |
| target_position | 目标位置（步数，带符号） |
| target_speed | 目标转速（RPM） |
| command | 运动命令：启动/停止/回零/复位 |

## 控制命令

通过 MQTT 发送控制：

```json
[{"id": "modbus_stepper_ctrl", "value": "target_position:2048,target_speed:100,command:1"}]
```

- `target_position`: 目标步数（正=正转，负=反转）
- `target_speed`: 运行速度 RPM
- `command`: 1=启动, 2=停止, 3=回零, 4=复位报警

### 滑台左右边界保护

若将 Modbus 步进电机用于直线滑台，建议在 Modbus 子设备配置中启用软限位：

```json
{
  "name": "滑台步进电机",
  "sensorId": "slide_motor",
  "deviceType": "motor",
  "slaveAddress": 8,
  "motorRegs": [0, 1, 2, 5, 7],
  "motorMinPosition": 0,
  "motorMaxPosition": 10000,
  "motorCurrentPosition": 0,
  "motorMoveStep": 1600,
  "motorLastPulse": 1600,
  "enabled": true
}
```

- `motorMinPosition`: 左边界，单位为步数。
- `motorMaxPosition`: 右边界，单位为步数。
- `motorCurrentPosition`: 当前估算位置，回零后通常设为 `0`。
- `motorMoveStep`: 每次 `forward`/`reverse` 默认移动步数。
- `motorLastPulse`: 最近一次 `setPulse` 脉冲数；规则未指定脉冲时会作为备用步长。

启用软限位后，系统会在发送正转/反转命令前计算下一位置。若会越过左右边界，会自动缩小本次脉冲数到边界内；若当前位置已经在边界，则拒绝动作。硬件限位开关和急停仍然必须保留，软件限位不能替代物理安全保护。

## 外设执行联动

### 定时往返运动

```json
{
  "name": "步进电机往返",
  "enabled": true,
  "triggers": [
    { "type": "timer", "params": { "interval": 10000 } }
  ],
  "actions": [
    {
      "type": "modbus_write",
      "params": {
        "periphId": "modbus_stepper_ctrl",
        "register": 100,
        "value": 2048
      }
    },
    {
      "type": "modbus_write",
      "params": {
        "periphId": "modbus_stepper_ctrl",
        "register": 102,
        "value": 1
      }
    }
  ]
}
```

### 按键触发回零

```json
{
  "name": "按键回零",
  "enabled": true,
  "triggers": [
    {
      "type": "event",
      "params": { "periphId": "btn1", "event": "pressed" }
    }
  ],
  "actions": [
    {
      "type": "modbus_write",
      "params": {
        "periphId": "modbus_stepper_ctrl",
        "register": 102,
        "value": 3
      }
    }
  ]
}
```

### 温度联动（配合温控卷帘）

```json
{
  "name": "高温开卷帘",
  "enabled": true,
  "triggers": [
    {
      "type": "poll",
      "params": {
        "periphId": "modbus_th",
        "field": "temperature",
        "operator": ">",
        "threshold": 32
      }
    }
  ],
  "actions": [
    {
      "type": "modbus_write",
      "params": { "periphId": "modbus_stepper_ctrl", "register": 100, "value": 5000 }
    },
    {
      "type": "modbus_write",
      "params": { "periphId": "modbus_stepper_ctrl", "register": 102, "value": 1 }
    }
  ]
}
```

## 注意事项

1. **驱动器选型**：确保驱动器支持 RS485 Modbus RTU 协议（非脉冲/方向型）
2. **寄存器地址**：不同品牌驱动器寄存器定义不同，务必查阅对应手册
3. **运动安全**：建议添加限位开关，防止过行程损坏机械结构
4. **急停功能**：硬件急停按钮必不可少，不能仅依赖软件停止
5. **通信超时**：步进电机执行耗时较长，轮询 interval 建议 ≥ 2000ms
6. **电源独立**：电机驱动电源与 ESP32 电源完全隔离，共地即可
