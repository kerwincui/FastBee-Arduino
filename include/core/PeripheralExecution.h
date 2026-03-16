#ifndef PERIPHERAL_EXECUTION_H
#define PERIPHERAL_EXECUTION_H

#include <Arduino.h>

// 条件操作符枚举
enum class ExecOperator : uint8_t {
    EQ = 0,           // 等于
    NEQ = 1,          // 不等于
    GT = 2,           // 大于
    LT = 3,           // 小于
    GTE = 4,          // 大于等于
    LTE = 5,          // 小于等于
    BETWEEN = 6,      // 区间内
    NOT_BETWEEN = 7,  // 区间外
    CONTAIN = 8,      // 包含
    NOT_CONTAIN = 9   // 不包含
};

// 动作类型枚举
enum class ExecActionType : uint8_t {
    ACTION_HIGH = 0,              // 设置高电平
    ACTION_LOW = 1,               // 设置低电平
    ACTION_BLINK = 2,             // 闪烁
    ACTION_BREATHE = 3,           // 呼吸灯
    ACTION_SET_PWM = 4,           // 设置PWM占空比
    ACTION_SET_DAC = 5,           // 设置DAC值
    ACTION_SYS_RESTART = 6,       // 系统重启
    ACTION_SYS_FACTORY_RESET = 7, // 恢复出厂设置
    ACTION_SYS_NTP_SYNC = 8,     // NTP时间同步
    ACTION_SYS_OTA = 9,          // OTA升级
    ACTION_SYS_AP_PROVISION = 10, // AP配网
    ACTION_SYS_BLE_PROVISION = 11,// BLE配网
    ACTION_CALL_PERIPHERAL = 12   // 调用其他外设
};

// 触发类型
enum class ExecTriggerType : uint8_t {
    DEVICE_TRIGGER = 0,  // 设备触发（MQTT消息匹配）
    TIMER_TRIGGER = 1    // 定时触发
};

// 定时模式
enum class ExecTimerMode : uint8_t {
    INTERVAL = 0,    // 间隔触发
    DAILY_TIME = 1   // 每日时间点
};

// 执行规则结构体
struct PeriphExecRule {
    String id;                  // 唯一标识 (exec_<millis>)
    String name;                // 显示名称
    bool enabled = true;        // 启用状态
    uint8_t triggerType = 0;    // 0=设备触发(MQTT), 1=定时触发

    // 设备触发字段
    String sourceId;            // MQTT JSON 中的 id 字段值 (如 "temperature")
    uint8_t operatorType = 0;   // ExecOperator 枚举值
    String compareValue;        // 比较值 (between 时用逗号分隔: "20,30")

    // 定时触发字段
    uint8_t timerMode = 0;      // 0=间隔, 1=每日时间点
    uint32_t intervalSec = 60;  // 间隔秒数
    String timePoint;           // HH:MM 格式

    // 动作字段
    String targetPeriphId;      // 目标外设 ID (系统功能时可为空)
    uint8_t actionType = 0;     // ExecActionType 枚举值
    String actionValue;         // 动作参数 (PWM值/DAC值/闪烁间隔ms等)

    // 运行时字段 (不持久化)
    unsigned long lastTriggerTime = 0;
    uint32_t triggerCount = 0;
};

// 配置文件路径
#define PERIPH_EXEC_CONFIG_FILE "/config/periph_exec.json"

#endif // PERIPHERAL_EXECUTION_H
