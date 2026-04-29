# PeriphExec (外设执行) 模块 - 完整业务逻辑文档

## 目录

- [1. 模块概述](#1-模块概述)
- [2. 数据模型](#2-数据模型)
- [3. 规则生命周期 (CRUD)](#3-规则生命周期-crud)
- [4. 触发器类型详解](#4-触发器类型详解)
- [5. 动作类型详解](#5-动作类型详解)
- [6. 触发-动作执行流程](#6-触发-动作执行流程)
- [7. 核心方法分析](#7-核心方法分析)
- [8. 异步执行引擎](#8-异步执行引擎)
- [9. 数据转换管道与上报控制](#9-数据转换管道与上报控制)
- [10. 按钮事件子系统](#10-按钮事件子系统)
- [11. 配置持久化与版本迁移](#11-配置持久化与版本迁移)
- [12. API 路由层](#12-api-路由层)
- [13. 已知问题与优化建议](#13-已知问题与优化建议)

---

## 1. 模块概述

PeriphExec (Peripheral Execution) 是 FastBee-Arduino 物联网设备的**规则引擎**模块，实现"当条件满足时执行动作"的自动化逻辑。

### 核心架构

```
┌─────────────────────────────────────────────────────────────┐
│                      触发源 (Trigger Sources)                │
│                                                             │
│  MQTT消息  │  定时器  │  系统事件  │  轮询数据  │  按钮事件   │
└─────┬──────┴────┬─────┴─────┬──────┴─────┬──────┴─────┬─────┘
      │           │           │            │            │
      ▼           ▼           ▼            ▼            ▼
┌─────────────────────────────────────────────────────────────┐
│                 PeriphExecManager (单例)                      │
│                                                             │
│  ┌──────────┐  ┌──────────────┐  ┌────────────────────┐    │
│  │ 规则存储  │  │  条件评估引擎  │  │  动作执行/调度引擎  │    │
│  │ map<id,  │  │ evaluateCond │  │ sync / async       │    │
│  │  rule>   │  │  ition()     │  │ dispatch           │    │
│  └──────────┘  └──────────────┘  └────────────────────┘    │
│                                                             │
│  ┌──────────────────┐  ┌──────────────────────────────┐    │
│  │ 持久化 (LittleFS) │  │  数据上报 (MQTT/TCP/HTTP/CoAP)│    │
│  └──────────────────┘  └──────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 关键设计约束

| 约束项 | 值 | 说明 |
|--------|-----|------|
| 每规则最大触发器数 | 3 | `MAX_TRIGGERS_PER_RULE` |
| 每规则最大动作数 | 4 | `MAX_ACTIONS_PER_RULE` |
| 最大并发异步任务 | 3 | `MAX_ASYNC_TASKS` |
| 异步任务最小堆内存 | 30000 bytes | `MIN_HEAP_FOR_ASYNC` |
| 脚本任务栈大小 | 8192 bytes | `SCRIPT_TASK_STACK` |
| 普通异步任务栈大小 | 4096 bytes | `SIMPLE_TASK_STACK` |
| 异步任务优先级 | 0 (最低) | `ASYNC_TASK_PRIORITY` |
| 同一规则触发器关系 | OR (任一匹配即触发) | — |
| 同一规则动作关系 | 顺序执行 (按数组顺序) | — |

---

## 2. 数据模型

### 2.1 枚举定义

#### ExecTriggerType - 触发器类型

| 值 | 名称 | 说明 |
|----|------|------|
| 0 | `PLATFORM_TRIGGER` | 平台数据触发 - MQTT 下发数据满足条件时触发 |
| 1 | `TIMER_TRIGGER` | 定时触发 - 按时间间隔或每日定点触发 |
| 4 | `EVENT_TRIGGER` | 事件触发 - 系统事件 (WiFi/MQTT/按钮等) 触发 |
| 5 | `POLL_TRIGGER` | 轮询触发 - 外设轮询数据满足条件时触发 |

#### ExecActionType - 动作类型

| 值 | 名称 | 分类 | 说明 |
|----|------|------|------|
| 0 | `ACTION_HIGH` | GPIO | 设置引脚高电平 |
| 1 | `ACTION_LOW` | GPIO | 设置引脚低电平 |
| 2 | `ACTION_BLINK` | GPIO | 闪烁 (actionValue=次数) |
| 3 | `ACTION_BREATHE` | GPIO | 呼吸灯效果 |
| 4 | `ACTION_PWM` | GPIO | PWM 输出 (actionValue=占空比) |
| 5 | `ACTION_DAC` | GPIO | DAC 输出 (actionValue=电压值) |
| 6 | `ACTION_INVERTED` | GPIO | 翻转当前电平状态 |
| 7 | `ACTION_RESTART` | 系统 | 重启设备 (延迟500ms) |
| 8 | `ACTION_FACTORY_RESET` | 系统 | 恢复出厂设置 |
| 9 | `ACTION_NTP_SYNC` | 系统 | NTP 时间同步 |
| 10 | `ACTION_OTA_UPDATE` | 系统 | OTA 固件更新 (预留) |
| 13 | `ACTION_MODBUS_WRITE` | 通信 | Modbus 写入 (FC05/FC06) |
| 14 | `ACTION_MQTT_PUBLISH` | 通信 | MQTT 发布消息 |
| 15 | `ACTION_HTTP_REQUEST` | 通信 | HTTP 请求 (预留) |
| 16 | `ACTION_SCRIPT_EXEC` | 数据 | 执行脚本 |
| 17 | `ACTION_MODBUS_POLL` | 通信 | Modbus 轮询+控制 |
| 18 | `ACTION_LOG_EVENT` | 数据 | 记录事件日志 (预留) |
| 19 | `ACTION_SENSOR_READ` | 数据 | 读取传感器数据 |

#### ExecOperator - 条件运算符

| 值 | 名称 | 说明 | 数据类型 |
|----|------|------|----------|
| 0 | `OP_EQ` | 等于 | 数值/字符串 |
| 1 | `OP_NEQ` | 不等于 | 数值/字符串 |
| 2 | `OP_GT` | 大于 | 数值 |
| 3 | `OP_LT` | 小于 | 数值 |
| 4 | `OP_GTE` | 大于等于 | 数值 |
| 5 | `OP_LTE` | 小于等于 | 数值 |
| 6 | `OP_BETWEEN` | 区间内 | 数值 (compareValue="min,max") |
| 7 | `OP_NOT_BETWEEN` | 区间外 | 数值 (compareValue="min,max") |
| 8 | `OP_CONTAIN` | 包含子串 | 字符串 |
| 9 | `OP_NOT_CONTAIN` | 不包含子串 | 字符串 |

### 2.2 核心数据结构

#### ExecTrigger - 触发器

```cpp
struct ExecTrigger {
    int triggerType;        // ExecTriggerType 枚举值
    String triggerPeriphId; // 关联外设ID (PLATFORM/POLL触发) 或空
    int operatorType;       // ExecOperator 枚举值
    String compareValue;    // 比较值 (数值字符串/区间/子串)
    int timerMode;          // 0=间隔模式, 1=每日定点模式
    int intervalSec;        // 间隔秒数 (timerMode=0)
    String timePoint;       // 定点时间 "HH:MM" (timerMode=1)
    String eventId;         // 事件ID (EVENT_TRIGGER 专用)
    int pollResponseTimeout;  // 轮询响应超时 ms (POLL触发)
    int pollMaxRetries;       // 轮询最大重试次数
    int pollInterPollDelay;   // 轮询间隔延迟 ms
    unsigned long lastTriggerTime; // 运行时: 上次触发时间戳
    int triggerCount;              // 运行时: 累计触发次数
};
```

#### ExecAction - 动作

```cpp
struct ExecAction {
    String targetPeriphId;   // 目标外设ID
    int actionType;          // ExecActionType 枚举值
    String actionValue;      // 动作参数值
    bool useReceivedValue;   // true=使用触发时接收到的值替代actionValue
    int syncDelayMs;         // 执行后延迟 ms (用于动作间间隔, 最大10000)
};
```

#### PeriphExecRule - 规则

```cpp
struct PeriphExecRule {
    String id;               // 唯一ID "exec_<millis>"
    String name;             // 规则名称
    bool enabled;            // 是否启用
    int execMode;            // 0=异步优先, 1=强制同步
    std::vector<ExecTrigger> triggers;  // 触发器列表 (OR关系, 最多3个)
    std::vector<ExecAction> actions;    // 动作列表 (顺序执行, 最多4个)
    int protocolType;        // 数据转换协议类型
    String scriptContent;    // 脚本内容
    bool reportAfterExec;    // 执行后是否上报设备数据
};
```

### 2.3 事件系统

系统预定义了 **37 个静态事件** (`STATIC_EVENTS[]`)，分为以下类别：

| 分类 | 事件ID范围 | 示例事件 |
|------|-----------|----------|
| WiFi | 1-3 | CONNECTED, DISCONNECTED, RECONNECTING |
| MQTT | 10-13 | CONNECTED, DISCONNECTED, RECONNECTING, MESSAGE_RECEIVED |
| Network | 20-22 | IP_OBTAINED, DNS_RESOLVED, SIGNAL_CHANGED |
| Protocol | 30-34 | MODBUS_OK/ERROR/TIMEOUT, TCP_CONNECTED/DISCONNECTED |
| System | 40-43, 70-73 | BOOT_COMPLETE, LOW_MEMORY, WATCHDOG_TRIGGERED, TIME_SYNCED, HEAP_LOW/CRITICAL, TASK_WATCHDOG, STACK_OVERFLOW |
| Provision | 50-53 | STARTED, COMPLETED, FAILED, RESET |
| Rule | 60-61 | RULE_TRIGGERED, RULE_ERROR |
| Button | 80-86 | SINGLE_CLICK, DOUBLE_CLICK, LONG_PRESS_{2,5,10}S, RELEASED, STATE_CHANGED |
| PeriphExec | 90 | EXEC_COMPLETED |
| Data | 100-101 | DATA_REPORTED, DATA_REPORT_FAILED |

---

## 3. 规则生命周期 (CRUD)

### 3.1 创建规则 (addRule)

```
请求 → 参数验证 → ID生成 → 唯一性检查 → 限制检查 → 运行时字段重置 → 入库 → 持久化
```

**详细流程：**

1. **互斥锁保护**：获取 `rulesMutex` 确保线程安全
2. **ID 生成**：若未提供 ID，自动生成 `"exec_" + String(millis())`
3. **唯一性检查**：在 `rules` map 中查找是否已存在该 ID
4. **触发器数量限制**：最多 `MAX_TRIGGERS_PER_RULE` (3) 个，超出则截断
5. **动作数量限制**：最多 `MAX_ACTIONS_PER_RULE` (4) 个，超出则截断
6. **运行时字段重置**：每个触发器的 `lastTriggerTime = 0`, `triggerCount = 0`
7. **写入 map**：`rules[rule.id] = rule`
8. **持久化**：API 层调用 `saveConfiguration()` 写入 LittleFS

### 3.2 更新规则 (updateRule)

```
请求 → ID验证 → 存在性检查 → 字段合并 → 运行时状态保留 → 替换 → 持久化
```

**关键设计 - 运行时状态保留：**

更新规则时，按索引匹配保留每个触发器的 `lastTriggerTime` 和 `triggerCount`：

```cpp
for (size_t i = 0; i < rule.triggers.size() && i < existingTriggers.size(); i++) {
    rule.triggers[i].lastTriggerTime = existingTriggers[i].lastTriggerTime;
    rule.triggers[i].triggerCount = existingTriggers[i].triggerCount;
}
```

这确保了更新规则配置后，定时器不会立即重新触发，触发计数也不会丢失。

### 3.3 删除规则 (removeRule)

```
请求 → ID验证 → 存在性检查 → 从map移除 → 持久化
```

### 3.4 启用/禁用规则 (enableRule / disableRule)

直接修改 `rule.enabled` 标志位，随后持久化。禁用的规则不会参与任何触发匹配。

### 3.5 手动执行 (runOnce)

```
请求 → ID验证 → 获取规则 → 跳过触发匹配 → 直接执行所有动作 → 触发设备状态上报
```

手动执行**不检查规则是否启用**，也**不检查触发条件**，直接执行动作列表。API 层额外触发 MQTT `publishDeviceInfo()` 进行状态上报。

---

## 4. 触发器类型详解

### 4.1 PLATFORM_TRIGGER (平台数据触发, type=0)

**触发源：** MQTT 下行消息 (平台下发指令)

**匹配流程：**

```
MQTT消息到达 → handleMqttMessage(periphId, value)
  │
  ├─ Phase 1 (持锁): 遍历所有 enabled 规则
  │   ├─ 遍历规则的每个触发器
  │   │   ├─ 触发器类型 == PLATFORM_TRIGGER?
  │   │   ├─ triggerPeriphId 匹配 periphId? (空ID则匹配任意)
  │   │   ├─ evaluateCondition(value, operatorType, compareValue)?
  │   │   └─ 防抖检查: 距上次触发 > 1000ms?
  │   │       → 匹配成功: 更新 lastTriggerTime, triggerCount++
  │   └─ 收集到匹配列表 matchedRules[]
  │
  └─ Phase 2 (释锁): 遍历 matchedRules[]
      └─ dispatchAsync(rule, receivedValue) 或 executeAllActions(rule, value)
```

**关键细节：**
- `triggerPeriphId` 为空时匹配**任意**外设的消息
- 同一规则多个 PLATFORM_TRIGGER 是 OR 关系，任一匹配即触发
- 1 秒防抖：同一触发器在 1 秒内不会重复触发

### 4.2 TIMER_TRIGGER (定时触发, type=1)

**触发源：** `checkTimers()` 函数，由主循环每秒调用

**两种定时模式：**

#### 间隔模式 (timerMode=0)

```
checkTimers() 每秒执行
  │
  ├─ 规则已启用 && 触发器类型 == TIMER_TRIGGER && timerMode == 0?
  ├─ lastTriggerTime == 0? → 立即触发 (首次运行)
  ├─ (当前时间 - lastTriggerTime) >= intervalSec * 1000?
  │   → 匹配成功: lastTriggerTime = now, triggerCount++
  └─ 收集后 Phase 2 执行动作
```

- **首次立即触发**：`lastTriggerTime == 0` 时不等待间隔，立即执行
- 最小间隔由 `intervalSec` 控制（默认 60 秒）

#### 每日定点模式 (timerMode=1)

```
checkTimers() 每秒执行
  │
  ├─ 规则已启用 && 触发器类型 == TIMER_TRIGGER && timerMode == 1?
  ├─ 解析 timePoint "HH:MM" → 目标 hour, minute
  ├─ 当前 hour:minute == 目标?
  │   ├─ 距上次触发 > 60秒? (60秒防重复窗口)
  │   │   → 匹配成功: lastTriggerTime = now, triggerCount++
  │   └─ 否则跳过 (已在本分钟内触发)
  └─ 收集后 Phase 2 执行动作
```

- **60 秒防重复**：每日定点模式使用 60 秒窗口防止在同一分钟内重复触发
- 需要 NTP 时间同步才能正常工作

### 4.3 EVENT_TRIGGER (事件触发, type=4)

**触发源：** 系统内部事件（WiFi/MQTT/按钮/协议等）

**事件触发链路：**

```
系统事件发生 (如 WiFi 连接)
  │
  ├─ triggerEvent(EventType type)
  │   ├─ 查找 STATIC_EVENTS[] 中匹配的 eventId
  │   └─ 调用 triggerEventById(eventId)
  │
  ├─ triggerEventById(eventId)
  │   ├─ Phase 1 (持锁): 遍历所有 enabled 规则
  │   │   ├─ 遍历触发器: type == EVENT_TRIGGER && eventId 匹配?
  │   │   ├─ 防抖: 距上次触发 > 1000ms?
  │   │   └─ 匹配则加入 matchedRules[]
  │   │
  │   └─ Phase 2 (释锁): 执行匹配规则的动作
  │
  └─ triggerPeriphExecEvent(eventId)  // 为外设执行规则触发的事件
      └─ 同上，用于规则完成/错误时的链式触发
```

**特殊事件处理：**
- 按钮事件使用专门的 `triggerButtonEvent()`，防抖窗口缩短为 **100ms**
- `EVENT_PERIPH_EXEC_COMPLETED` (ID=90) 在异步任务完成时自动触发，支持规则链式执行

### 4.4 POLL_TRIGGER (轮询触发, type=5)

**触发源：** 外设轮询数据回调

**匹配流程：**

```
外设轮询完成 → handlePollData(periphId, value)
  │
  ├─ Phase 1 (持锁): 遍历所有 enabled 规则
  │   ├─ 遍历触发器: type == POLL_TRIGGER?
  │   ├─ triggerPeriphId 匹配? (空ID匹配任意)
  │   ├─ evaluateCondition(value, operatorType, compareValue)?
  │   ├─ 防抖: 距上次触发 > 1000ms?
  │   └─ 匹配则更新计数并加入列表
  │
  └─ Phase 2 (释锁): 执行匹配规则的动作
```

**轮询触发特有参数：**
- `pollResponseTimeout`：轮询响应超时 (默认 1000ms)
- `pollMaxRetries`：最大重试次数 (默认 2)
- `pollInterPollDelay`：轮询间隔延迟 (默认 100ms)

**定时轮询启动：**

`checkTimers()` 也会检查 POLL_TRIGGER 类型的触发器，按 `intervalSec` 间隔主动发起轮询请求，轮询完成后通过回调进入 `handlePollData()` 流程。

---

## 5. 动作类型详解

### 5.1 GPIO 动作 (type 0-6)

通过 `executePeripheralAction()` 执行，调用 `PeripheralManager` 操作硬件引脚：

| 动作 | 实现方式 | actionValue |
|------|----------|-------------|
| ACTION_HIGH (0) | `pm.setPeripheralState(id, true)` | 忽略 |
| ACTION_LOW (1) | `pm.setPeripheralState(id, false)` | 忽略 |
| ACTION_BLINK (2) | `pm.blinkPeripheral(id, times)` | 闪烁次数 |
| ACTION_BREATHE (3) | `pm.breathePeripheral(id)` | 忽略 |
| ACTION_PWM (4) | `pm.setPWMValue(id, value)` | 占空比 (0-255) |
| ACTION_DAC (5) | `pm.setDACValue(id, value)` | DAC值 |
| ACTION_INVERTED (6) | `pm.invertPeripheral(id)` | 忽略 |

**执行后操作：** 记录动作结果 `{id, value, remark}` 用于后续上报。

### 5.2 系统动作 (type 7-12)

通过 `executeSystemAction()` 执行：

| 动作 | 实现 | 异步策略 |
|------|------|----------|
| ACTION_RESTART (7) | `delay(500); ESP.restart()` | **强制同步** |
| ACTION_FACTORY_RESET (8) | `LittleFS格式化 + restart` | **强制同步** |
| ACTION_NTP_SYNC (9) | `configTime(gmtOffset, daylightOffset, ntpServer)` | 可异步 |
| ACTION_OTA_UPDATE (10) | 预留，未实现 | — |

**注**：旧版曾预留 `ACTION_AP_MODE`(11) 和 `ACTION_BLE_TOGGLE`(12)，因项目移除独立蓝牙配网与 AP 配网向导、统一采用 AP+STA 双模自动切换，已移除这两个枚举值。

**重要：** 系统动作 (RESTART/FACTORY_RESET) 在 `dispatchAsync()` 中会被**强制降级为同步执行**，因为异步任务在重启后无法正确完成。

### 5.3 通信动作 (type 13-15, 17)

#### ACTION_MODBUS_WRITE (13)

通过 `executeModbusAction()` 执行：

```
解析 targetPeriphId: "modbus:<index>"
  │
  ├─ 获取 ModbusRTU 接口
  ├─ actionValue 判断:
  │   ├─ 以 "FC05:" 开头 → 写线圈 (coilWrite)
  │   ├─ 以 "FC06:" 开头 → 写寄存器 (writeHreg)
  │   └─ 数值型:
  │       ├─ value 为 0/1 → FC05 写线圈
  │       └─ 其他 → FC06 写寄存器
  └─ 记录结果
```

#### ACTION_MODBUS_POLL (17)

通过 `executeModbusPollAction()` 执行，支持两种数据格式：

**JSON 格式：**
```json
{
  "poll": [0, 1],           // 轮询的从机索引列表
  "ctrl": [                 // 可选控制指令
    {"type":"relay","idx":0,"val":1},
    {"type":"pwm","idx":0,"val":128},
    {"type":"pid","idx":0,"sp":25.0}
  ]
}
```

**旧版逗号分隔格式：**
```
0,1,2   // 轮询的从机索引列表
```

#### ACTION_MQTT_PUBLISH (14)

```
解析 actionValue 为 topic (或使用默认设备 topic)
  └─ mqtt->publish(topic, value)
```

#### ACTION_HTTP_REQUEST (15)

预留，未实现。

### 5.4 数据动作 (type 16, 18, 19)

#### ACTION_SCRIPT_EXEC (16)

通过 `executeScriptAction()` 执行：

```
获取 ScriptEngine 实例
  ├─ parse(scriptContent) → 语法检查
  ├─ validate() → 语义检查
  └─ execute(receivedValue) → 执行脚本
```

脚本使用 `protocolType` 和 `scriptContent` 字段，支持数据转换逻辑。

#### ACTION_SENSOR_READ (19)

通过 `executeSensorReadAction()` 执行，actionValue 为 JSON 配置：

```json
{
  "periphId": "sensor_01",
  "category": "temperature",
  "scaleFactor": 1.0,
  "offset": 0.0,
  "decimals": 2
}
```

执行流程：
```
解析 JSON 配置 → 读取外设原始值 → 应用缩放: value * scaleFactor + offset
  → 按 decimals 格式化 → 记录结果
```

#### ACTION_LOG_EVENT (18)

预留，未实现。

---

## 6. 触发-动作执行流程

### 6.1 两阶段锁模式 (Two-Phase Lock Pattern)

这是 PeriphExec 最核心的设计模式，所有触发入口 (`handleMqttMessage`, `handlePollData`, `triggerEvent`, `triggerButtonEvent`) 都采用此模式：

```
Phase 1 - 持锁阶段 (规则匹配)
┌──────────────────────────────────────┐
│  RecursiveMutexGuard lock(mutex)     │
│                                      │
│  for (rule : rules) {                │
│    if (!rule.enabled) continue;      │
│    for (trigger : rule.triggers) {   │
│      if (matchCondition()) {         │
│        matchedRules.push_back(rule); │
│        break; // OR关系,一个匹配即可  │
│      }                               │
│    }                                 │
│  }                                   │
│                                      │
│  // 自动释放锁                        │
└──────────────────────────────────────┘
                │
                ▼
Phase 2 - 释锁阶段 (动作执行)
┌──────────────────────────────────────┐
│  // 此时无锁，动作执行可能耗时较长     │
│                                      │
│  for (rule : matchedRules) {         │
│    dispatchAsync(rule, value);       │
│  }                                   │
└──────────────────────────────────────┘
```

**设计目的：** 持锁期间只做轻量的条件匹配，动作执行在释锁后进行，避免长时间持锁导致其他线程阻塞。

### 6.2 条件评估 (evaluateCondition)

```cpp
bool evaluateCondition(String& receivedValue, int operatorType, String& compareValue)
```

**评估逻辑：**

```
receivedValue 和 compareValue
  │
  ├─ CONTAIN (8): receivedValue.indexOf(compareValue) >= 0
  ├─ NOT_CONTAIN (9): receivedValue.indexOf(compareValue) < 0
  │
  ├─ 转换为 float: recvFloat, compFloat
  │
  ├─ EQ (0): recvFloat == compFloat
  ├─ NEQ (1): recvFloat != compFloat
  ├─ GT (2): recvFloat > compFloat
  ├─ LT (3): recvFloat < compFloat
  ├─ GTE (4): recvFloat >= compFloat
  ├─ LTE (5): recvFloat <= compFloat
  │
  ├─ BETWEEN (6): 解析 "min,max" → recvFloat >= min && recvFloat <= max
  └─ NOT_BETWEEN (7): 解析 "min,max" → recvFloat < min || recvFloat > max
```

### 6.3 动作顺序执行 (executeAllActions)

```
executeAllActions(rule, receivedValue)
  │
  for (action : rule.actions) {
  │
  ├─ useReceivedValue == true?
  │   └─ 将 actionValue 替换为 receivedValue (数据透传)
  │
  ├─ 根据 actionType 分发到对应执行函数:
  │   ├─ 0-6: executePeripheralAction()  // GPIO
  │   ├─ 7-12: executeSystemAction()     // 系统
  │   ├─ 13: executeModbusAction()       // Modbus写
  │   ├─ 14: MQTT publish                // MQTT发布
  │   ├─ 16: executeScriptAction()       // 脚本
  │   ├─ 17: executeModbusPollAction()   // Modbus轮询
  │   └─ 19: executeSensorReadAction()   // 传感器读取
  │
  ├─ syncDelayMs > 0? (最大 10000ms)
  │   └─ delay(syncDelayMs)  // 动作间延迟
  │
  }  // 下一个动作
  │
  ├─ reportAfterExec == true?
  │   └─ reportActionResults() → 上报执行结果
  │
  └─ triggerPeriphExecEvent(EVENT_PERIPH_EXEC_COMPLETED)
      └─ 触发链式规则
```

### 6.4 完整规则执行生命周期

```
规则创建/更新
  └─ addRule() / updateRule() → saveConfiguration() → LittleFS 持久化
      │
      ▼
设备运行期间
  │
  ├─ MQTT消息到达 → handleMqttMessage()
  │   └─ PLATFORM_TRIGGER 匹配 → 条件评估 → 防抖 → 动作执行
  │
  ├─ 每秒 checkTimers()
  │   ├─ TIMER_TRIGGER: 间隔/定点检查 → 动作执行
  │   └─ POLL_TRIGGER: 按间隔发起轮询
  │
  ├─ 外设数据回调 → handlePollData()
  │   └─ POLL_TRIGGER 匹配 → 条件评估 → 防抖 → 动作执行
  │
  ├─ 系统事件 → triggerEvent()
  │   └─ EVENT_TRIGGER 匹配 → 防抖 → 动作执行
  │
  └─ 按钮状态机 → triggerButtonEvent()
      └─ EVENT_TRIGGER 匹配 → 100ms防抖 → 动作执行
          │
          ▼
动作执行链
  ├─ 同步执行: 直接在调用线程中顺序执行
  └─ 异步执行: FreeRTOS 新建任务在 Core 1 上执行
      │
      ▼
执行后处理
  ├─ reportAfterExec → 上报动作结果 (MQTT)
  ├─ tryReportDeviceData → 上报设备整体状态 (多协议)
  └─ 触发 EVENT_PERIPH_EXEC_COMPLETED → 可能引发链式规则
```

---

## 7. 核心方法分析

### 7.1 handleDataCommand()

**位置：** `PeriphExecManager.cpp:500-638`

**功能：** 处理平台下发的数据命令，同步执行匹配规则并构建响应。

**与 handleMqttMessage 的区别：**
- `handleMqttMessage` 用于一般的 MQTT 数据，异步执行
- `handleDataCommand` 用于需要**同步响应**的命令场景，调用方需要立即获取执行结果

**详细流程：**

```
handleDataCommand(items[], itemCount, response)
  │
  ├─ Step 1: 对 "modbus_read" 类型数据进行预处理
  │   ├─ 解析外设注册表中的 modbus 节点
  │   ├─ 将原始 modbus 寄存器值映射到业务外设ID
  │   └─ 展开为独立的 (periphId, value) 对
  │
  ├─ Step 2: 遍历所有数据项
  │   ├─ 持锁: 遍历 enabled 规则的 PLATFORM_TRIGGER
  │   │   ├─ triggerPeriphId 匹配当前 periphId?
  │   │   ├─ evaluateCondition(value)?
  │   │   └─ 匹配 → 同步执行 executeAllActions()
  │   │
  │   ├─ 释锁后: 跟踪哪些数据项被规则消费
  │   └─ 未匹配的数据项保留为 "unmatched"
  │
  └─ Step 3: 构建响应 JSON
      ├─ 已执行的动作结果
      └─ 未匹配的数据项 (原样返回给调用方)
```

**关键特点：**
- **同步执行**：不使用 `dispatchAsync()`，直接调用 `executeAllActions()`
- **Modbus 预处理**：将底层 modbus 寄存器地址自动映射为高层外设 ID
- **未匹配跟踪**：记录哪些下发数据没有被任何规则处理

### 7.2 checkTimers()

**位置：** `PeriphExecManager.cpp:685-750`

**功能：** 定时触发检查，由主循环每秒调用一次。

**详细流程：**

```
checkTimers()  // 每秒调用
  │
  ├─ Phase 1 (持锁):
  │   for (rule : rules) {
  │     if (!rule.enabled) continue;
  │     for (trigger : rule.triggers) {
  │       │
  │       ├─ TIMER_TRIGGER (type=1):
  │       │   ├─ timerMode == 0 (间隔):
  │       │   │   ├─ lastTriggerTime == 0 → 立即匹配 (首次)
  │       │   │   └─ now - lastTriggerTime >= intervalSec * 1000 → 匹配
  │       │   │
  │       │   └─ timerMode == 1 (每日定点):
  │       │       ├─ 解析 timePoint "HH:MM"
  │       │       ├─ 当前时刻 == 目标时刻?
  │       │       └─ now - lastTriggerTime > 60000 → 匹配 (60s防重复)
  │       │
  │       └─ POLL_TRIGGER (type=5):
  │           └─ now - lastTriggerTime >= intervalSec * 1000
  │               → 标记需要发起轮询 (不直接执行动作)
  │     }
  │   }
  │
  └─ Phase 2 (释锁):
      ├─ TIMER_TRIGGER 匹配 → dispatchAsync() / executeAllActions()
      └─ POLL_TRIGGER 到期 → 发起外设轮询请求 (结果通过回调进入 handlePollData)
```

### 7.3 executeAllActions() (核心执行器)

**位置：** `PeriphExecManager.cpp:771-815`

**功能：** 按顺序执行规则的所有动作。

```
executeAllActions(rule, receivedValue)
  │
  ├─ 初始化结果收集器 actionResults[]
  │
  ├─ for (i = 0; i < rule.actions.size(); i++) {
  │   │
  │   ├─ action = rule.actions[i]
  │   │
  │   ├─ useReceivedValue?
  │   │   └─ effectiveValue = receivedValue  (数据透传模式)
  │   │   └─ else: effectiveValue = action.actionValue
  │   │
  │   ├─ switch (action.actionType):
  │   │   ├─ 0-6 → executePeripheralAction(action, effectiveValue, results)
  │   │   ├─ 7-12 → executeSystemAction(action, effectiveValue)
  │   │   ├─ 13 → executeModbusAction(action, effectiveValue, results)
  │   │   ├─ 14 → MQTT publish
  │   │   ├─ 16 → executeScriptAction(rule, effectiveValue)
  │   │   ├─ 17 → executeModbusPollAction(action, effectiveValue, results)
  │   │   └─ 19 → executeSensorReadAction(action, effectiveValue, results)
  │   │
  │   └─ syncDelayMs > 0 && syncDelayMs <= 10000?
  │       └─ delay(min(syncDelayMs, 10000))
  │   }
  │
  ├─ rule.reportAfterExec && results 不为空?
  │   └─ reportActionResults(results)  → MQTT 上报
  │
  └─ triggerPeriphExecEvent(EVENT_PERIPH_EXEC_COMPLETED)
```

### 7.4 evaluateCondition()

**位置：** `PeriphExecManager.cpp:648-681`

**功能：** 评估触发条件是否满足。

**算法：**

```
evaluateCondition(receivedValue, operatorType, compareValue)
  │
  ├─ OP_CONTAIN (8):
  │   return receivedValue.indexOf(compareValue) >= 0
  │
  ├─ OP_NOT_CONTAIN (9):
  │   return receivedValue.indexOf(compareValue) < 0
  │
  ├─ 数值转换: recv = receivedValue.toFloat(), comp = compareValue.toFloat()
  │
  ├─ OP_EQ (0):  return recv == comp
  ├─ OP_NEQ (1): return recv != comp
  ├─ OP_GT (2):  return recv > comp
  ├─ OP_LT (3):  return recv < comp
  ├─ OP_GTE (4): return recv >= comp
  ├─ OP_LTE (5): return recv <= comp
  │
  ├─ OP_BETWEEN (6):
  │   解析 compareValue 按逗号分割为 min, max
  │   return recv >= min && recv <= max
  │
  └─ OP_NOT_BETWEEN (7):
      解析 compareValue 按逗号分割为 min, max
      return recv < min || recv > max
```

**注意事项：**
- 数值比较使用 `float`，存在浮点精度问题（如 EQ 比较）
- CONTAIN/NOT_CONTAIN 操作直接使用字符串的 `indexOf`
- BETWEEN 使用逗号分隔的格式 `"min,max"`

---

## 8. 异步执行引擎

### 8.1 调度决策 (dispatchAsync)

**位置：** `PeriphExecManager.cpp:1270-1354`

```
dispatchAsync(rule, receivedValue)
  │
  ├─ 检查 1: 包含系统动作 (RESTART/FACTORY_RESET)?
  │   └─ YES → 强制同步执行 (异步任务会在重启时被终止)
  │
  ├─ 检查 2: 用户配置 execMode == 1 (强制同步)?
  │   └─ YES → 同步执行
  │
  ├─ 检查 3: 可用堆内存 < MIN_HEAP_FOR_ASYNC (30000)?
  │   └─ YES → 降级为同步执行 (内存不足以创建任务)
  │
  ├─ 检查 4: 信号量 taskSlotSemaphore 可用? (最大3个并发)
  │   └─ NO → 降级为同步执行 (任务槽已满)
  │
  └─ 所有检查通过:
      ├─ 创建 AsyncExecContext (deep copy rule + value)
      ├─ 计算栈大小: 包含脚本? SCRIPT_TASK_STACK(8192) : SIMPLE_TASK_STACK(4096)
      └─ xTaskCreatePinnedToCore(asyncExecTaskFunc, ..., Core 1)
```

### 8.2 异步任务生命周期

```
asyncExecTaskFunc(context)  // 运行在 FreeRTOS 任务中 (Core 1)
  │
  ├─ 执行所有动作: executeAllActions(context.ruleCopy, context.receivedValue)
  │
  ├─ 记录执行结果: recordResult(ruleId, success, ...)
  │   └─ 存入最近结果列表 (用于查询)
  │
  ├─ 触发完成事件: triggerPeriphExecEvent(EVENT_PERIPH_EXEC_COMPLETED)
  │   └─ 可能触发其他规则的 EVENT_TRIGGER
  │
  ├─ 释放任务槽: xSemaphoreGive(taskSlotSemaphore)
  │
  └─ 删除任务: vTaskDelete(NULL)
```

### 8.3 RAII 锁保护

项目使用 RAII 模式管理 FreeRTOS 互斥锁：

```cpp
class MutexGuard {
    SemaphoreHandle_t _mutex;
    bool _locked;
public:
    explicit MutexGuard(SemaphoreHandle_t m, TickType_t timeout = portMAX_DELAY);
    ~MutexGuard();  // 析构时自动释放
    bool locked() const;
};

class RecursiveMutexGuard {
    SemaphoreHandle_t _mutex;
    bool _locked;
public:
    explicit RecursiveMutexGuard(SemaphoreHandle_t m, TickType_t timeout = portMAX_DELAY);
    ~RecursiveMutexGuard();
    bool locked() const;
};
```

使用递归互斥锁是因为某些调用路径可能嵌套（如规则执行触发事件，事件触发其他规则）。

---

## 9. 数据转换管道与上报控制

### 9.1 数据转换管道

```
外设原始数据
  │
  ├─ useReceivedValue == true?
  │   └─ 直接将触发数据传递给动作 (数据透传)
  │
  ├─ ACTION_SENSOR_READ:
  │   └─ rawValue → value * scaleFactor + offset → 格式化(decimals)
  │
  ├─ ACTION_SCRIPT_EXEC:
  │   └─ ScriptEngine 执行自定义数据转换脚本
  │       rule.protocolType → 选择协议解析方式
  │       rule.scriptContent → 转换脚本内容
  │
  └─ Modbus 数据预处理 (handleDataCommand):
      └─ 寄存器地址 → 外设ID 映射 → 业务值
```

### 9.2 执行结果上报 (reportActionResults)

**位置：** `PeriphExecManager.cpp:1609-1638`

```
reportActionResults(results[])
  │
  ├─ 构建 JSON 数组:
  │   [{
  │     "id": "periph_01",       // 外设ID
  │     "value": "1",            // 执行结果值
  │     "remark": "GPIO HIGH"    // 执行备注
  │   }, ...]
  │
  └─ 通过 MQTT 发布到设备上报 topic
```

### 9.3 设备状态上报 (tryReportDeviceData)

**位置：** `PeriphExecManager.cpp:1758-1839`

```
tryReportDeviceData()
  │
  ├─ 检查可用协议:
  │   ├─ MQTT 已连接?
  │   ├─ TCP 已连接?
  │   ├─ HTTP 可用?
  │   └─ CoAP 可用?
  │
  ├─ 收集设备数据: collectPeripheralData()
  │   ├─ 遍历所有已启用的 GPIO 外设
  │   └─ 获取当前状态 (HIGH/LOW/PWM值等)
  │
  └─ 按优先级尝试上报:
      MQTT (优先) → TCP → HTTP → CoAP (降级)
```

**协议降级链：** 当高优先级协议不可用时，自动尝试下一个协议，确保数据尽可能上报。

### 9.4 数据收集 (collectPeripheralData)

**位置：** `PeriphExecManager.cpp:1649-1694`

```
collectPeripheralData()
  │
  ├─ 获取 PeripheralManager 实例
  ├─ 遍历所有已注册外设
  │   ├─ 外设已启用?
  │   ├─ 外设类型为 GPIO?
  │   └─ 读取当前状态 → 加入数据列表
  │
  └─ 返回 [{periphId, value}] 列表
```

---

## 10. 按钮事件子系统

### 10.1 按钮状态机

**位置：** `PeriphExecManager.cpp:1843-1961`

按钮事件通过硬件轮询和状态机实现，不依赖中断。

**状态机配置：**

```cpp
struct ButtonEventConfig {
    String periphId;           // 关联的GPIO外设ID
    uint8_t pin;               // GPIO引脚号
    bool activeLow;            // 是否低电平有效
    unsigned long debounceMs;  // 消抖时间 (默认20ms)
};

struct ButtonRuntimeState {
    bool lastStableState;      // 上次稳定状态
    bool lastRawState;         // 上次原始读取值
    unsigned long lastChangeTime;   // 上次状态变化时间
    unsigned long pressStartTime;   // 按下开始时间
    uint8_t clickCount;        // 连击计数
    unsigned long lastClickTime;    // 上次点击时间
    bool longPress2sTriggered; // 2秒长按已触发
    bool longPress5sTriggered; // 5秒长按已触发
    bool longPress10sTriggered; // 10秒长按已触发
};
```

**状态机流程 (checkButtonEvents, 每20ms调用)：**

```
读取GPIO引脚状态 (digitalRead)
  │
  ├─ 状态有变化?
  │   └─ 重置消抖计时器 lastChangeTime = now
  │
  ├─ 消抖完成? (now - lastChangeTime >= debounceMs)
  │   │
  │   ├─ 从 未按下 → 按下:
  │   │   ├─ pressStartTime = now
  │   │   ├─ 重置长按标志
  │   │   └─ 触发 EVENT_BUTTON_STATE_CHANGED (86)
  │   │
  │   ├─ 保持按下中:
  │   │   ├─ 按下时长 >= 2s && !longPress2sTriggered?
  │   │   │   └─ 触发 EVENT_BUTTON_LONG_PRESS_2S (83)
  │   │   ├─ 按下时长 >= 5s && !longPress5sTriggered?
  │   │   │   └─ 触发 EVENT_BUTTON_LONG_PRESS_5S (84)
  │   │   └─ 按下时长 >= 10s && !longPress10sTriggered?
  │   │       └─ 触发 EVENT_BUTTON_LONG_PRESS_10S (85)
  │   │
  │   └─ 从 按下 → 松开:
  │       ├─ 触发 EVENT_BUTTON_RELEASED (85)
  │       ├─ 触发 EVENT_BUTTON_STATE_CHANGED (86)
  │       ├─ clickCount++
  │       ├─ lastClickTime = now
  │       └─ 延迟判断:
  │           ├─ now - lastClickTime > 300ms? (双击超时)
  │           │   ├─ clickCount == 1 → 触发 EVENT_BUTTON_SINGLE_CLICK (80)
  │           │   └─ clickCount >= 2 → 触发 EVENT_BUTTON_DOUBLE_CLICK (81)
  │           └─ 等待下一次点击...
```

### 10.2 按钮事件防抖

按钮事件触发规则时使用 **100ms 防抖**（区别于一般事件的 1000ms），因为按钮操作通常需要更快的响应。

---

## 11. 配置持久化与版本迁移

### 11.1 存储格式 (v3)

**位置：** `PeriphExecManager.cpp:140-203`

```json
{
  "version": 3,
  "rules": [
    {
      "id": "exec_12345",
      "name": "温度告警",
      "enabled": true,
      "execMode": 0,
      "protocolType": 0,
      "scriptContent": "",
      "reportAfterExec": true,
      "triggers": [
        {
          "triggerType": 0,
          "triggerPeriphId": "temp_01",
          "operatorType": 2,
          "compareValue": "35",
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
          "targetPeriphId": "relay_01",
          "actionType": 0,
          "actionValue": "",
          "useReceivedValue": false,
          "syncDelayMs": 0
        }
      ]
    }
  ]
}
```

### 11.2 版本迁移

**loadConfiguration()** 支持三个版本的配置格式：

#### v1 → v3 迁移

v1 格式特征：无 `version` 字段，使用扁平结构

```
v1 扁平字段:
  triggerType, triggerPeriphId, operatorType, compareValue,
  timerMode, intervalSec, timePoint, eventId,
  targetPeriphId, actionType, actionValue, useReceivedValue, syncDelayMs
  
  → 转换为 triggers[单元素] + actions[单元素]
```

额外迁移处理：
- `inverted` 字段 → `ACTION_INVERTED` 动作
- 旧 `eventId` 格式迁移到新的事件编号系统

#### v2 → v3 迁移

v2 格式特征：`version: 2`，同样使用扁平结构

```
v2 同 v1 的扁平字段 → 转换为 triggers[]/actions[] 数组
```

#### v3 原生加载

直接解析 `triggers[]` 和 `actions[]` JSON 数组。

### 11.3 持久化时机

以下操作会触发 `saveConfiguration()`：
- `addRule()` → API 层调用
- `updateRule()` → API 层调用
- `removeRule()` → API 层调用
- `enableRule()` / `disableRule()` → API 层调用

**注意：** 运行时状态 (`lastTriggerTime`, `triggerCount`) **不会**持久化到配置文件，设备重启后这些值重置为 0。

---

## 12. API 路由层

### 12.1 路由注册

**位置：** `PeriphExecRouteHandler.cpp:14-72`

| 方法 | 路径 | 处理函数 | 权限 |
|------|------|----------|------|
| GET | `/api/periph-exec` | handleGetRules | system.view |
| POST | `/api/periph-exec` (JSON) | handleAddRuleJson | config.edit |
| POST | `/api/periph-exec` (form) | handleAddRule | config.edit |
| POST | `/api/periph-exec/update` (JSON) | handleUpdateRuleJson | config.edit |
| POST | `/api/periph-exec/update` (form) | handleUpdateRule | config.edit |
| DELETE | `/api/periph-exec/` | handleDeleteRule | config.edit |
| POST | `/api/periph-exec/enable` | handleEnableRule | config.edit |
| POST | `/api/periph-exec/disable` | handleDisableRule | config.edit |
| POST | `/api/periph-exec/run` | handleRunOnce | config.edit |
| GET | `/api/periph-exec/events/static` | handleGetStaticEvents | system.view |
| GET | `/api/periph-exec/events/dynamic` | handleGetDynamicEvents | system.view |
| GET | `/api/periph-exec/events/categories` | handleGetEventCategories | system.view |
| GET | `/api/periph-exec/trigger-types` | handleGetTriggerTypes | system.view |

### 12.2 路由注册顺序

路由注册顺序非常重要，因为 `AsyncCallbackJsonWebHandler` 使用**前缀匹配**：

1. **先注册具体路径**：`/events/static`, `/events/dynamic`, `/events/categories`, `/trigger-types`
2. **JSON handler 先注册 update**：`/api/periph-exec/update` 必须在 `/api/periph-exec` 之前
3. **最后注册通用路径**：`/api/periph-exec` (GET/POST)

如果顺序颠倒，`/api/periph-exec/update` 的 POST 请求会被 `/api/periph-exec` 的 POST handler 截获。

### 12.3 双格式支持

每个写入操作同时支持两种请求格式：

| 格式 | Content-Type | Handler | 说明 |
|------|-------------|---------|------|
| JSON | application/json | handleAddRuleJson / handleUpdateRuleJson | 支持完整的 triggers[]/actions[] 数组 |
| Form | application/x-www-form-urlencoded | handleAddRule / handleUpdateRule | 向后兼容，只支持单个触发器/动作 |

JSON handler 通过 `AsyncCallbackJsonWebHandler` 注册，优先级高于 form handler。

### 12.4 JSON 解析辅助

`parseRuleFromJson()` 统一处理 JSON 到 PeriphExecRule 的解析，包括：
- 字符串安全提取：使用 `| ""` 防止 `as<String>()` 返回 `"null"` 字符串
- 整数兼容解析：`jsonInt()` 同时支持 JSON 数值和字符串格式
- 运行时字段重置：解析时 `lastTriggerTime = 0`, `triggerCount = 0`

### 12.5 响应格式

**成功响应：**
```json
{"success": true, "message": "Rule added"}
```

**列表响应：**
```json
{
  "success": true,
  "data": [
    {
      "id": "exec_12345",
      "name": "...",
      "triggers": [{...}],
      "actions": [{...}],
      "triggerPeriphName": "温度传感器",  // 关联外设名称 (额外字段)
      "targetPeriphName": "继电器",       // 关联外设名称 (额外字段)
      "targetPeriphType": 1               // 外设类型 (额外字段)
    }
  ]
}
```

**错误响应：**
```json
{"success": false, "message": "Rule not found"}
```

---

## 13. 已知问题与优化建议

### 13.1 浮点精度问题

**位置：** `evaluateCondition()` (PeriphExecManager.cpp:648-681)

**问题：** 使用 `float` 进行 `OP_EQ` 比较时，浮点精度可能导致误判。例如 `0.1 + 0.2 != 0.3`。

**建议：** 对于 EQ/NEQ 操作，增加 epsilon 容差比较：
```cpp
bool floatEq(float a, float b, float eps = 0.001) {
    return fabs(a - b) < eps;
}
```

### 13.2 防抖时间硬编码

**问题：** 事件防抖时间在代码中硬编码：
- 一般事件: 1000ms (`handleMqttMessage`, `handlePollData`, `triggerEvent`)
- 按钮事件: 100ms (`triggerButtonEvent`)
- 每日定时: 60000ms (`checkTimers` 每日定点模式)

**建议：** 将防抖时间提取为可配置参数，或在触发器结构中增加 `debounceMs` 字段，允许每个触发器独立设置。

### 13.3 TIMER_TRIGGER 首次立即触发

**问题：** 间隔模式下 `lastTriggerTime == 0` 导致设备重启后立即触发所有定时规则，这在某些场景下可能不是期望行为（如重启后立即触发告警）。

**建议：** 增加配置项 `skipFirstTrigger`，允许用户选择重启后是否跳过首次触发。或者在 `addRule()` 时将 `lastTriggerTime` 初始化为 `millis()`。

### 13.4 异步任务失败无重试

**问题：** `asyncExecTaskFunc` 中如果动作执行失败，只记录结果和触发完成事件，没有重试机制。

**建议：** 对于通信类动作 (Modbus/MQTT/HTTP)，增加可配置的重试次数和退避策略。

### 13.5 规则数量无上限

**问题：** `rules` map 没有设置最大容量限制。在 ESP32 的有限内存下，大量规则可能导致内存耗尽。

**建议：** 增加 `MAX_RULES` 限制（如 20-50），在 `addRule()` 中检查。

### 13.6 配置保存频率

**问题：** 每次 CRUD 操作都会触发 `saveConfiguration()`，对 LittleFS 闪存进行写入。频繁的写入操作会缩短闪存寿命。

**建议：** 实现延迟写入（dirty flag + 定时保存），或在批量操作时合并保存。

### 13.7 两阶段锁的时序风险

**问题：** Phase 1 收集匹配规则后释放锁，Phase 2 执行时规则可能已被删除或修改。当前通过复制规则数据缓解，但在高频触发场景下仍有边界情况。

**现状：** 代码已通过在 Phase 1 中复制规则数据到匹配列表来解决此问题，但对于非异步路径中的 `executeAllActions` 调用，仍然使用了规则引用，存在潜在的竞态条件。

### 13.8 系统动作的安全性

**问题：** `ACTION_FACTORY_RESET` 可通过规则自动触发，如果条件配置不当（如事件链循环触发），可能导致意外的出厂重置。

**建议：** 对破坏性系统动作 (RESTART/FACTORY_RESET) 增加二次确认机制或执行次数限制。

### 13.9 按钮双击检测的延迟

**问题：** 双击判定需要等待 300ms 超时后才能确定是单击还是双击，这意味着单击响应永远有 300ms 延迟。

**建议：** 可提供配置项让用户在"单击响应速度"和"双击支持"之间选择。不需要双击的场景可禁用双击检测以获得更快的单击响应。

### 13.10 未实现的预留功能

以下动作类型已定义枚举值但未实现：
- `ACTION_OTA_UPDATE` (10)
- `ACTION_HTTP_REQUEST` (15)
- `ACTION_LOG_EVENT` (18)

这些在 `executeAllActions` 的 switch 中没有对应的 case，执行时会静默跳过。

**建议：** 对未实现的动作类型，至少输出日志警告，避免用户配置后无反馈。

---

## 附录 A: 源码文件索引

| 文件 | 行数 | 功能 |
|------|------|------|
| `include/core/PeripheralExecution.h` | ~200 | 枚举定义、数据结构、事件常量 |
| `include/core/AsyncExecTypes.h` | ~60 | FreeRTOS 异步执行类型、RAII 锁 |
| `include/core/PeriphExecManager.h` | ~120 | 管理器类接口声明 |
| `src/core/PeriphExecManager.cpp` | 2162 | 完整业务逻辑实现 |
| `include/network/handlers/PeriphExecRouteHandler.h` | ~45 | API 路由处理器声明 |
| `src/network/handlers/PeriphExecRouteHandler.cpp` | 566 | API 路由处理实现 |

## 附录 B: 关键方法速查表

| 方法 | 文件位置 (行号) | 功能 |
|------|-----------------|------|
| `initialize()` | PeriphExecManager.cpp:15-25 | 初始化互斥锁和信号量，加载配置 |
| `addRule()` | PeriphExecManager.cpp:29-58 | 创建新规则 |
| `updateRule()` | PeriphExecManager.cpp:60-89 | 更新规则(保留运行时状态) |
| `saveConfiguration()` | PeriphExecManager.cpp:140-203 | v3 格式持久化到 LittleFS |
| `loadConfiguration()` | PeriphExecManager.cpp:205-349 | 加载配置(支持 v1/v2/v3) |
| `handleMqttMessage()` | PeriphExecManager.cpp:353-422 | MQTT 消息触发 (PLATFORM_TRIGGER) |
| `handlePollData()` | PeriphExecManager.cpp:426-496 | 轮询数据触发 (POLL_TRIGGER) |
| `handleDataCommand()` | PeriphExecManager.cpp:500-638 | 同步数据命令处理 |
| `evaluateCondition()` | PeriphExecManager.cpp:648-681 | 条件表达式评估 |
| `checkTimers()` | PeriphExecManager.cpp:685-750 | 定时触发检查 (每秒) |
| `executeAllActions()` | PeriphExecManager.cpp:771-815 | 顺序执行规则动作 |
| `executePeripheralAction()` | PeriphExecManager.cpp:817-891 | GPIO 动作执行 |
| `executeModbusAction()` | PeriphExecManager.cpp:893-952 | Modbus 写入 |
| `executeModbusPollAction()` | PeriphExecManager.cpp:954-1119 | Modbus 轮询+控制 |
| `executeSensorReadAction()` | PeriphExecManager.cpp:1121-1174 | 传感器读取 |
| `executeSystemAction()` | PeriphExecManager.cpp:1176-1227 | 系统动作 |
| `executeScriptAction()` | PeriphExecManager.cpp:1231-1253 | 脚本执行 |
| `dispatchAsync()` | PeriphExecManager.cpp:1270-1354 | 异步/同步调度决策 |
| `asyncExecTaskFunc()` | PeriphExecManager.cpp:1357-1403 | FreeRTOS 异步任务函数 |
| `triggerEvent()` | PeriphExecManager.cpp:1458-1511 | 事件触发入口 |
| `triggerButtonEvent()` | PeriphExecManager.cpp:1964-2009 | 按钮事件触发 |
| `checkButtonEvents()` | PeriphExecManager.cpp:1843-1961 | 按钮状态机 (每20ms) |
| `reportActionResults()` | PeriphExecManager.cpp:1609-1638 | 上报执行结果 |
| `tryReportDeviceData()` | PeriphExecManager.cpp:1758-1839 | 设备状态上报 |
| `collectPeripheralData()` | PeriphExecManager.cpp:1649-1694 | 收集外设状态数据 |
