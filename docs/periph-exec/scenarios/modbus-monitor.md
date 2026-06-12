# Modbus 设备监控场景

## 场景描述

通过轮询触发定期采集 Modbus RS485 从站设备数据（温湿度/空气质量），采集结果自动通过 MQTT 上报至平台，并在 OLED 屏幕显示。

## 所需外设

| 外设 | 类型 | 说明 |
|------|------|------|
| uart_rs485 | UART(1) | RS485 通信端口 |
| modbus_sensor | MODBUS_DEVICE(51) | Modbus 传感器设备 |
| oled1 | LCD(36) | OLED 显示屏（可选） |

## 完整配置流程

### 方式1：Web界面配置（推荐）

本场景需要先配置 Modbus RTU 串口和从站，再在外设执行中创建轮询采集、MQTT 上报和 OLED 显示动作。

![Modbus RTU 配置页面](../../system/images/protocol-modbus-rtu.png)

![外设配置列表](../../system/images/peripheral-management.png)

![外设执行规则列表](../../system/images/periph-exec-management.png)

#### 第一步：配置外设

> 💡 **前提**：需提前配置好Modbus传感器设备和OLED显示屏（参考scenarios/modbus-sensor-devices.md）

**所需外设**：
- `uart_rs485` - UART(type: 1) - RS485通信端口
- `modbus_sensor` - MODBUS_DEVICE(type: 51) - Modbus传感器设备
- `oled1` - LCD(type: 36) - OLED显示屏（可选）

---

#### 第二步：配置外设执行规则

**规则：每5分钟轮询Modbus传感器**

1. 点击左侧菜单 **外设配置** → 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 填写基础配置：
   - **规则名称**：`采集环境质量`
   - **上报数据**：✅ 启用（自动上报采集结果）
   - **启用**：✅ 启用

4. 配置触发器：
   - **触发类型**：选择 **轮询触发**
   - **轮询间隔**：`300`（300秒=5分钟）
   - **响应超时**：`1000`（1000毫秒）
   - **最大重试**：`2`
   - **请求间隔**：`100`（100毫秒）

5. 配置动作（需要3个动作）：

   **动作1：轮询Modbus任务0**
   - **动作类型**：选择 **Modbus轮询**
   - **目标外设**：填写 `modbus-task:0`（固定格式）
   - **任务索引**：`[0]`
   - **执行延时**：`0`

   **动作2：轮询Modbus任务1**
   - **动作类型**：选择 **Modbus轮询**
   - **目标外设**：填写 `modbus-task:1`
   - **任务索引**：`[1]`
   - **执行延时**：`200`（延时200ms避免总线冲突）

   **动作3：OLED显示数据**
   - **动作类型**：选择 **OLED显示**
   - **目标外设**：选择 `oled1`
   - **显示模板**：
     ```
     #环境监测
     温度: ${modbus_temp.value}°C
     湿度: ${modbus_humi.value}%
     CO2: ${modbus_co2.value}ppm
     ```
   - **执行延时**：`500`（延时500ms等待数据采集完成）

6. 点击 **保存** 按钮

> 💡 **提示**：
> - targetPeriphId格式固定为 `modbus-task:N`，N为设备索引
> - 多个Modbus动作间需设置syncDelayMs避免总线冲突
> - 启用“上报数据”后采集结果自动通过MQTT上报

---

### 方式2：JSON配置文件导入

## 通信参数说明

| 参数 | 值 | 说明 |
|------|-----|------|
| pollResponseTimeout | 1000 | 等待从站响应最大1秒 |
| pollMaxRetries | 2 | 失败后重试2次 |
| pollInterPollDelay | 100 | 两次请求间隔100ms |

## 数据流

1. 轮询触发器每 300 秒触发一次
2. ACTION_MODBUS_POLL 依次采集 task:0 和 task:1 的数据
3. 采集结果存入传感器缓存
4. OLED 显示动作引用缓存数据更新屏幕
5. `reportAfterExec: true` 使采集结果通过 MQTT 上报

## 注意事项

1. **Modbus 就绪**：轮询触发自动检查 Modbus 是否就绪，未就绪时跳过
2. **动作间隔**：多个 MODBUS_POLL 动作间设置 syncDelayMs 避免总线冲突
3. **失败退避**：通信失败后 30 秒内不再重试，避免堵塞系统
4. **异步执行**：Modbus 轮询在 FreeRTOS 异步任务中执行，不阻塞主循环
