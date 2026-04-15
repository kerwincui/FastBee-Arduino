#ifndef PERIPHERAL_EXECUTION_H
#define PERIPHERAL_EXECUTION_H

#include <Arduino.h>
#include <vector>

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
    ACTION_CALL_PERIPHERAL = 12,  // 调用其他外设
    ACTION_HIGH_INVERTED = 13,    // 设置高电平(反转) - 物理输出低电平
    ACTION_LOW_INVERTED = 14,     // 设置低电平(反转) - 物理输出高电平
    ACTION_SCRIPT = 15,           // 命令序列脚本
    ACTION_MODBUS_COIL_WRITE = 16,  // Modbus 线圈写入 (FC05)
    ACTION_MODBUS_REG_WRITE = 17,   // Modbus 寄存器写入 (FC06)
    ACTION_MODBUS_POLL = 18,        // Modbus 轮询子设备采集 (由 PeriphExec 调度)
    ACTION_SENSOR_READ = 19         // 传感器数据读取 (模拟/数字/脉冲)
};

// 传感器类别枚举
enum class SensorCategory : uint8_t {
    SENSOR_ANALOG = 0,    // 模拟输入 (ADC, GPIO_ANALOG_INPUT)
    SENSOR_DIGITAL = 1,   // 数字输入 (GPIO_DIGITAL_INPUT, PULLUP, PULLDOWN)
    SENSOR_PULSE = 2      // 脉冲/频率 (ENCODER, 预留)
};

// 触发类型
enum class ExecTriggerType : uint8_t {
    PLATFORM_TRIGGER = 0,          // 平台触发（IoT平台MQTT指令下发）
    TIMER_TRIGGER = 1,             // 定时触发
    // DATA_RECEIVE = 2 和 DATA_REPORT = 3 已弃用，统一使用 EVENT_TRIGGER
    EVENT_TRIGGER = 4,             // 触发事件（WiFi/MQTT/NTP/按键/数据/外设执行等事件）
    POLL_TRIGGER = 5               // 轮询触发（本地数据源条件评估，如Modbus传感器周期数据）
};

// 触发事件类型枚举
enum class EventType : uint8_t {
    NONE = 0,
    // WiFi 事件 (1-9)
    EVENT_WIFI_CONNECTED = 1,            // WiFi连接成功
    EVENT_WIFI_DISCONNECTED = 2,         // WiFi断开连接
    EVENT_WIFI_CONN_FAILED = 3,          // WiFi连接失败
    // MQTT 事件 (10-19)
    EVENT_MQTT_CONNECTED = 10,           // MQTT连接成功
    EVENT_MQTT_DISCONNECTED = 11,        // MQTT断开连接
    EVENT_MQTT_CONN_FAILED = 12,         // MQTT连接失败
    EVENT_MQTT_ENABLED = 13,             // MQTT协议启用
    // 网络模式事件 (20-29)
    EVENT_NET_MODE_AP = 20,              // 网络模式切换为AP
    EVENT_NET_MODE_STA = 21,             // 网络模式切换为STA
    EVENT_NET_MODE_AP_STA = 22,          // 网络模式切换为AP+STA
    // 协议事件 (30-39)
    EVENT_MODBUS_RTU_ENABLED = 30,       // Modbus RTU启用
    EVENT_MODBUS_TCP_ENABLED = 31,       // Modbus TCP启用
    EVENT_TCP_ENABLED = 32,              // TCP协议启用
    EVENT_HTTP_ENABLED = 33,             // HTTP协议启用
    EVENT_COAP_ENABLED = 34,             // CoAP协议启用
    // 系统服务事件 (40-49)
    EVENT_NTP_SYNCED = 40,               // NTP时间同步完成
    EVENT_OTA_START = 41,                // OTA升级开始
    EVENT_OTA_SUCCESS = 42,              // OTA升级成功
    EVENT_OTA_FAILED = 43,               // OTA升级失败
    // 配网事件 (50-59)
    EVENT_AP_PROVISION_START = 50,       // AP配网开始
    EVENT_AP_PROVISION_DONE = 51,        // AP配网完成
    EVENT_BLE_PROVISION_START = 52,      // 蓝牙配网开始
    EVENT_BLE_PROVISION_DONE = 53,       // 蓝牙配网完成
    // 规则引擎事件 (60-69)
    EVENT_RULE_EXEC_TIME = 60,           // 规则脚本执行时间（用于监控）
    EVENT_RULE_EXEC_ERROR = 61,          // 规则脚本执行错误
    // 系统状态事件 (70-79)
    EVENT_SYSTEM_BOOT = 70,              // 系统启动
    EVENT_SYSTEM_READY = 71,             // 系统就绪
    EVENT_SYSTEM_ERROR = 72,             // 系统错误
    EVENT_FACTORY_RESET = 73,            // 恢复出厂设置
    
    // 按键事件 (80-89) - 用于数字输入上拉/下拉类型的按键检测
    EVENT_BUTTON_CLICK = 80,             // 按键单击
    EVENT_BUTTON_DOUBLE_CLICK = 81,      // 按键双击
    EVENT_BUTTON_LONG_PRESS_2S = 82,     // 按键长按2秒
    EVENT_BUTTON_LONG_PRESS_5S = 83,     // 按键长按5秒
    EVENT_BUTTON_LONG_PRESS_10S = 84,    // 按键长按10秒
    EVENT_BUTTON_PRESS = 85,             // 按键按下
    EVENT_BUTTON_RELEASE = 86,           // 按键释放
    
    // 外设执行事件 (90-99) - 当外设执行规则被执行时触发
    EVENT_PERIPH_EXEC_COMPLETED = 90,    // 外设执行完成
    
    // 数据事件 (100-109) - 协议数据收发事件
    EVENT_DATA_RECEIVE = 100,            // 数据接收（协议数据到达）
    EVENT_DATA_REPORT = 101              // 数据上报（协议数据发送）
};

// 触发事件定义结构体
struct EventDef {
    EventType type;
    const char* id;          // 事件ID（用于配置匹配）
    const char* name;        // 显示名称
    const char* category;    // 分类
};

// 预定义的触发事件列表（静态事件）— 定义在 PeripheralExecution.cpp
extern const EventDef STATIC_EVENTS[];
extern const size_t STATIC_EVENTS_COUNT;

// 获取静态事件数量
size_t getStaticEventCount();

// 根据ID查找静态事件
const EventDef* findStaticEvent(const char* id);

// 协议类型
enum class ExecProtocolType : uint8_t {
    PROTOCOL_MQTT = 0,
    PROTOCOL_MODBUS_RTU = 1,
    PROTOCOL_MODBUS_TCP = 2,
    PROTOCOL_HTTP = 3,
    PROTOCOL_COAP = 4,
    PROTOCOL_TCP = 5
};

// 定时模式
enum class ExecTimerMode : uint8_t {
    INTERVAL = 0,    // 间隔触发
    DAILY_TIME = 1   // 每日时间点
};

// 执行模式
enum class ExecMode : uint8_t {
    EXEC_ASYNC = 0,  // 异步执行（FreeRTOS 任务，不阻塞主循环，默认）
    EXEC_SYNC  = 1   // 同步执行（阻塞主循环，适用于需要立即完成的简单动作）
};

// 每条规则的最大触发器和动作数量
#define MAX_TRIGGERS_PER_RULE 3
#define MAX_ACTIONS_PER_RULE  4

// 触发器结构体（一条规则可包含多个触发器，OR 关系）
struct ExecTrigger {
    uint8_t triggerType = 0;        // ExecTriggerType 枚举值: 0=平台触发, 1=定时触发, 4=事件触发
    String triggerPeriphId;         // 数据源外设 ID（平台触发时匹配 MQTT/Modbus 消息的 item.id）
    uint8_t operatorType = 0;       // ExecOperator 枚举值（平台触发时的条件运算符）
    String compareValue;            // 比较值（平台触发时的条件值）
    uint8_t timerMode = 0;          // 定时模式: 0=间隔触发, 1=每日时间点
    uint32_t intervalSec = 60;      // 间隔秒数（定时触发用）
    String timePoint;               // HH:MM 格式时间点（定时触发用）
    String eventId;                 // 事件 ID（事件触发时使用，如 "wifi_connected"）
    // 轮询触发通信参数（仅 POLL_TRIGGER 使用）
    uint16_t pollResponseTimeout = 1000;  // Modbus 响应超时(ms)
    uint8_t  pollMaxRetries = 2;          // 最大重试次数
    uint16_t pollInterPollDelay = 100;    // 两次请求间最小间隔(ms)
    // 运行时字段（不持久化）
    unsigned long lastTriggerTime = 0;
    uint32_t triggerCount = 0;
};

// 动作结构体（一条规则可包含多个动作，顺序执行）
struct ExecAction {
    String targetPeriphId;          // 执行目标外设 ID（系统动作时可为空）
    uint8_t actionType = 0;         // ExecActionType 枚举值
    String actionValue;             // 动作参数（PWM值/DAC值/闪烁间隔ms/脚本内容等）
    bool useReceivedValue = false;  // 启用时：用触发接收到的值替代 actionValue
    uint16_t syncDelayMs = 0;       // 执行前延时（毫秒，用于多动作顺序编排，最大 10000）
    uint8_t execMode = 0;           // 0=异步执行(默认), 1=同步执行（per-action）
};

// 动作执行结果（用于精准上报）
struct ActionExecResult {
    String targetPeriphId;          // 目标外设 ID
    String actualValue;             // 执行后的实际值
    bool success = false;           // 是否成功
};

// 外设执行规则结构体
struct PeriphExecRule {
    String id;                  // 唯一标识 (exec_<millis>)
    String name;                // 显示名称
    bool enabled = true;        // 启用状态
    uint8_t execMode = 0;       // 0=异步执行(默认), 1=同步执行

    // 触发器列表（多个触发器之间为 OR 关系：任一匹配即触发）
    std::vector<ExecTrigger> triggers;

    // 动作列表（按顺序执行）
    std::vector<ExecAction> actions;

    // 数据转换管道字段
    uint8_t protocolType = 0;   // ExecProtocolType 枚举值
    String scriptContent;       // 纯文本模板 (${key} 占位符)

    // 数据上报控制
    bool reportAfterExec = true; // 执行完成后是否上报设备数据（默认启用）
};

// 配置文件路径
#define PERIPH_EXEC_CONFIG_FILE "/config/periph_exec.json"

#endif // PERIPHERAL_EXECUTION_H
