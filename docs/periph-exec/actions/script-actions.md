# 脚本动作

## 动作类型

| 动作 | actionType | 说明 |
|------|------------|------|
| ACTION_SCRIPT | 15 | 命令序列脚本 |
| ACTION_CALL_PERIPHERAL | 10 | 调用外设 |

## 配置示例

### 方式1：Web界面配置（推荐）

外设执行页面如下。脚本动作配置时重点核对脚本内容、执行耗时和目标外设是否存在。

![外设执行规则列表](../../system/images/periph-exec-management.png)

![命令脚本执行流程](../../images/command-script-execution-flow.svg)

脚本动作建议只承载短动作序列；先用单条命令确认目标外设可控，再加入延时和多步动作，避免长脚本阻塞执行队列。

#### 示例1：流水灯效果

**场景**：依次点亮LED1→LED2→LED3，每个延时500ms

**配置步骤**：

1. 在外设执行管理页面编辑规则
2. 点击 **添加动作** 按钮
3. 填写动作配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **动作类型** | 选择 **命令脚本** | 脚本动作 |
   | **脚本内容** | 见下方 | 多行命令序列 |

4. 在脚本内容中输入（每行一条命令）：
   ```
   PERIPH led1 HIGH
   DELAY 500
   PERIPH led1 LOW
   PERIPH led2 HIGH
   DELAY 500
   PERIPH led2 LOW
   PERIPH led3 HIGH
   DELAY 500
   PERIPH led3 LOW
   LOG 流水灯一轮完成
   ```

5. 点击 **保存** 按钮

> 💡 **提示**：
> - 命令之间用换行分隔
> - PERIPH命令格式：`PERIPH <外设ID> <动作> [参数]`
> - DELAY单位为毫秒

---

#### 示例2：PWM 渐变控制

**场景**：电机从0加速到最大，再减速到0

**配置步骤**：

1. 编辑规则，添加动作
2. 动作类型：选择 **命令脚本**
3. 脚本内容：
   ```
   PERIPH motor_pwm PWM 0
   DELAY 1000
   PERIPH motor_pwm PWM 2048
   DELAY 2000
   PERIPH motor_pwm PWM 4095
   DELAY 3000
   PERIPH motor_pwm PWM 0
   LOG 电机运行完毕
   ```

4. 点击 **保存**

> 💡 **提示**：PWM值范围取决于外设配置（8位0-255，12位0-4095）

---

#### 示例3：MQTT 上报测试数据

**场景**：定时上报随机温湿度数据用于测试

**配置步骤**：

1. 编辑规则，添加动作
2. 动作类型：选择 **命令脚本**
3. 脚本内容：
   ```
   MQTT 0 [{"id":"temperature","value":"RANDOMF(15,35,1)"},{"id":"humidity","value":"RANDOMF(30,90,1)"}]
   ```

4. 点击 **保存**

> 💡 **提示**：
> - MQTT命令格式：`MQTT <qos> <payload>`
> - RANDOMF(min,max,decimals)生成随机浮点数
> - RANDOM(min,max)生成随机整数

---

#### 示例4：调用其他外设

**场景**：直接调用DHT11传感器读取

**配置步骤**：

1. 编辑规则，添加动作
2. 填写：
   - **动作类型**：选择 **调用外设**
   - **目标外设**：选择 `dht1`
   - **调用命令**：填写 `read`

3. 点击 **保存**

---

### 方式2：JSON配置文件导入

## 完整规则示例

### 风机高低档位运行

```json
{
  "id": "exec_fan_script",
  "name": "风机脚本运行",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 1,
      "triggerPeriphId": "",
      "operatorType": 0,
      "compareValue": "",
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
      "actionType": 15,
      "actionValue": "PERIPH motor_pwm PWM 4005\nDELAY 5000\nPERIPH motor_pwm PWM 4095\nDELAY 5000\nPERIPH motor_pwm PWM 0\nLOG 风扇高低档位运行一次",
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

## 注意事项

1. **异步执行**：脚本动作在 FreeRTOS 异步任务中执行，不阻塞主循环
2. **DELAY 限制**：单条 DELAY 建议不超过 10 秒，总脚本执行时间建议不超过 60 秒
3. **资源保护**：含脚本的重规则在堆内存不足时会被跳过（不会降级同步）
4. **targetPeriphId**：脚本动作的 targetPeriphId 通常为空（由脚本内的 PERIPH 命令指定目标）
5. **编译开关**：完整脚本功能需要 `FASTBEE_ENABLE_COMMAND_SCRIPT=1`
