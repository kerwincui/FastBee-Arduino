# FastBee-Arduino 外设执行文档

> **概述**: 外设执行模块是实现“当条件满足时执行动作”的规则引擎,采用触发-动作模型,支持 4 种触发器(平台/定时/事件/轮询)和 27 种动作(GPIO/PWM/传感器/显示/系统等)。规则异步执行,Worker 池预创建 3 个任务,最小堆内存门控 30KB。配置存储在 `/config/periph_exec.json` 中。

出厂 `data/config/periph_exec.json` 中的规则全部默认禁用，避免未知接线环境下自动驱动外设。导入示例或创建规则后，请先确认目标外设存在且已完成引脚核对，再启用规则；Lite/Standard 文件系统生成会过滤当前档位不支持或引用已裁剪外设的动作。

本目录包含外设执行规则系统的分模块详细文档，按触发器、动作和完整场景组织。

## 文档索引

### 触发器文档 (triggers/)
| 文档 | triggerType | 说明 |
|------|:-----------:|------|
| [platform-trigger.md](./triggers/platform-trigger.md) | 0 | 平台触发（MQTT/Modbus指令下发） |
| [timer-trigger.md](./triggers/timer-trigger.md) | 1 | 定时触发（间隔/每日时间点） |
| [event-trigger.md](./triggers/event-trigger.md) | 4 | 事件触发（系统/按键/数据/外设事件） |
| [poll-trigger.md](./triggers/poll-trigger.md) | 5 | 轮询触发（本地数据源条件评估） |

### 动作文档 (actions/)
| 文档 | actionType | 说明 |
|------|:----------:|------|
| [gpio-actions.md](./actions/gpio-actions.md) | 0,1,13,14 | GPIO输出（高/低/反转） |
| [pwm-actions.md](./actions/pwm-actions.md) | 2,3,4,5 | PWM/呼吸灯/DAC |
| [system-actions.md](./actions/system-actions.md) | 6,7,8,9 | 系统重启/恢复/NTP/OTA |
| [modbus-actions.md](./actions/modbus-actions.md) | 16,17,18 | Modbus线圈/寄存器/轮询 |
| [sensor-actions.md](./actions/sensor-actions.md) | 19 | 传感器数据读取 |
| [display-actions.md](./actions/display-actions.md) | 24,25,26,27 | 数码管/OLED显示 |
| [event-actions.md](./actions/event-actions.md) | 21,22,23 | 触发事件/规则控制 |
| [script-actions.md](./actions/script-actions.md) | 10,15 | 调用外设/命令脚本 |

### 完整场景文档 (scenarios/)
| 文档 | 场景 | 涉及模块 |
|------|------|---------|
| [temperature-alarm.md](./scenarios/temperature-alarm.md) | DHT温控+继电器联动 | DHT11/22 + 继电器 |
| [smoke-alarm.md](./scenarios/smoke-alarm.md) | 烟雾报警+蜂鸣器 | MQ-2 + 蜂鸣器 + MQTT |
| [light-control.md](./scenarios/light-control.md) | 光敏自动灯控 | 光敏ADC + LED/继电器 |
| [button-control.md](./scenarios/button-control.md) | 按键多模式控制 | 按键 + 多动作 |
| [modbus-monitor.md](./scenarios/modbus-monitor.md) | Modbus设备监控 | Modbus子设备 + 报警 |
| [ultrasonic-alarm.md](./scenarios/ultrasonic-alarm.md) | 超声波距离告警 | HC-SR04 + 蜂鸣器 |

## 快速入门

### 规则结构

```json
{
  "id": "exec_<timestamp>",
  "name": "规则名称",
  "enabled": true,
  "execMode": 0,
  "triggers": [ /* 触发器数组，OR关系 */ ],
  "actions": [ /* 动作数组，顺序执行 */ ],
  "reportAfterExec": true
}
```

### 触发器类型速查

| triggerType | 名称 | 典型用途 |
|:-----------:|------|---------|
| 0 | 平台触发 | IoT平台MQTT指令匹配 |
| 1 | 定时触发 | 周期采集、定时控制 |
| 4 | 事件触发 | WiFi/MQTT/按键/传感器数据事件 |
| 5 | 轮询触发 | Modbus传感器周期数据条件判断 |

### 动作类型速查

| actionType | 名称 | 参数说明 |
|:----------:|------|---------|
| 0 | 高电平 | targetPeriphId=GPIO外设ID |
| 1 | 低电平 | targetPeriphId=GPIO外设ID |
| 2 | 闪烁 | actionValue=间隔ms |
| 3 | 呼吸灯 | actionValue=周期ms |
| 4 | 设置PWM | actionValue=占空比值 |
| 5 | 设置DAC | actionValue=0~255 |
| 6 | 系统重启 | 无参数 |
| 7 | 恢复出厂 | 无参数 |
| 8 | NTP同步 | 无参数 |
| 9 | OTA升级 | actionValue=URL |
| 10 | 调用外设 | targetPeriphId=目标外设 |
| 13 | 高电平(反转) | 物理输出低电平 |
| 14 | 低电平(反转) | 物理输出高电平 |
| 15 | 命令脚本 | actionValue=脚本内容 |
| 16 | Modbus线圈 | FC05 |
| 17 | Modbus寄存器 | FC06 |
| 18 | Modbus轮询 | 采集子设备数据 |
| 19 | 传感器读取 | 采集温度/湿度/距离/电流/电压 |
| 21 | 触发事件 | actionValue=事件ID |
| 22 | 启用规则 | targetPeriphId=规则ID |
| 23 | 禁用规则 | targetPeriphId=规则ID |
| 24 | 显示数字 | actionValue="12.34" |
| 25 | 显示文本 | actionValue="PLAY" |
| 26 | 清屏 | 无参数 |
| 27 | OLED显示 | actionValue=多行模板 |

## 条件运算符

| operatorType | 名称 | 说明 |
|:------------:|------|------|
| 0 | 等于 (EQ) | value == compareValue |
| 1 | 不等于 (NEQ) | value != compareValue |
| 2 | 大于 (GT) | value > compareValue |
| 3 | 小于 (LT) | value < compareValue |
| 4 | 大于等于 (GTE) | value >= compareValue |
| 5 | 小于等于 (LTE) | value <= compareValue |
| 6 | 区间内 (BETWEEN) | compareValue="min,max" |
| 7 | 区间外 (NOT_BETWEEN) | compareValue="min,max" |
| 8 | 包含 (CONTAIN) | 字符串包含 |
| 9 | 不包含 (NOT_CONTAIN) | 字符串不包含 |
