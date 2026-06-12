# Modbus 子设备

## 功能说明

Modbus 子设备外设用于通过 RS485 总线控制外部 Modbus 从站设备，如继电器模块、PWM 控制器、PID 控制器、电机驱动器等。支持线圈写入（FC05）和寄存器写入（FC06）两种控制协议。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| MODBUS_DEVICE | 51 | Modbus RS485 子设备 |

## 通信方式

- **物理层**：RS485 半双工
- **协议**：Modbus RTU
- **控制方式**：通过 UART 外设配置的 RS485 端口发送命令

> Modbus 子设备不占用 GPIO 引脚（通过已配置的 UART/RS485 端口通信）

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。Modbus 子设备还需要先在通信协议页确认 Modbus RTU 串口参数、从站地址和波特率。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

![Modbus RTU 配置页面](../system/images/protocol-modbus-rtu.png)

![Modbus RTU 主站轮询链路](../images/modbus-rtu-polling-flow.svg)

Modbus 子设备的外设对象只描述“从站和寄存器怎么映射到系统”，底层串口、波特率和总线连线仍以通信协议页配置为准。

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加Modbus子设备外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `mb_relay` 或 `mb_pwm` | 唯一标识符 |
   | **名称** | `4路继电器` 或 `4路PWM调光` | 显示名称 |
   | **外设类型** | **Modbus设备** (type: 51) | RS485控制 |
   | **从站地址** | `1` 或 `2` | Modbus地址(1-247) |
   | **通道数量** | `4` | 继电器路数或PWM路数 |
   | **控制协议** | **线圈(FC05)** 或 **寄存器(FC06)** | 根据设备类型 |
   | **设备类型** | **relay** 或 **pwm** | 继电器或PWM |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 使用控制面板测试控制

> ⚠️ **重要**：Modbus设备依赖UART外设提供RS485物理通道，需先配置UART

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

### 4路继电器模块

```json
{
  "id": "mb_relay",
  "name": "4路继电器",
  "type": 51,
  "enabled": false,
  "pins": [],
  "params": {
    "slaveAddress": 1,
    "channelCount": 4,
    "coilBase": 0,
    "ncMode": false,
    "controlProtocol": 0,
    "deviceType": 0,
    "deviceIndex": 0,
    "batchRegister": 0,
    "pwmRegBase": 0,
    "pwmResolution": 0,
    "motorRegs": [0, 0, 0, 0, 0],
    "motorDecimals": 0,
    "sensorId": ""
  }
}
```

### PWM 调光模块

```json
{
  "id": "mb_pwm",
  "name": "4路PWM调光",
  "type": 51,
  "enabled": false,
  "pins": [],
  "params": {
    "slaveAddress": 2,
    "channelCount": 4,
    "coilBase": 0,
    "ncMode": false,
    "controlProtocol": 1,
    "deviceType": 1,
    "deviceIndex": 1,
    "batchRegister": 0,
    "pwmRegBase": 100,
    "pwmResolution": 10,
    "motorRegs": [0, 0, 0, 0, 0],
    "motorDecimals": 0,
    "sensorId": ""
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| slaveAddress | Modbus 从站地址（1-247） |
| channelCount | 通道数量 |
| coilBase | 线圈/寄存器基地址 |
| ncMode | NC 常闭模式（true=常闭，控制逻辑反转） |
| controlProtocol | 控制协议：0=线圈(FC05), 1=寄存器(FC06) |
| deviceType | 设备类型：0=relay, 1=pwm, 2=pid, 3=motor |
| deviceIndex | 在 ModbusHandler config 中的索引 |
| batchRegister | 位图批量寄存器地址（0=不使用） |
| pwmRegBase | PWM 寄存器基地址 |
| pwmResolution | PWM 分辨率(bits) |
| motorRegs | 电机寄存器地址 [正转,反转,停止,速度,脉冲数] |
| motorDecimals | 电机参数小数位 |
| motorMinPosition | 电机/滑台软限位最小位置，单位为步数 |
| motorMaxPosition | 电机/滑台软限位最大位置；当 `motorMaxPosition > motorMinPosition` 时启用软限位 |
| motorCurrentPosition | 当前估算位置，回零或人工校准后应更新为对应步数 |
| motorMoveStep | `forward`/`reverse` 默认相对移动步数；未设置时使用最近一次 `setPulse` 值 |
| motorLastPulse | 最近一次脉冲数，运行时会更新 |
| sensorId | 传感器标识符（用于数据采集上报） |

### Modbus 电机滑台软限位

当子设备 `deviceType` 为 `motor` 时，可以通过以下字段限制滑台行程：

```json
{
  "deviceType": "motor",
  "motorRegs": [0, 1, 2, 5, 7],
  "motorMinPosition": 0,
  "motorMaxPosition": 10000,
  "motorCurrentPosition": 0,
  "motorMoveStep": 1600,
  "motorLastPulse": 1600
}
```

启用后，`forward` 会向 `motorMaxPosition` 方向移动，`reverse` 会向 `motorMinPosition` 方向移动。若本次移动会超过边界，系统会先把脉冲数缩小到边界内再发送方向命令；若已在边界或缺少可用脉冲数，则拒绝执行并返回 `MOTOR_SOFT_LIMIT`。

软限位依赖当前估算位置，首次上电、手动移动或回零后，需要把 `motorCurrentPosition` 校准到实际位置。真正的左右极限仍建议接硬件限位开关或急停，软件限位用于减少误操作和规则越界。

## 与外设执行联动

### Web界面配置步骤

**创建Modbus继电器控制规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置触发器（如平台触发、事件触发）
4. 添加Modbus动作：
   - 动作类型：**Modbus线圈写入**
   - 目标外设：**mb_relay**
   - 动作值：`{"ch":0,"val":1}`（通道0，开启）
5. 点击 **保存**

> 💡 **提示**：线圈写入(FC05)用于继电器，寄存器写入(FC06)用于PWM

---

### JSON配置示例

### Modbus 线圈写入（ACTION_MODBUS_COIL_WRITE = 16）

```json
{
  "targetPeriphId": "mb_relay",
  "actionType": 16,
  "actionValue": "{\"ch\":0,\"val\":1}",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

### Modbus 寄存器写入（ACTION_MODBUS_REG_WRITE = 17）

```json
{
  "targetPeriphId": "mb_pwm",
  "actionType": 17,
  "actionValue": "{\"reg\":100,\"val\":512}",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

### Modbus 轮询采集（ACTION_MODBUS_POLL = 18）

```json
{
  "targetPeriphId": "modbus-task:0",
  "actionType": 18,
  "actionValue": "{\"poll\":[0]}",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

## 注意事项

1. **UART 前提**：Modbus 设备依赖 UART 外设提供 RS485 物理通道，需先配置 UART
2. **地址唯一**：同一总线上每个设备地址必须唯一
3. **通信间隔**：连续命令间建议 ≥50ms 间隔（pollInterPollDelay 参数控制）
4. **超时重试**：默认响应超时 1000ms，最大重试 2 次
5. **pinCount=0**：Modbus 设备不使用 GPIO 引脚
