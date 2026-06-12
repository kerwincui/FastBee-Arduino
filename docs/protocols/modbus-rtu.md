# Modbus RTU 配置

## 功能说明

FastBee 支持作为 Modbus RTU Master，通过串口连接工业传感器和控制设备。支持自动从站扫描、寄存器批量读取和周期轮询。

![Modbus RTU 配置页面](../system/images/protocol-modbus-rtu.png)

配置时先固定 UART 引脚和波特率，再添加从站和寄存器映射。RS485 总线现场请确认 A/B 线、终端电阻、从站地址和轮询间隔。

![Modbus RTU 主站轮询链路](../images/modbus-rtu-polling-flow.svg)

如果轮询没有数据，先确认主站 UART 与 RS485 收发器，再逐个检查从站地址、功能码、寄存器地址和响应超时。

![Modbus API 与功能码映射](../images/modbus-api-function-map.svg)

配置面板、REST API 和功能码可以按上图对应起来。现场调试时，不要只看页面按钮是否点击成功，还要确认它实际调用的是读线圈、写线圈、读寄存器还是写寄存器。

## 操作指南

1. 配置 UART 外设作为 Modbus 串口
2. 在 Web 界面配置 Modbus 从站设备
3. 定义寄存器读取规则
4. 启用轮询

## 参数说明

### 串口配置

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| 串口端口 | UART1/UART2 | UART1 |
| TX 引脚 | 发送引脚 | GPIO16 |
| RX 引脚 | 接收引脚 | GPIO17 |
| 波特率 | 通信速率 | 9600 |
| 数据位 | 数据位数 | 8 |
| 停止位 | 停止位数 | 1 |
| 校验 | 校验方式 | 无（0） |

### 从站设备配置

| 配置项 | 说明 |
|--------|------|
| 从站地址 | 1-247 |
| 功能码 | 03(读保持寄存器), 04(读输入寄存器) |
| 起始地址 | 寄存器起始地址 |
| 寄存器数量 | 读取的寄存器个数 |
| 轮询间隔 | 读取周期（毫秒） |
| 数据类型 | INT16/UINT16/FLOAT32 |

### 支持的功能码

| 功能码 | 说明 | 操作 |
|--------|------|------|
| 0x01 | 读线圈 | 读取开关量 |
| 0x02 | 读离散输入 | 读取输入状态 |
| 0x03 | 读保持寄存器 | 读取数据（常用） |
| 0x04 | 读输入寄存器 | 读取只读数据 |
| 0x05 | 写单线圈 | 控制单个开关 |
| 0x06 | 写单寄存器 | 写入单个值 |
| 0x10 | 写多寄存器 | 批量写入 |

## 配置示例

### 外设配置（peripherals.json）

```json
{
  "id": "modbus_rtu",
  "name": "Modbus RTU-工业设备",
  "type": 1,
  "enabled": false,
  "pins": [16, 17],
  "params": {
    "baudRate": 9600,
    "dataBits": 8,
    "stopBits": 1,
    "parity": 0
  }
}
```

### 读取温湿度传感器（从站地址1）

```json
{
  "name": "Modbus温湿度读取",
  "enabled": true,
  "triggers": [
    { "type": "timer", "params": { "interval": 5000 } }
  ],
  "actions": [
    {
      "type": "modbus_read",
      "params": {
        "peripheralId": "modbus_rtu",
        "slaveId": 1,
        "functionCode": 3,
        "startAddress": 0,
        "quantity": 2,
        "dataType": "int16",
        "scale": 0.1
      }
    }
  ]
}
```

### 控制继电器模块（从站地址2）

```json
{
  "name": "Modbus继电器控制",
  "enabled": true,
  "triggers": [
    { "type": "platform", "params": { "topic": "modbus/relay" } }
  ],
  "actions": [
    {
      "type": "modbus_write",
      "params": {
        "peripheralId": "modbus_rtu",
        "slaveId": 2,
        "functionCode": 5,
        "address": 0,
        "value": "${payload}"
      }
    }
  ]
}
```

## 故障排除

| 问题 | 可能原因 | 解决方案 |
|------|---------|---------|
| 通信超时 | 接线错误/波特率不匹配 | 检查 TX-RX 交叉接线和波特率 |
| CRC 错误 | 电气干扰/线路过长 | 缩短线路或添加终端电阻（120Ω） |
| 从站无响应 | 地址错误 | 确认从站地址（使用扫描功能） |
| 数据异常 | 数据类型/字节序错误 | 检查 INT16/FLOAT32 和大小端设置 |
| 只能读不能写 | 功能码错误 | 读用03/04，写用05/06/10 |

## 相关文档

- [Modbus 使用指南](modbus_usage_guide.md) — 详细教程
- [Modbus 设备外设](../peripherals/modbus-device.md) — 外设类型配置
