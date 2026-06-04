# 系统管理动作

## 动作类型

| 动作 | actionType | 说明 |
|------|------------|------|
| ACTION_SYS_RESTART | 6 | 系统重启 |
| ACTION_SYS_FACTORY_RESET | 7 | 恢复出厂设置 |
| ACTION_SYS_NTP_SYNC | 8 | NTP 时间同步 |
| ACTION_SYS_OTA | 9 | OTA 升级 |

## 配置示例

### 方式1：Web界面配置（推荐）

#### 示例1：系统重启

**场景**：平台下发重启命令后延时500ms重启

**配置步骤**：

1. 在外设执行管理页面编辑规则
2. 点击 **添加动作** 按钮
3. 填写动作配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **动作类型** | 选择 **系统重启** | 重启设备 |
   | **执行延时** | `500` | 延时500ms让前置动作完成 |

4. 点击 **保存** 按钮

> 💡 **提示**：系统重启强制同步执行，建议设置500ms延时让MQTT消息发送完毕

---

#### 示例2：恢复出厂设置

**场景**：按键长按10秒后恢复出厂设置

**配置步骤**：

1. 编辑规则，添加动作
2. 填写：
   - **动作类型**：选择 **恢复出厂设置**
   - **执行延时**：`0`

3. 点击 **保存**

> ⚠️ **警告**：恢复出厂设置会删除所有配置文件（device.json, network.json, protocol.json等），设备将恢复为初始状态，此操作不可逆！

---

#### 示例3：NTP 时间同步

**场景**：定时触发NTP时间同步

**配置步骤**：

1. 编辑规则，添加动作
2. 填写：
   - **动作类型**：选择 **NTP时间同步**

3. 点击 **保存**

> 💡 **提示**：每日时间点触发模式需要NTP同步成功才会生效

---

#### 示例4：OTA 升级

**场景**：平台下发升级命令后触发OTA

**配置步骤**：

1. 编辑规则，添加动作
2. 填写：
   - **动作类型**：选择 **OTA升级**

3. 点击 **保存**

> 💡 **提示**：OTA升级需要网络连接和预配置的OTA服务器地址

---

### 方式2：JSON配置文件导入

## 完整规则示例

### 按键长按 10 秒恢复出厂

```json
{
  "id": "exec_factory_reset",
  "name": "长按恢复出厂",
  "enabled": false,
  "execMode": 0,
  "triggers": [
    {
      "triggerType": 4,
      "triggerPeriphId": "btn1",
      "operatorType": 0,
      "compareValue": "",
      "timerMode": 0,
      "intervalSec": 60,
      "timePoint": "",
      "eventId": "button_long_press_10s",
      "pollResponseTimeout": 1000,
      "pollMaxRetries": 2,
      "pollInterPollDelay": 100
    }
  ],
  "actions": [
    {
      "targetPeriphId": "",
      "actionType": 7,
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

1. **同步强制**：系统重启和恢复出厂动作强制同步执行，不进异步队列
2. **不可逆**：恢复出厂设置会清除所有用户配置，谨慎使用
3. **重启前延时**：建议设置 syncDelayMs 让 MQTT 消息发送完毕后再重启
4. **OTA 前提**：OTA 需要网络连接和有效的升级服务器
