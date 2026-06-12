# 平台触发 (PLATFORM_TRIGGER)

## 概述

平台触发（triggerType = 0）用于响应 IoT 平台通过 MQTT 下发的数据命令。当平台下发的数据匹配触发条件时，执行对应规则的动作列表。

![触发器时序对比](../../images/trigger-timing-comparison.svg)

平台触发只在平台消息到达时评估条件，不会主动读取本地传感器。若需要按固定频率读取外设，请改用定时或轮询触发。

## 触发机制

1. 平台通过 MQTT 下发数据命令：`[{"id":"xxx","value":"yyy"}]`
2. 系统解析命令中的 `id` 和 `value`
3. 匹配规则中 `triggerPeriphId` == 下发数据的 `id`
4. 使用 `operatorType` 和 `compareValue` 评估条件
5. 条件满足则触发规则执行

## 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| triggerType | 0 | 平台触发 |
| triggerPeriphId | String | 数据源外设 ID（匹配下发数据中的 item.id） |
| operatorType | uint8 | 条件操作符（见下表） |
| compareValue | String | 比较值 |

## 条件操作符

| 操作符 | 值 | 说明 | 示例 |
|--------|-----|------|------|
| EQ | 0 | 等于 | value == "1" |
| NEQ | 1 | 不等于 | value != "0" |
| GT | 2 | 大于 | value > "30" |
| LT | 3 | 小于 | value < "10" |
| GTE | 4 | 大于等于 | value >= "50" |
| LTE | 5 | 小于等于 | value <= "100" |
| BETWEEN | 6 | 区间内 | "20" <= value <= "80" |
| NOT_BETWEEN | 7 | 区间外 | value < "20" 或 value > "80" |
| CONTAIN | 8 | 包含 | value 包含 "alarm" |
| NOT_CONTAIN | 9 | 不包含 | value 不包含 "ok" |

> 区间操作符的 compareValue 格式为 `"min,max"`，如 `"20,80"`

## 配置示例

### 方式1：Web界面配置（推荐）

外设执行页面如下。平台触发配置时重点核对 MQTT/平台下发内容、比较值和目标动作，避免误触发现场外设。

![外设执行规则列表](../../system/images/periph-exec-management.png)

#### 示例1：精确匹配（等于）

**场景**：当平台下发 `[{"id":"gpio_button","value":"0"}]` 时触发

**配置步骤**：

1. 点击左侧菜单 **外设配置** → 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 填写基础配置：
   - **规则名称**：`平台按钮触发`
   - **上报数据**：根据需求启用
   - **启用**：✅ 启用

4. 配置触发器：
   - **触发类型**：选择 **平台触发**
   - **目标外设ID**：填写 `gpio_button`（匹配下发数据的id）
   - **运算符**：选择 `等于 (=)`
   - **比较值**：填写 `0`

5. 配置动作（根据需求添加）

6. 点击 **保存** 按钮

**测试方法**：通过MQTT发送 `[{"id":"gpio_button","value":"0"}]`

---

#### 示例2：阈值判断（大于）

**场景**：当温度传感器上报值 > 35 时触发

**配置步骤**：

1. 创建规则，名称：`温度超标报警`
2. 触发器配置：
   - **触发类型**：选择 **平台触发**
   - **目标外设ID**：填写 `dht1`
   - **运算符**：选择 `大于 (>)`
   - **比较值**：填写 `35`
3. 配置动作：添加报警相关动作
4. 点击 **保存**

**测试方法**：通过MQTT发送 `[{"id":"dht1","value":"40"}]`

---

#### 示例3：区间判断

**场景**：当湿度值在 20~80 范围内时触发

**配置步骤**：

1. 创建规则，名称：`湿度正常范围`
2. 触发器配置：
   - **触发类型**：选择 **平台触发**
   - **目标外设ID**：填写 `humidity`
   - **运算符**：选择 `区间内 (BETWEEN)`
   - **比较值**：填写 `20,80`（格式：最小值,最大值）
3. 配置动作
4. 点击 **保存**

**测试方法**：通过MQTT发送 `[{"id":"humidity","value":"50"}]`

---

### 方式2：JSON配置文件导入

## 完整规则示例

平台下发重启命令时执行系统重启：

```json
{
  "id": "exec_platform_restart",
  "name": "平台远程重启",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 0,
      "triggerPeriphId": "sys_restart",
      "operatorType": 0,
      "compareValue": "1",
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
      "targetPeriphId": "",
      "actionType": 6,
      "actionValue": "",
      "useReceivedValue": false,
      "syncDelayMs": 500,
      "execMode": 0
    }
  ],
  "protocolType": 0,
  "scriptContent": "",
  "reportAfterExec": false
}
```

## 注意事项

1. **ID 匹配**：`triggerPeriphId` 必须与平台下发数据中的 `id` 字段精确匹配
2. **数值比较**：compareValue 会被解析为浮点数进行数值比较
3. **字符串比较**：CONTAIN/NOT_CONTAIN 为字符串子串匹配
4. **handleDataCommand**：平台触发通过 `handleDataCommand` 方法处理，同步执行并立即返回结果
