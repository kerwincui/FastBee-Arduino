# 规则脚本

## 功能说明

规则脚本是 FastBee 外设执行中的 `script` 动作类型，支持使用简化的 JavaScript 语法编写自定义逻辑。适用于复杂的条件判断、数据计算和多步骤控制。

![规则脚本页面](images/rule-script.png)

脚本适合把多步 GPIO、PWM、延时、变量读写封装到一条动作中。上线前建议先在禁用规则状态下保存和检查，再通过手动执行验证。

截图要点：规则脚本页面用于维护可复用脚本模板，适合记录脚本名称、启用状态、最近更新时间和脚本用途。若脚本会写 GPIO、PWM 或继电器，截图留档时应同步记录实际接线，避免后续迁移到不同硬件时误触发。

![脚本排障决策树](../images/script-debug-decision-tree.svg)

脚本排障时先判断故障发生在哪一段：保存阶段、触发阶段、变量替换阶段、外设动作阶段或资源保护阶段。每一段对应的日志和处理方式不同。

## 操作指南

在外设执行规则的 actions 中使用 `"type": "script"` 动作：

```json
{ "type": "script", "params": { "code": "你的脚本代码" } }
```

## 参数说明

### 内置函数

| 函数 | 说明 | 示例 |
|------|------|------|
| gpioWrite(id, val) | GPIO写入 | gpioWrite('led_d1', 0) |
| gpioRead(id) | GPIO读取 | var v = gpioRead('key1') |
| pwmWrite(id, duty) | PWM写入 | pwmWrite('pwm_led', 128) |
| servoWrite(id, angle) | 舵机控制 | servoWrite('servo_01', 90) |
| analogRead(id) | ADC读取 | var v = analogRead('adc_01') |
| getVar(name, default) | 读全局变量 | var x = getVar('count', 0) |
| setVar(name, value) | 写全局变量 | setVar('count', x+1) |
| delay(ms) | 延时（毫秒） | delay(100) |
| log(msg) | 输出日志 | log('温度:' + temp) |

### 内置变量

| 变量 | 说明 |
|------|------|
| event.data | 触发事件携带的数据 |
| event.type | 触发事件类型 |
| event.source | 触发事件源 |

## 配置示例

### 条件控制

```javascript
var temp = getVar('temperature', 25);
if (temp > 35) {
  gpioWrite('fan_relay', 1);
  gpioWrite('led_d1', 0);
} else {
  gpioWrite('fan_relay', 0);
}
```

### 循环渐变

```javascript
var duty = getVar('duty', 0);
var dir = getVar('dir', 1);
duty += dir * 10;
if (duty >= 255) { duty = 255; dir = -1; }
if (duty <= 0) { duty = 0; dir = 1; }
pwmWrite('pwm_led', duty);
setVar('duty', duty);
setVar('dir', dir);
```

## 故障排除

| 问题 | 可能原因 | 解决方案 |
|------|---------|---------|
| 脚本不执行 | 语法错误 | 检查日志中的错误信息 |
| 变量丢失 | 设备重启 | 全局变量仅存于内存，重启后清零 |
| 执行超时 | 脚本过长/死循环 | 限制脚本复杂度，避免循环 |
| 函数未定义 | 拼写错误 | 检查函数名大小写 |
