# FastBee 命令脚本编写指南

## 概述

命令脚本是 FastBee 外设执行系统的高级功能，允许用户通过简单的文本命令序列实现复杂的自动化控制逻辑。脚本在 ESP32 设备端本地解析和执行，支持 GPIO/PWM/DAC 硬件控制、外设联动、MQTT 数据上报以及随机数生成等能力。

## 快速开始

在 Web 管理页面中，进入 **外设执行** > **新增规则**，将动作类型选择为 **命令脚本**，然后在脚本编辑框中输入命令即可。

**最简示例** -- 控制 GPIO 5 闪烁一次：

```
GPIO 5 HIGH
DELAY 500
GPIO 5 LOW
```

## 支持的命令一览

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

## 基本语法规则

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

## 命令参考

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

**示例：**

```
# 随机温度 -10 到 100 之间的整数
MQTT 0 [{"id":"temperature","value":"RANDOM(-10,100)"}]

# 随机湿度 0 到 100 之间的整数
LOG 当前湿度: RANDOM(0,100)%
```

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

**示例：**

```
# 随机温度，保留 1 位小数: 如 "23.5"
MQTT 0 [{"id":"temperature","value":"RANDOMF(-10,100,1)"}]

# 随机电压，保留 2 位小数: 如 "3.27"
MQTT 0 [{"id":"voltage","value":"RANDOMF(0,5,2)"}]

# 组合使用，同时上报多个随机值
MQTT 0 [{"id":"temp","value":"RANDOMF(20,30,1)"},{"id":"humi","value":"RANDOMF(40,80,1)"}]
```

## 限制参数汇总

| 项目 | 限制值 | 说明 |
|------|--------|------|
| 脚本最大长度 | 1024 字节 | 脚本文本的总字节数 |
| 最大命令数 | 50 条 | 不含注释和空行 |
| 单条 DELAY 上限 | 10,000 ms | 10 秒 |
| DELAY 累计上限 | 30,000 ms | 30 秒 |
| 脚本执行超时 | 35,000 ms | 35 秒，超时自动中止 |
| 禁用引脚 | GPIO 6-11 | SPI Flash 专用，不可使用 |

## 实用脚本示例

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

### 示例 4：设备自检

开机或定时触发，逐个检查外设状态。

```
# 设备自检序列
LOG 开始设备自检
PERIPH led_1 HIGH
DELAY 300
PERIPH led_1 LOW
PERIPH led_2 HIGH
DELAY 300
PERIPH led_2 LOW
PERIPH buzzer HIGH
DELAY 100
PERIPH buzzer LOW
LOG 自检完成
```

### 示例 5：DAC 输出控制

使用 DAC 输出模拟信号控制外部模块。

```
# DAC 阶梯输出测试
LOG DAC 阶梯输出开始
DAC 25 0
DELAY 500
DAC 25 64
DELAY 500
DAC 25 128
DELAY 500
DAC 25 192
DELAY 500
DAC 25 255
DELAY 500
DAC 25 0
LOG DAC 测试完成
```

### 示例 6：定时上报模拟传感器数据

配合定时触发，周期性上报随机环境数据到 MQTT 平台。

```
# 上报模拟环境传感器数据
LOG 开始上报环境数据
MQTT 0 [{"id":"temperature","value":"RANDOMF(-10,45,1)"},{"id":"humidity","value":"RANDOMF(20,95,1)"},{"id":"co2","value":"RANDOM(400,2000)"}]
LOG 环境数据上报完成
```

### 示例 7：报警联动 + 状态上报

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

## 错误排查

### 常见错误及解决方法

| 现象 | 可能原因 | 解决方法 |
|------|---------|---------|
| 脚本保存失败，提示"脚本内容不能为空" | 脚本编辑框为空 | 输入至少一条有效命令 |
| 脚本保存失败，提示"脚本超过最大长度" | 脚本超过 1024 字节 | 精简脚本内容，减少注释 |
| 脚本不执行 | 规则未启用 | 检查规则的启用开关 |
| 脚本部分执行后停止 | 执行超时（超过 35 秒） | 减少 DELAY 总时间 |
| PERIPH 命令无效果 | 外设 ID 不匹配 | 核对外设配置中的 ID |
| GPIO 命令报错 | 使用了引脚 6-11 | 更换为其他可用引脚 |
| MQTT 命令无效果 | MQTT 未连接或主题索引错误 | 检查 MQTT 连接状态，确认主题索引正确 |
| RANDOM 表达式未替换 | 语法错误（缺少逗号或括号） | 检查格式：`RANDOM(min,max)` 或 `RANDOMF(min,max,decimals)` |

### 调试方法

1. **添加 LOG 命令**：在关键步骤前后添加 `LOG` 标记执行进度
2. **查看设备日志**：进入 Web 管理页面的 **设备日志**，查找 `[Script]` 和 `[PeriphExec]` 前缀的日志
3. **分段测试**：将复杂脚本拆分为小段，逐段验证后再合并
4. **检查触发条件**：确认规则的触发类型（设备触发/定时触发）和条件配置正确

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
