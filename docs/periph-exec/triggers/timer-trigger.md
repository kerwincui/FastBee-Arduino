# 定时触发 (TIMER_TRIGGER)

## 概述

定时触发（triggerType = 1）用于按固定时间间隔或每日指定时间点自动执行规则。适用于周期采集、定时控制等场景。

## 触发模式

| 模式 | timerMode | 说明 |
|------|-----------|------|
| 间隔模式 | 0 | 每隔 N 秒触发一次 |
| 每日时间点 | 1 | 每天在指定 HH:MM 时刻触发 |

## 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| triggerType | 1 | 定时触发 |
| timerMode | uint8 | 0=间隔模式, 1=每日时间点 |
| intervalSec | uint32 | 间隔秒数（timerMode=0 时生效） |
| timePoint | String | "HH:MM" 格式时间点（timerMode=1 时生效） |

## 配置示例

### 方式1：Web界面配置（推荐）

#### 示例1：间隔触发（每 60 秒）

**场景**：每60秒执行一次规则

**配置步骤**：

1. 点击左侧菜单 **外设配置** → 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 填写基础配置：
   - **规则名称**：`定时采集数据`
   - **上报数据**：✅ 启用（需要上报结果时）
   - **启用**：✅ 启用

4. 配置触发器：
   - **触发类型**：选择 **定时触发**
   - **定时模式**：选择 **固定间隔**
   - **间隔时间**：填写 `60`（60秒）

5. 配置动作：添加需要执行的动作

6. 点击 **保存** 按钮

---

#### 示例2：每日时间点触发（每天 08:00）

**场景**：每天早上8点自动执行

**配置步骤**：

1. 创建规则，名称：`定时开灯`
2. 触发器配置：
   - **触发类型**：选择 **定时触发**
   - **定时模式**：选择 **每日时间点**
   - **时间点**：填写 `08:00`（格式：HH:MM）
3. 配置动作：添加开灯相关动作
4. 点击 **保存**

> 💡 **提示**：每日时间点模式需要NTP时间同步成功，否则不会触发

---

### 方式2：JSON配置文件导入

## 完整规则示例

### 每 5 分钟采集温湿度

```json
{
  "id": "exec_dht_poll",
  "name": "定时采集温湿度",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 0,
      "intervalSec": 300,
      "timePoint": "",
      "eventId": "",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "dht1",
      "actionType": 19,
      "actionValue": "",
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

### 每天 22:00 关灯

```json
{
  "id": "exec_night_off",
  "name": "每晚自动关灯",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 1,
      "intervalSec": 60,
      "timePoint": "22:00",
      "eventId": "",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "relay1",
      "actionType": 1,
      "actionValue": "",
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

## 注意事项

1. **最小间隔**：intervalSec 最小可设为 1 秒，但建议 ≥5 秒避免频繁执行
2. **NTP 依赖**：每日时间点模式需要 NTP 同步成功（`tm_year >= 100`），否则跳过
3. **时间窗口**：每日触发在指定分钟内仅触发一次（60 秒去重窗口）
4. **首次触发**：间隔模式启动后立即触发第一次（lastTriggerTime 初始为 0）
5. **运行中保护**：若上一次执行尚未完成，本次定时触发将被跳过
