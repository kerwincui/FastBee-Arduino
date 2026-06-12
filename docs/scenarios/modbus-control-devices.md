# Modbus 控制设备配置

## 场景概述

通过 Modbus RTU 协议连接外部控制设备（如多路继电器模块、电机控制器、调光器等），实现远程控制和自动化联动。

## 适用设备

| 设备类型 | 典型型号 | 控制功能 |
|---------|---------|---------|
| 多路继电器 | 4/8/16 路 RS485 继电器 | 通断控制 |
| 电机控制器 | 变频器 RS485 | 转速/方向控制 |
| 调光模块 | 0-10V/PWM RS485 | 灯光亮度调节 |
| 电磁阀 | RS485 比例阀 | 流量控制 |

## 硬件接线

### RS485 接口连接

| ESP32 引脚 | TTL转RS485模块 | 说明 |
|-----------|---------------|------|
| GPIO 16 (TX) | TXD | 串口发送 |
| GPIO 17 (RX) | RXD | 串口接收 |
| GPIO 4（可选） | DE/RE | 方向控制（半双工） |
| 3.3V | VCC | 电源 |
| GND | GND | 地 |

## 外设配置

### 方式1：Web界面配置（推荐）

本场景需要同时确认外设配置和 Modbus RTU 串口参数。控制类设备上线前建议先保持规则禁用，手动执行单路动作验证地址和功能码。

![外设配置列表](../system/images/peripheral-management.png)

![Modbus RTU 配置页面](../system/images/protocol-modbus-rtu.png)

![外设执行规则列表](../system/images/periph-exec-management.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加4路继电器模块

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `modbus_relay4` | 4路继电器 |
   | **名称** | `4路继电器` | 显示名称 |
   | **外设类型** | **UART** (type: 1) | Modbus RTU |
   | **引脚配置** | `16,17` | TX=GPIO16, RX=GPIO17 |
   | **协议** | `modbus_rtu` | Modbus RTU |
   | **从站地址** | `10` | 拨码设置 |
   | **功能码** | `5` | 写单个线圈 |
   | **起始寄存器** | `0` | 查手册 |
   | **寄存器数量** | `4` | 4路继电器 |

3. 点击 **保存**

> 💡 **提示**：功能码5（Write Single Coil），写入65280（0xFF00）为ON，0为OFF

#### 步骤3：添加电机控制器（可选）

1. 再次点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `modbus_motor` | 电机控制器 |
   | **名称** | `电机控制器` | 显示名称 |
   | **外设类型** | **UART** (type: 1) | Modbus RTU |
   | **引脚配置** | `16,17` | TX=GPIO16, RX=GPIO17 |
   | **协议** | `modbus_rtu` | Modbus RTU |
   | **从站地址** | `11` | 拨码设置 |
   | **功能码** | `6` | 写单寄存器 |
   | **起始寄存器** | `0` | 查手册 |
   | **寄存器数量** | `2` | 速度+方向 |

3. 点击 **保存**

> 💡 **提示**：功能码6（Write Single Register）用于写入保持寄存器

---

### 方式2：JSON配置文件导入

## 控制命令

### 通过 MQTT 发送控制

控制继电器：
```json
[{"id": "modbus_relay4", "value": "relay1:1,relay2:0"}]
```

控制电机：
```json
[{"id": "modbus_motor", "value": "speed:500,direction:1"}]
```

调节亮度：
```json
[{"id": "modbus_dimmer", "value": "brightness:75"}]
```

### 通过 Web 界面控制

在外设管理页面可直接操作各控制通道的开关/数值。

## 与外设执行联动

### 温度联动继电器（配合传感器场景）

```json
{
  "name": "高温开风扇继电器",
  "enabled": true,
  "triggers": [
    {
      "type": "poll",
      "params": {
        "periphId": "modbus_temp_humi",
        "field": "temperature",
        "operator": ">",
        "threshold": 30
      }
    }
  ],
  "actions": [
    {
      "type": "modbus_write",
      "params": {
        "periphId": "modbus_relay4",
        "register": 0,
        "value": 65280
      }
    }
  ]
}
```

### 定时控制灯光

```json
{
  "name": "定时开灯",
  "enabled": true,
  "triggers": [
    {
      "type": "timer",
      "params": { "cron": "0 18 * * *" }
    }
  ],
  "actions": [
    {
      "type": "modbus_write",
      "params": {
        "periphId": "modbus_dimmer",
        "register": 0,
        "value": 80
      }
    }
  ]
}
```

### 按键手动控制

```json
{
  "name": "按键控制继电器",
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
        "periphId": "modbus_relay4",
        "register": 0,
        "value": 65280
      }
    }
  ]
}
```

## 状态回读

部分控制设备支持状态回读：

```json
{
  "id": "modbus_relay4_status",
  "name": "继电器状态",
  "type": 1,
  "enabled": false,
  "pins": [16, 17],
  "params": {
    "protocol": "modbus_rtu",
    "slaveId": 10,
    "function": 1,
    "startRegister": 0,
    "registerCount": 4,
    "interval": 2000
  }
}
```

> 功能码 1（Read Coils）用于读取线圈状态。

## 注意事项

1. **功能码选择**：线圈控制用功能码 5/15，寄存器控制用功能码 6/16
2. **写入确认**：写入后建议回读状态确认执行成功
3. **互锁保护**：相反动作（如正转/反转）需设计互锁逻辑，避免同时导通
4. **掉电记忆**：部分继电器模块支持掉电记忆，重启后恢复上次状态
5. **响应超时**：控制设备响应较快，超时设置建议 500-1000ms
6. **安全联锁**：关键设备控制建议添加硬件急停按钮，不完全依赖软件控制
