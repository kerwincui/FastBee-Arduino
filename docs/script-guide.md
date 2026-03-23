# FastBee 脚本系统完整指南

本文档涵盖 FastBee 外设执行系统的两大脚本功能：

- **规则脚本**（数据转换模板引擎） -- 用于多协议数据格式转换
- **命令脚本**（硬件控制脚本引擎） -- 用于 GPIO/PWM/DAC 硬件自动化控制

---

# 第一部分：规则脚本（数据转换模板）

## 1. 功能概述

规则脚本是 FastBee 的多协议数据格式转换管道。当设备通过不同协议（MQTT、Modbus RTU/TCP、HTTP、CoAP、TCP）收发数据时，规则脚本可以在数据流经系统时自动进行格式转换。

**核心用途：**

- 将平台标准数组格式与第三方系统的自定义 JSON 格式互转
- 统一不同协议的数据格式，使外部设备无缝接入 FastBee 平台
- 无需修改固件代码即可适配新的数据格式需求

**执行模型：**

```
输入数据 (JSON) --> 提取 key-value --> 替换模板占位符 --> 输出结果
```

规则脚本是纯数据转换管道，不产生副作用（不控制 GPIO、不发送消息），仅对经过的数据做格式变换。

## 2. 基本概念

### 2.1 触发类型

规则脚本有两种触发类型：

| 触发类型 | 编号 | 说明 | 数据流向 |
|---------|------|------|---------|
| 数据接收 (DATA_RECEIVE) | 3 | 协议数据到达时触发 | 外部 --> ESP32 |
| 数据上报 (DATA_REPORT) | 4 | 协议数据发送前触发 | ESP32 --> 外部 |

**数据接收 (triggerType=3)：** 设备从外部接收到数据后，先经过规则脚本转换，再交给系统处理。典型场景是将第三方设备发来的自定义 JSON 格式转为 FastBee 标准数组格式。

**数据上报 (triggerType=4)：** 设备准备向外部发送数据前，先经过规则脚本转换。典型场景是将 FastBee 内部的标准数组格式转为目标平台要求的 JSON 格式。

### 2.2 协议类型

每条规则绑定一种协议类型，只对该协议的数据流生效：

| 协议类型 | 编号 | 说明 |
|---------|------|------|
| MQTT | 0 | MQTT 消息收发 |
| Modbus RTU | 1 | Modbus 串口通信 |
| Modbus TCP | 2 | Modbus TCP/IP 通信 |
| HTTP | 3 | HTTP 请求/响应 |
| CoAP | 4 | CoAP 物联网协议 |
| TCP | 5 | 原始 TCP 套接字 |

### 2.3 模板语法 (`${key}` 占位符)

规则脚本使用纯文本模板，通过 `${key}` 占位符引用输入数据中的字段值。

**工作原理：**

1. 系统解析输入的 JSON 数据，提取所有 key-value 对
2. 扫描模板中的 `${key}` 占位符
3. 将每个占位符替换为对应的值
4. 返回替换后的字符串作为输出

**支持的输入格式：**

- **数组格式**（FastBee 标准）：`[{"id":"temperature","value":"27.43"}, ...]`
  - 提取规则：`id` 字段作为 key，`value` 字段作为 value
- **对象格式**（扁平 JSON）：`{"temperature": 27.43, ...}`
  - 提取规则：对象的每个键名作为 key，键值作为 value

## 3. 创建和管理规则脚本

### 3.1 创建规则

1. 打开 Web 管理页面，进入 **规则脚本** 页面
2. 点击 **新增规则** 按钮
3. 填写规则信息：
   - **名称**：规则的描述性名称，如 "MQTT上报:数组转对象"
   - **触发类型**：选择 "数据接收" 或 "数据上报"
   - **协议类型**：选择该规则适用的通信协议
   - **脚本内容**：编写 `${key}` 占位符模板（详见下文）
4. 点击 **保存**

### 3.2 编辑规则

在规则脚本表格中，点击对应规则的 **编辑** 按钮，修改后保存。

### 3.3 启用/禁用规则

- 点击规则行的 **启用/禁用** 按钮切换状态
- 禁用的规则不参与数据转换匹配
- 新建的预设示例默认禁用，需手动启用

### 3.4 删除规则

点击规则行的 **删除** 按钮移除规则。删除操作不可撤销。

### 3.5 规则匹配优先级

- 同一协议类型和触发方向，只有**第一条**匹配的已启用规则会生效
- 如果没有匹配的规则，数据原样传递不做任何转换

## 4. 脚本语法详解

### 4.1 占位符格式

```
${变量名}
```

- 变量名对应输入 JSON 中的 key（数组格式的 `id` 值，或对象格式的键名）
- 变量名区分大小写：`${temperature}` 和 `${Temperature}` 是不同的
- 未匹配到的占位符保持原样不替换

### 4.2 数组格式输入的解析

输入数据为 FastBee 标准数组格式时：

```json
[
  {"id": "temperature", "value": "27.43", "remark": ""},
  {"id": "humidity", "value": "32.18", "remark": ""}
]
```

系统提取的 key-value 对：

| key | value |
|-----|-------|
| temperature | 27.43 |
| humidity | 32.18 |

### 4.3 对象格式输入的解析

输入数据为扁平 JSON 对象时：

```json
{"temperature": 26.5, "humidity": 65.8}
```

系统提取的 key-value 对：

| key | value |
|-----|-------|
| temperature | 26.5 |
| humidity | 65.8 |

### 4.4 限制参数

| 项目 | 限制值 | 说明 |
|------|--------|------|
| 脚本最大长度 | 2048 字节 | scriptContent 字段的最大字节数 |
| 最大 key-value 对数 | 32 个 | 超出部分忽略 |
| 输入格式 | JSON 数组或对象 | 非 JSON 输入返回原始数据 |

## 5. 实际应用案例

### 案例 1：MQTT 上报 -- 数组转对象

**场景：** FastBee 内部使用数组格式存储传感器数据，但云平台要求扁平 JSON 对象格式。

**触发类型：** 数据上报 (DATA_REPORT)
**协议类型：** MQTT

**输入数据（FastBee 标准格式）：**

```json
[
  {"id": "temperature", "value": "27.43", "remark": ""},
  {"id": "humidity", "value": "32.18", "remark": ""}
]
```

**脚本内容：**

```
{"temperature": ${temperature}, "humidity": ${humidity}}
```

**输出结果：**

```json
{"temperature": 27.43, "humidity": 32.18}
```

### 案例 2：MQTT 接收 -- 对象转数组

**场景：** 外部设备发来扁平 JSON，需要转为 FastBee 标准数组格式才能被系统正确解析。

**触发类型：** 数据接收 (DATA_RECEIVE)
**协议类型：** MQTT

**输入数据（外部设备格式）：**

```json
{"temperature": 26.5, "humidity": 65.8}
```

**脚本内容：**

```
[{"id":"temperature","value":"${temperature}","remark":""},{"id":"humidity","value":"${humidity}","remark":""}]
```

**输出结果：**

```json
[{"id":"temperature","value":"26.5","remark":""},{"id":"humidity","value":"65.8","remark":""}]
```

### 案例 3：Modbus RTU 数据接收转换

**场景：** Modbus RTU 从站返回的寄存器数据被驱动层解析为 JSON 对象后，需要转为 FastBee 标准数组格式上报平台。

**触发类型：** 数据接收 (DATA_RECEIVE)
**协议类型：** Modbus RTU

**输入数据：**

```json
{"temperature": 25.3, "humidity": 60.1}
```

**脚本内容：**

```
[{"id":"temperature","value":"${temperature}","remark":""},{"id":"humidity","value":"${humidity}","remark":""}]
```

**输出结果：**

```json
[{"id":"temperature","value":"25.3","remark":""},{"id":"humidity","value":"60.1","remark":""}]
```

### 案例 4：HTTP 上报自定义格式

**场景：** 向第三方 HTTP API 推送数据，目标 API 要求特定的 JSON 结构。

**触发类型：** 数据上报 (DATA_REPORT)
**协议类型：** HTTP

**输入数据：**

```json
[{"id":"temperature","value":"27.43"},{"id":"humidity","value":"32.18"}]
```

**脚本内容：**

```
{"device":"esp32","temp":${temperature},"humi":${humidity}}
```

**输出结果：**

```json
{"device":"esp32","temp":27.43,"humi":32.18}
```

### 案例 5：TCP 上报精简文本

**场景：** 通过 TCP 连接向服务器发送精简文本格式数据，节省带宽。

**触发类型：** 数据上报 (DATA_REPORT)
**协议类型：** TCP

**输入数据：**

```json
[{"id":"temperature","value":"27.43"},{"id":"humidity","value":"32.18"}]
```

**脚本内容：**

```
T:${temperature},H:${humidity}
```

**输出结果：**

```
T:27.43,H:32.18
```

### 案例 6：多传感器组合转换

**场景：** 设备同时采集温度、湿度、气压、光照四个传感器值，需要转为嵌套 JSON 格式。

**脚本内容：**

```
{"sensors":{"temp":${temperature},"humi":${humidity},"press":${pressure},"light":${light}},"unit":"metric"}
```

## 6. 最佳实践

### 6.1 命名规范

使用清晰的命名格式：`协议名 + 方向 + 简要说明`

```
MQTT上报:数组转对象
MQTT接收:对象转数组
ModbusRTU接收转换
HTTP上报:自定义格式
```

### 6.2 先禁用再编辑

编辑复杂的转换模板时，建议先禁用规则，编辑完成后再启用。避免编辑过程中不完整的模板影响数据流。

### 6.3 一个协议方向一条规则

同一协议类型 + 同一触发方向只有第一条匹配规则生效。避免创建重复的规则。

### 6.4 注意值类型

- 数值类型直接使用占位符：`"temp": ${temperature}` -- 输出 `"temp": 27.43`
- 字符串类型需要加引号：`"name": "${deviceName}"` -- 输出 `"name": "esp32"`

### 6.5 预设示例数据

系统首次启动时会自动创建 7 条预设示例规则，覆盖所有 6 种协议类型。这些示例默认禁用，可以直接修改后启用使用，也可作为参考模板。

| 示例名称 | 触发类型 | 协议 | 说明 |
|---------|---------|------|------|
| MQTT上报:数组转对象 | 数据上报 | MQTT | 标准数组 --> 扁平对象 |
| MQTT接收:对象转数组 | 数据接收 | MQTT | 扁平对象 --> 标准数组 |
| ModbusRTU接收转换 | 数据接收 | Modbus RTU | 对象 --> 标准数组 |
| ModbusTCP接收转换 | 数据接收 | Modbus TCP | 对象 --> 标准数组 |
| HTTP上报:自定义格式 | 数据上报 | HTTP | 标准数组 --> 自定义 JSON |
| CoAP接收转换 | 数据接收 | CoAP | 对象 --> 标准数组 |
| TCP上报:精简格式 | 数据上报 | TCP | 标准数组 --> 纯文本 |

## 7. API 接口说明

规则脚本通过与外设执行共用的 REST API 进行管理。规则脚本与普通外设执行规则存储在同一配置文件中，通过 `triggerType` 字段区分。

### 7.1 获取所有规则

```
GET /api/periph-exec
```

**响应字段（规则脚本相关）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| id | string | 规则唯一标识 |
| name | string | 规则名称 |
| enabled | boolean | 是否启用 |
| triggerType | number | 触发类型（3=数据接收, 4=数据上报） |
| protocolType | number | 协议类型（0-5） |
| scriptContent | string | 模板脚本内容 |
| lastTriggerTime | number | 上次触发时间（毫秒时间戳） |
| triggerCount | number | 累计触发次数 |

### 7.2 新增规则

```
POST /api/periph-exec
Content-Type: application/x-www-form-urlencoded

name=MQTT上报:数组转对象
&triggerType=4
&protocolType=0
&scriptContent={"temperature": ${temperature}, "humidity": ${humidity}}
&enabled=true
&execMode=0
&operatorType=0
&compareValue=
&sourcePeriphId=
&timerMode=0
&intervalSec=60
&timePoint=
&targetPeriphId=
&actionType=0
&actionValue=
```

### 7.3 更新规则

```
POST /api/periph-exec/update
Content-Type: application/x-www-form-urlencoded

id=exec_script_mqtt_a2o
&name=MQTT上报:数组转对象
&triggerType=4
&protocolType=0
&scriptContent={"temperature": ${temperature}, "humidity": ${humidity}}
```

### 7.4 启用/禁用规则

```
POST /api/periph-exec/enable   id=exec_script_mqtt_a2o
POST /api/periph-exec/disable  id=exec_script_mqtt_a2o
```

### 7.5 删除规则

```
DELETE /api/periph-exec/?id=exec_script_mqtt_a2o
```

---

# 第二部分：命令脚本（硬件控制）

## 1. 功能概述

命令脚本是 FastBee 外设执行系统的高级功能，允许用户通过简单的文本命令序列实现复杂的自动化控制逻辑。脚本在 ESP32 设备端本地解析和执行，支持 GPIO/PWM/DAC 硬件控制、外设联动、MQTT 数据上报以及随机数生成等能力。

## 2. 快速开始

在 Web 管理页面中，进入 **外设执行** > **新增规则**，将动作类型选择为 **命令脚本**，然后在脚本编辑框中输入命令即可。

**最简示例** -- 控制 GPIO 5 闪烁一次：

```
GPIO 5 HIGH
DELAY 500
GPIO 5 LOW
```

## 3. 支持的命令一览

| 命令 | 格式 | 说明 |
|------|------|------|
| `GPIO` | `GPIO <pin> HIGH\|LOW` | 设置引脚高/低电平 |
| `DELAY` | `DELAY <ms>` | 延时等待（最大 10 秒/条） |
| `PWM` | `PWM <pin> <duty>` | 设置 PWM 占空比（0-255） |
| `DAC` | `DAC <pin> <value>` | 设置 DAC 模拟输出（0-255） |
| `LOG` | `LOG <message>` | 输出调试日志（支持随机数表达式） |
| `PERIPH` | `PERIPH <id> <action> [param]` | 通过 ID 控制已配置外设 |
| `MQTT` | `MQTT <topicIndex> <message>` | 发布 MQTT 消息（支持随机数表达式） |

**随机数表达式**（可用于 `MQTT` 和 `LOG` 命令）：

| 表达式 | 说明 | 示例 | 输出 |
|--------|------|------|------|
| `RANDOM(min,max)` | 随机整数 | `RANDOM(-10,100)` | `27` |
| `RANDOMF(min,max,decimals)` | 随机浮点数 | `RANDOMF(-10,100,1)` | `23.5` |

## 4. 基本语法规则

- 每行一条命令
- 命令名不区分大小写（`GPIO`、`gpio`、`Gpio` 均有效）
- 参数之间用空格或制表符分隔
- 以 `#` 开头的行为注释，会被跳过
- 空行会被自动忽略

```
# 这是注释，不会执行
GPIO 5 HIGH      # 行内注释不支持，此行会解析失败

# 正确写法：注释独占一行
GPIO 5 HIGH
```

## 5. 命令参考

### GPIO -- 数字引脚控制

设置指定 GPIO 引脚的高/低电平。

```
GPIO <引脚号> HIGH|LOW
```

| 参数 | 说明 |
|------|------|
| 引脚号 | GPIO 编号，如 2、4、5、12-33 等 |
| HIGH/LOW | 输出电平，HIGH 为高电平，LOW 为低电平 |

**示例：**

```
GPIO 2 HIGH       # 板载 LED 亮（大部分 ESP32 开发板）
GPIO 2 LOW        # 板载 LED 灭
GPIO 15 HIGH      # GPIO15 输出高电平
```

**注意：** 引脚 6-11 为 SPI Flash 专用，脚本中禁止使用。

---

### DELAY -- 延时等待

暂停脚本执行指定的毫秒数。

```
DELAY <毫秒>
```

| 参数 | 说明 |
|------|------|
| 毫秒 | 等待时间，单位 ms，范围 1-10000 |

**示例：**

```
DELAY 500         # 等待 500 毫秒
DELAY 1000        # 等待 1 秒
DELAY 5000        # 等待 5 秒
```

**限制：**
- 单条 DELAY 最大 10000ms（10 秒）
- 脚本中所有 DELAY 的累计时间不超过 30000ms（30 秒）

---

### PWM -- PWM 输出

设置指定引脚的 PWM 占空比。

```
PWM <引脚号> <占空比>
```

| 参数 | 说明 |
|------|------|
| 引脚号 | GPIO 编号 |
| 占空比 | 0-255，0 为完全关闭，255 为完全导通 |

**示例：**

```
PWM 4 128         # 50% 占空比
PWM 4 255         # 100% 占空比（全亮）
PWM 4 0           # 关闭 PWM 输出
```

**说明：** 脚本中的 PWM 使用 LEDC 通道 15（专用），频率 5000Hz，8 位分辨率，不会与外设管理器的通道冲突。

---

### DAC -- 模拟输出

设置指定引脚的 DAC 模拟输出值。

```
DAC <引脚号> <输出值>
```

| 参数 | 说明 |
|------|------|
| 引脚号 | DAC 引脚，ESP32 仅支持 GPIO 25 和 GPIO 26 |
| 输出值 | 0-255，对应 0V-3.3V |

**示例：**

```
DAC 25 128        # 输出约 1.65V
DAC 26 255        # 输出约 3.3V
DAC 25 0          # 输出 0V
```

---

### LOG -- 输出日志

在系统日志中输出一条信息，用于调试和追踪脚本执行过程。支持 `RANDOM`/`RANDOMF` 随机数表达式。

```
LOG <消息内容>
```

| 参数 | 说明 |
|------|------|
| 消息内容 | 任意文本，支持空格，多个词会自动拼接。支持 `RANDOM`/`RANDOMF` 表达式 |

**示例：**

```
LOG 脚本开始执行
LOG Setting GPIO 5 to HIGH
LOG 温度过高，启动风扇
LOG 当前模拟温度: RANDOMF(20,35,1) 度
```

日志会以 `[Script]` 前缀输出到系统日志，可在 **设备日志** 页面查看。

---

### PERIPH -- 外设控制

通过外设 ID 控制已配置的外设，支持多种子动作。

```
PERIPH <外设ID> <子动作> [参数]
```

| 参数 | 说明 |
|------|------|
| 外设ID | 外设配置中的标识符，如 `led_1`、`fan`、`relay_main` |
| 子动作 | 控制动作，见下表 |
| 参数 | 部分子动作需要额外参数 |

**支持的子动作：**

| 子动作 | 参数 | 说明 |
|--------|------|------|
| `HIGH` | 无 | 设置外设为高电平 |
| `LOW` | 无 | 设置外设为低电平 |
| `PWM <duty>` | 占空比 0-255 | 设置 PWM 占空比 |
| `BLINK [ms]` | 可选，间隔毫秒，默认 500 | 启动闪烁效果（异步定时器） |
| `BREATHE [ms]` | 可选，周期毫秒，默认 2000 | 启动呼吸灯效果（异步定时器） |
| `STOP` | 无 | 停止当前定时器动作（闪烁/呼吸灯） |

**示例：**

```
# 基本开关控制
PERIPH relay_1 HIGH          # 打开继电器
PERIPH relay_1 LOW           # 关闭继电器

# PWM 调光
PERIPH led_strip PWM 200     # LED 灯带亮度调到 200

# 效果控制
PERIPH status_led BLINK 300  # 状态灯 300ms 间隔闪烁
PERIPH mood_led BREATHE 1500 # 氛围灯 1.5 秒周期呼吸
PERIPH mood_led STOP         # 停止呼吸灯效果
```

**注意：**
- 外设 ID 必须与外设配置页面中已配置的外设一致
- `BLINK` 和 `BREATHE` 启动异步定时器后立即返回，不阻塞后续命令
- 若外设 ID 不存在，该命令会被跳过（记录警告日志），脚本继续执行

---

### MQTT -- MQTT 数据上报

通过 MQTT 协议发布消息到指定的发布主题，可用于模拟数据上报、状态通知等场景。

```
MQTT <主题索引> <消息内容>
```

| 参数 | 说明 |
|------|------|
| 主题索引 | 已配置的 MQTT 发布主题索引（从 0 开始） |
| 消息内容 | 要发布的消息文本，支持 `RANDOM`/`RANDOMF` 随机数表达式 |

**示例：**

```
# 上报固定数据
MQTT 0 [{"id":"switch","value":"1"}]

# 上报随机温度（整数，-10 到 100）
MQTT 0 [{"id":"temperature","value":"RANDOM(-10,100)"}]

# 上报随机温度（浮点数，保留 1 位小数）
MQTT 0 [{"id":"temperature","value":"RANDOMF(-10,100,1)"}]

# 同时上报多个属性
MQTT 0 [{"id":"temperature","value":"RANDOMF(15,35,1)"},{"id":"humidity","value":"RANDOMF(30,90,1)"}]
```

**注意：**
- 主题索引对应 MQTT 配置页面中 **发布主题** 列表的顺序（第一个为 0，第二个为 1，以此类推）
- MQTT 未连接时该命令会被跳过（记录警告日志），脚本继续执行
- 消息内容中的 `RANDOM`/`RANDOMF` 表达式会在每次执行时生成新的随机值

---

### 随机数表达式

随机数表达式可用于 `MQTT` 和 `LOG` 命令的消息内容中，每次脚本执行时生成不同的随机值。

#### RANDOM(min,max) -- 随机整数

生成 `[min, max]` 范围内的随机整数（闭区间）。

```
RANDOM(min,max)
```

| 参数 | 说明 |
|------|------|
| min | 最小值（整数，可为负数） |
| max | 最大值（整数） |

#### RANDOMF(min,max,decimals) -- 随机浮点数

生成 `[min, max]` 范围内的随机浮点数，并格式化到指定小数位数。

```
RANDOMF(min,max,decimals)
```

| 参数 | 说明 |
|------|------|
| min | 最小值（可为负数或小数） |
| max | 最大值 |
| decimals | 小数位数（0-6） |

## 6. 命令脚本限制参数

| 项目 | 限制值 | 说明 |
|------|--------|------|
| 脚本最大长度 | 1024 字节 | 脚本文本的总字节数 |
| 最大命令数 | 50 条 | 不含注释和空行 |
| 单条 DELAY 上限 | 10,000 ms | 10 秒 |
| DELAY 累计上限 | 30,000 ms | 30 秒 |
| 脚本执行超时 | 35,000 ms | 35 秒，超时自动中止 |
| 禁用引脚 | GPIO 6-11 | SPI Flash 专用，不可使用 |

## 7. 命令脚本实用示例

### 示例 1：报警闪烁

温度过高时触发，LED 快速闪烁 5 次进行报警提示。

```
# 温度报警 - LED快速闪烁5次
LOG 温度报警触发
GPIO 2 HIGH
DELAY 200
GPIO 2 LOW
DELAY 200
GPIO 2 HIGH
DELAY 200
GPIO 2 LOW
DELAY 200
GPIO 2 HIGH
DELAY 200
GPIO 2 LOW
DELAY 200
GPIO 2 HIGH
DELAY 200
GPIO 2 LOW
DELAY 200
GPIO 2 HIGH
DELAY 200
GPIO 2 LOW
LOG 报警闪烁完成
```

### 示例 2：渐亮效果

通过 PWM 实现 LED 从暗到亮的渐变效果。

```
# LED 渐亮效果
LOG 开始渐亮
PWM 4 0
DELAY 100
PWM 4 50
DELAY 100
PWM 4 100
DELAY 100
PWM 4 150
DELAY 100
PWM 4 200
DELAY 100
PWM 4 255
LOG 渐亮完成
```

### 示例 3：多外设联动

通过外设 ID 协调控制多个设备。

```
# 场景：晚间模式
LOG 进入晚间模式
PERIPH main_light LOW
DELAY 500
PERIPH night_light PWM 30
PERIPH mood_led BREATHE 3000
LOG 晚间模式已激活
```

### 示例 4：定时上报模拟传感器数据

配合定时触发，周期性上报随机环境数据到 MQTT 平台。

```
# 上报模拟环境传感器数据
LOG 开始上报环境数据
MQTT 0 [{"id":"temperature","value":"RANDOMF(-10,45,1)"},{"id":"humidity","value":"RANDOMF(20,95,1)"},{"id":"co2","value":"RANDOM(400,2000)"}]
LOG 环境数据上报完成
```

### 示例 5：报警联动 + 状态上报

检测到异常后，控制外设报警并上报状态。

```
# 异常报警联动
LOG 检测到异常，启动报警
PERIPH alarm_led BLINK 200
PERIPH buzzer HIGH
DELAY 3000
PERIPH buzzer LOW
MQTT 0 [{"id":"alarm","value":"1"},{"id":"alarm_type","value":"temperature_high"}]
LOG 报警状态已上报
```

---

# 第三部分：常见问题与故障排除

## 规则脚本 FAQ

### Q: 为什么我的转换规则没有生效？

**检查项：**
1. 规则是否已启用（enabled=true）
2. 触发类型是否正确（3=数据接收，4=数据上报）
3. 协议类型是否匹配（确保与实际使用的通信协议一致）
4. 脚本内容中的 `${key}` 变量名是否与输入数据中的 key 完全一致（大小写敏感）

### Q: 同一协议可以创建多条转换规则吗？

可以创建多条，但同一协议类型 + 同一触发方向只有第一条匹配的已启用规则生效。建议每种协议每个方向只保留一条规则。

### Q: 输入的 JSON 解析失败会怎样？

如果输入数据不是有效的 JSON，模板引擎会跳过转换，返回原始数据不做任何修改。系统日志中不会产生错误，数据流不受影响。

### Q: `${key}` 中的 key 找不到会怎样？

未匹配的占位符保持原样。例如输入中没有 `pressure` 字段，模板中的 `${pressure}` 会保留为字面文本 `${pressure}`。

### Q: 规则脚本的执行会影响设备性能吗？

影响很小。模板引擎使用简单的字符串替换，不涉及正则表达式或脚本解释器。转换过程在微秒级完成。mutex 锁仅在匹配规则阶段持有，模板替换在无锁状态下执行。

### Q: 如何查看规则脚本的转换日志？

在设备日志页面查找 `[PeriphExec] Template applied:` 前缀的日志。每次成功转换都会记录变量数量和数据大小变化。

## 命令脚本 FAQ

### Q: 脚本保存失败，提示"脚本内容不能为空"

脚本编辑框为空或只有空行/注释。请输入至少一条有效命令。

### Q: 脚本保存失败，提示"脚本超过最大长度"

脚本超过 1024 字节。请精简脚本内容，减少注释。

### Q: 脚本不执行

检查规则是否已启用。进入规则列表确认启用状态为开。

### Q: PERIPH 命令无效果

外设 ID 不匹配。核对外设配置页面中的 ID 是否与脚本中使用的一致。

### Q: MQTT 命令无效果

MQTT 未连接或主题索引错误。检查 MQTT 连接状态，确认主题索引对应的发布主题存在。

## 调试方法

1. **添加 LOG 命令**：在关键步骤前后添加 `LOG` 标记执行进度
2. **查看设备日志**：进入 Web 管理页面的 **设备日志**，查找 `[Script]` 和 `[PeriphExec]` 前缀的日志
3. **分段测试**：将复杂脚本拆分为小段，逐段验证后再合并
4. **检查触发条件**：确认规则的触发类型和条件配置正确

---

# 附录

## GPIO 引脚速查

以下为 ESP32 常用可用引脚（因开发板型号不同可能有差异）：

| 引脚 | 说明 | 脚本可用 |
|------|------|---------|
| GPIO 0 | BOOT 按钮，上拉 | 谨慎使用 |
| GPIO 1 | TX0 串口输出 | 不建议 |
| GPIO 2 | 板载 LED（部分开发板） | 可用 |
| GPIO 3 | RX0 串口输入 | 不建议 |
| GPIO 4 | 通用 | 可用 |
| GPIO 5 | 通用 | 可用 |
| **GPIO 6-11** | **SPI Flash** | **禁止使用** |
| GPIO 12-33 | 通用 | 可用 |
| GPIO 34-39 | 仅输入（无内部上拉） | 仅读取，不支持输出 |

## 配置文件

规则脚本和命令脚本的规则都存储在 `/config/periph_exec.json` 文件中，通过 `triggerType` 字段区分：

- `triggerType` 0/1/2: 外设执行规则（平台触发/定时触发/设备触发）
- `triggerType` 3: 规则脚本 - 数据接收转换
- `triggerType` 4: 规则脚本 - 数据上报转换
