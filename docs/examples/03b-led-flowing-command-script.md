# 实验：命令脚本流水灯

## 实验概述

使用 FastBee 的**命令脚本动作**（ACTION_SCRIPT），通过 `PERIPH` 和 `DELAY` 命令序列实现 LED 流水灯效果。与 JavaScript 脚本不同，命令脚本采用声明式命令序列，更直观易读。

## 硬件接线

| 开发板标识 | GPIO引脚 | 连接设备 |
|-----------|---------|---------|
| D1 | GPIO15 | LED1（低电平点亮） |
| D2 | GPIO2 | LED2（低电平点亮） |
| D3 | GPIO0 | LED3（低电平点亮） |
| D4 | GPIO4 | LED4（低电平点亮） |

## FastBee 外设配置

本实验的 Web 操作入口如下：先在“外设配置”创建硬件对象，再在“外设执行”添加采集、控制或显示规则。新增外设时建议先保持禁用，确认接线后再启用。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

![外设执行规则列表](../system/images/periph-exec-management.png)

![命令脚本执行流程](../images/command-script-execution-flow.svg)

流水灯脚本适合按“单命令、短脚本、完整脚本”的顺序调试。先确认每个 LED 外设能单独开关，再把 `PERIPH` 和 `DELAY` 组合成流水效果。

为4个 LED 分别创建 GPIO_DIGITAL_OUTPUT 外设：

```json
{
  "peripherals": [
    {
      "id": "cmd_led1",
      "name": "命令流水灯-LED1",
      "type": 12,
      "enabled": true,
      "pins": [15],
      "params": { "initialState": 1 }
    },
    {
      "id": "cmd_led2",
      "name": "命令流水灯-LED2",
      "type": 12,
      "enabled": true,
      "pins": [2],
      "params": { "initialState": 1 }
    },
    {
      "id": "cmd_led3",
      "name": "命令流水灯-LED3",
      "type": 12,
      "enabled": true,
      "pins": [0],
      "params": { "initialState": 1 }
    },
    {
      "id": "cmd_led4",
      "name": "命令流水灯-LED4",
      "type": 12,
      "enabled": true,
      "pins": [4],
      "params": { "initialState": 1 }
    }
  ]
}
```

## 方式1：基础顺序流水灯

### 外设执行规则配置

```json
{
  "id": "rule_cmd_flow_basic",
  "name": "命令脚本-基础流水灯",
  "enabled": true,
  "triggers": [
    { "type": "manual" }
  ],
  "actions": [
    {
      "type": 15,
      "actionType": 15,
      "actionValue": "PERIPH cmd_led1 LOW\nDELAY 300\nPERIPH cmd_led1 HIGH\nPERIPH cmd_led2 LOW\nDELAY 300\nPERIPH cmd_led2 HIGH\nPERIPH cmd_led3 LOW\nDELAY 300\nPERIPH cmd_led3 HIGH\nPERIPH cmd_led4 LOW\nDELAY 300\nPERIPH cmd_led4 HIGH\nLOG 基础流水灯一轮完成",
      "execMode": 0
    }
  ]
}
```

### 命令脚本解析

```
PERIPH cmd_led1 LOW      # 点亮LED1（低电平）
DELAY 300                # 延时300ms
PERIPH cmd_led1 HIGH     # 熄灭LED1（高电平）
PERIPH cmd_led2 LOW      # 点亮LED2
DELAY 300                # 延时300ms
PERIPH cmd_led2 HIGH     # 熄灭LED2
...（依次类推）
LOG 基础流水灯一轮完成    # 打印日志
```

### 执行方式

- **手动触发**：通过 API 或 Web 界面手动触发规则
- **API 调用**：`POST /api/periph-exec/rules/rule_cmd_flow_basic/trigger`

---

## 方式2：定时器自动循环流水灯

### 外设执行规则配置

```json
{
  "id": "rule_cmd_flow_timer",
  "name": "命令脚本-定时循环流水灯",
  "enabled": true,
  "triggers": [
    { 
      "type": "timer", 
      "params": { 
        "interval": 1500,
        "repeat": -1
      } 
    }
  ],
  "actions": [
    {
      "type": 15,
      "actionType": 15,
      "actionValue": "PERIPH cmd_led1 LOW\nDELAY 300\nPERIPH cmd_led1 HIGH\nPERIPH cmd_led2 LOW\nDELAY 300\nPERIPH cmd_led2 HIGH\nPERIPH cmd_led3 LOW\nDELAY 300\nPERIPH cmd_led3 HIGH\nPERIPH cmd_led4 LOW\nDELAY 300\nPERIPH cmd_led4 HIGH",
      "execMode": 0
    }
  ]
}
```

### 配置说明

- `interval: 1500` - 每1.5秒执行一轮流水（4个LED × 300ms + 延时余量）
- `repeat: -1` - 无限循环
- 无需手动触发，系统启动后自动运行

---

## 方式3：来回往复流水灯

### 外设执行规则配置

```json
{
  "id": "rule_cmd_flow_bidirectional",
  "name": "命令脚本-往复流水灯",
  "enabled": true,
  "triggers": [
    { 
      "type": "timer", 
      "params": { 
        "interval": 2500,
        "repeat": -1
      } 
    }
  ],
  "actions": [
    {
      "type": 15,
      "actionType": 15,
      "actionValue": "PERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nPERIPH cmd_led2 LOW\nDELAY 200\nPERIPH cmd_led2 HIGH\nPERIPH cmd_led3 LOW\nDELAY 200\nPERIPH cmd_led3 HIGH\nPERIPH cmd_led4 LOW\nDELAY 200\nPERIPH cmd_led4 HIGH\nPERIPH cmd_led3 LOW\nDELAY 200\nPERIPH cmd_led3 HIGH\nPERIPH cmd_led2 LOW\nDELAY 200\nPERIPH cmd_led2 HIGH",
      "execMode": 0
    }
  ]
}
```

### 执行顺序

```
LED1 → LED2 → LED3 → LED4 → LED3 → LED2 → (循环)
```

实现类似"乒乓球"的来回效果。

---

## 方式4：叠加渐亮流水灯

### 外设执行规则配置

```json
{
  "id": "rule_cmd_flow_accumulate",
  "name": "命令脚本-叠加流水灯",
  "enabled": true,
  "triggers": [
    { 
      "type": "timer", 
      "params": { 
        "interval": 2000,
        "repeat": -1
      } 
    }
  ],
  "actions": [
    {
      "type": 15,
      "actionType": 15,
      "actionValue": "PERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led2 LOW\nDELAY 200\nPERIPH cmd_led3 LOW\nDELAY 200\nPERIPH cmd_led4 LOW\nDELAY 500\nPERIPH cmd_led1 HIGH\nPERIPH cmd_led2 HIGH\nPERIPH cmd_led3 HIGH\nPERIPH cmd_led4 HIGH",
      "execMode": 0
    }
  ]
}
```

### 效果说明

- LED依次点亮，不熄灭
- 4个LED全部点亮后延时500ms
- 然后全部熄灭，开始下一轮

---

## 方式5：随机流水灯（结合MQTT命令）

### 外设执行规则配置

```json
{
  "id": "rule_cmd_flow_random",
  "name": "命令脚本-随机流水灯",
  "enabled": true,
  "triggers": [
    { 
      "type": "timer", 
      "params": { 
        "interval": 500,
        "repeat": -1
      } 
    }
  ],
  "actions": [
    {
      "type": 15,
      "actionType": 15,
      "actionValue": "PERIPH cmd_led1 HIGH\nPERIPH cmd_led2 HIGH\nPERIPH cmd_led3 HIGH\nPERIPH cmd_led4 HIGH\nMQTT 0 [{\"id\":\"flow_state\",\"value\":\"RANDOM(1,4)\"}]\nLOG 随机流水灯触发",
      "execMode": 0
    },
    {
      "type": 10,
      "actionType": 10,
      "targetPeriphId": "cmd_led{{received.value}}",
      "actionValue": "LOW",
      "useReceivedValue": false,
      "execMode": 0
    }
  ]
}
```

### 说明

- 使用 `RANDOM(1,4)` 生成随机数
- 通过 MQTT 上报当前点亮的LED编号
- 随机点亮其中一个LED

---

## 命令脚本语法参考

### 支持的命令

| 命令 | 格式 | 说明 |
|------|------|------|
| **PERIPH** | `PERIPH <id> <action> [value]` | 控制外设 |
| **DELAY** | `DELAY <ms>` | 延时（毫秒） |
| **LOG** | `LOG <message>` | 打印日志 |
| **MQTT** | `MQTT <qos> <payload>` | 发送MQTT消息 |

### PERIPH 子命令

| 子命令 | 格式 | 说明 |
|--------|------|------|
| HIGH | `PERIPH <id> HIGH` | 输出高电平 |
| LOW | `PERIPH <id> LOW` | 输出低电平 |
| PWM | `PERIPH <id> PWM <value>` | 设置PWM值(0-4095) |
| BLINK | `PERIPH <id> BLINK <interval_ms>` | 闪烁 |
| COLOR | `PERIPH <id> COLOR <#RRGGBB>` | NeoPixel颜色 |

### 命令分隔符

- 使用 `\n`（换行符）分隔多条命令
- JSON 中需转义为 `\\n`

---

## 命令脚本 vs JavaScript 脚本对比

| 特性 | 命令脚本 (ACTION_SCRIPT) | JavaScript 脚本 |
|------|-------------------------|----------------|
| **语法** | 声明式命令序列 | 编程语言 |
| **可读性** | ⭐⭐⭐⭐⭐ 非常直观 | ⭐⭐⭐ 需要编程基础 |
| **灵活性** | ⭐⭐⭐ 固定命令集 | ⭐⭐⭐⭐⭐ 完全自由 |
| **性能** | ⭐⭐⭐⭐ 轻量级解析 | ⭐⭐⭐ 需要JS引擎 |
| **适用场景** | 简单时序控制、调试 | 复杂逻辑、计算 |
| **变量支持** | ❌ 不支持 | ✅ 支持getVar/setVar |
| **条件判断** | ❌ 不支持 | ✅ 支持if/else |
| **循环** | ❌ 不支持（需配合定时器） | ✅ 支持for/while |

### 选择建议

- ✅ **使用命令脚本**：简单时序、初学者、快速原型、调试外设
- ✅ **使用JavaScript脚本**：复杂逻辑、状态机、动态计算、条件分支

---

## 注意事项

1. **命令长度限制**：单条 actionValue 建议不超过 512 字符
2. **DELAY 精度**：实际延时可能有 ±10ms 误差
3. **GPIO0 限制**：启动时不要将 GPIO0 拉低，否则会进入下载模式
4. **阻塞执行**：命令脚本是同步执行的，DELAY 会阻塞后续命令
5. **触发频率**：定时器间隔应大于脚本总执行时间，避免队列堆积
6. **日志输出**：使用 LOG 命令可追踪脚本执行进度，便于调试
7. **错误处理**：如果 PERIPH 命令中的外设ID不存在，会跳过并继续执行

---

## 扩展练习

### 练习1：呼吸灯效果

使用 PWM 命令实现 LED 呼吸效果：

```json
{
  "actionValue": "PERIPH cmd_led1 PWM 0\nDELAY 50\nPERIPH cmd_led1 PWM 512\nDELAY 50\nPERIPH cmd_led1 PWM 1024\nDELAY 50\nPERIPH cmd_led1 PWM 2048\nDELAY 50\nPERIPH cmd_led1 PWM 4095\nDELAY 50\nPERIPH cmd_led1 PWM 2048\nDELAY 50\nPERIPH cmd_led1 PWM 1024\nDELAY 50\nPERIPH cmd_led1 PWM 512\nDELAY 50\nPERIPH cmd_led1 PWM 0"
}
```

### 练习2：交通灯模拟

使用3个LED模拟红黄绿灯：

```json
{
  "actionValue": "PERIPH led_red LOW\nDELAY 3000\nPERIPH led_red HIGH\nPERIPH led_yellow LOW\nDELAY 1000\nPERIPH led_yellow HIGH\nPERIPH led_green LOW\nDELAY 3000\nPERIPH led_green HIGH\nLOG 交通灯一轮完成"
}
```

### 练习3：摩尔斯电码

使用LED发送SOS信号：

```json
{
  "actionValue": "PERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nDELAY 200\nPERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nDELAY 200\nPERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nDELAY 600\nPERIPH cmd_led1 LOW\nDELAY 600\nPERIPH cmd_led1 HIGH\nDELAY 600\nPERIPH cmd_led1 LOW\nDELAY 600\nPERIPH cmd_led1 HIGH\nDELAY 600\nPERIPH cmd_led1 LOW\nDELAY 600\nPERIPH cmd_led1 HIGH\nDELAY 600\nPERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nDELAY 200\nPERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nDELAY 200\nPERIPH cmd_led1 LOW\nDELAY 200\nPERIPH cmd_led1 HIGH\nLOG SOS发送完成"
}
```

---

## 相关文档

- [实验3：LED流水灯（JavaScript脚本）](03-led-flowing.md)
- [脚本动作文档](../periph-exec/actions/script-actions.md)
- [外设执行配置指南](../periph-exec/periph-exec-configuration-guide.md)
- [脚本命令参考](../periph-exec/script-guide.md)

---

**提示**：命令脚本非常适合快速验证外设功能和实现简单的时序控制。对于更复杂的逻辑，建议参考 [03-led-flowing.md](03-led-flowing.md) 使用 JavaScript 脚本实现。
