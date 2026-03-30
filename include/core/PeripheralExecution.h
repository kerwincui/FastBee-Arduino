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
    ACTION_CALL_PERIPHERAL = 12,  // 调用其他外设
    ACTION_HIGH_INVERTED = 13,    // 设置高电平(反转) - 物理输出低电平
    ACTION_LOW_INVERTED = 14,     // 设置低电平(反转) - 物理输出高电平
    ACTION_SCRIPT = 15            // 命令序列脚本
};

// 触发类型
enum class ExecTriggerType : uint8_t {
    PLATFORM_TRIGGER = 0,          // 平台触发（IoT平台MQTT指令下发）
    TIMER_TRIGGER = 1,             // 定时触发
    DEVICE_TRIGGER = 2,            // 设备触发（按键/触摸屏/光电开关等本地输入）
    DATA_RECEIVE = 3,              // 数据接收触发（协议数据到达时应用模板转换）
    DATA_REPORT = 4,               // 数据上报触发（协议数据发送前应用模板转换）
    SYSTEM_EVENT_TRIGGER = 5,      // 系统事件触发（WiFi/MQTT/NTP等系统事件）
    BUTTON_EVENT_TRIGGER = 6       // 按键事件触发（单击/双击/长按等按键事件）
};

// 系统事件类型枚举
enum class SystemEventType : uint8_t {
    NONE = 0,
    // WiFi 事件
    SYS_WIFI_CONNECTED = 1,            // WiFi连接成功
    SYS_WIFI_DISCONNECTED = 2,         // WiFi断开连接
    SYS_WIFI_CONN_FAILED = 3,          // WiFi连接失败
    // MQTT 事件
    SYS_MQTT_CONNECTED = 10,           // MQTT连接成功
    SYS_MQTT_DISCONNECTED = 11,        // MQTT断开连接
    SYS_MQTT_CONN_FAILED = 12,         // MQTT连接失败
    SYS_MQTT_ENABLED = 13,             // MQTT协议启用
    // 网络模式事件
    SYS_NET_MODE_AP = 20,              // 网络模式切换为AP
    SYS_NET_MODE_STA = 21,             // 网络模式切换为STA
    SYS_NET_MODE_AP_STA = 22,          // 网络模式切换为AP+STA
    // 协议事件
    SYS_MODBUS_RTU_ENABLED = 30,       // Modbus RTU启用
    SYS_MODBUS_TCP_ENABLED = 31,       // Modbus TCP启用
    SYS_TCP_ENABLED = 32,              // TCP协议启用
    SYS_HTTP_ENABLED = 33,             // HTTP协议启用
    SYS_COAP_ENABLED = 34,             // CoAP协议启用
    // 系统服务事件
    SYS_NTP_SYNCED = 40,               // NTP时间同步完成
    SYS_OTA_START = 41,                // OTA升级开始
    SYS_OTA_SUCCESS = 42,              // OTA升级成功
    SYS_OTA_FAILED = 43,               // OTA升级失败
    // 配网事件
    SYS_AP_PROVISION_START = 50,       // AP配网开始
    SYS_AP_PROVISION_DONE = 51,        // AP配网完成
    SYS_BLE_PROVISION_START = 52,      // 蓝牙配网开始
    SYS_BLE_PROVISION_DONE = 53,       // 蓝牙配网完成
    // 规则引擎事件
    SYS_RULE_EXEC_TIME = 60,           // 规则脚本执行时间（用于监控）
    SYS_RULE_EXEC_ERROR = 61,          // 规则脚本执行错误
    // 系统状态事件
    SYS_SYSTEM_BOOT = 70,              // 系统启动
    SYS_SYSTEM_READY = 71,             // 系统就绪
    SYS_SYSTEM_ERROR = 72,             // 系统错误
    SYS_FACTORY_RESET = 73,            // 恢复出厂设置
    
    // 按键事件 (80-89) - 用于数字输入上拉/下拉类型的按键检测
    SYS_BUTTON_CLICK = 80,             // 按键单击
    SYS_BUTTON_DOUBLE_CLICK = 81,      // 按键双击
    SYS_BUTTON_LONG_PRESS_2S = 82,     // 按键长按2秒
    SYS_BUTTON_LONG_PRESS_5S = 83,     // 按键长按5秒
    SYS_BUTTON_LONG_PRESS_10S = 84,    // 按键长按10秒
    SYS_BUTTON_PRESS = 85,             // 按键按下
    SYS_BUTTON_RELEASE = 86            // 按键释放
};

// 系统事件定义结构体
struct SystemEventDef {
    SystemEventType type;
    const char* id;          // 事件ID（用于配置匹配）
    const char* name;        // 显示名称
    const char* category;    // 分类
};

// 预定义的系统事件列表
static const SystemEventDef SYSTEM_EVENTS[] = {
    // WiFi 事件
    {SystemEventType::SYS_WIFI_CONNECTED, "sys_wifi_connected", "WiFi连接成功", "WiFi"},
    {SystemEventType::SYS_WIFI_DISCONNECTED, "sys_wifi_disconnected", "WiFi断开连接", "WiFi"},
    {SystemEventType::SYS_WIFI_CONN_FAILED, "sys_wifi_conn_failed", "WiFi连接失败", "WiFi"},
    // MQTT 事件
    {SystemEventType::SYS_MQTT_CONNECTED, "sys_mqtt_connected", "MQTT连接成功", "MQTT"},
    {SystemEventType::SYS_MQTT_DISCONNECTED, "sys_mqtt_disconnected", "MQTT断开连接", "MQTT"},
    {SystemEventType::SYS_MQTT_CONN_FAILED, "sys_mqtt_conn_failed", "MQTT连接失败", "MQTT"},
    {SystemEventType::SYS_MQTT_ENABLED, "sys_mqtt_enabled", "MQTT协议启用", "MQTT"},
    // 网络模式事件
    {SystemEventType::SYS_NET_MODE_AP, "sys_net_mode_ap", "网络模式切换为AP", "网络"},
    {SystemEventType::SYS_NET_MODE_STA, "sys_net_mode_sta", "网络模式切换为STA", "网络"},
    {SystemEventType::SYS_NET_MODE_AP_STA, "sys_net_mode_ap_sta", "网络模式切换为AP+STA", "网络"},
    // 协议事件
    {SystemEventType::SYS_MODBUS_RTU_ENABLED, "sys_modbus_rtu_enabled", "Modbus RTU启用", "协议"},
    {SystemEventType::SYS_MODBUS_TCP_ENABLED, "sys_modbus_tcp_enabled", "Modbus TCP启用", "协议"},
    {SystemEventType::SYS_TCP_ENABLED, "sys_tcp_enabled", "TCP协议启用", "协议"},
    {SystemEventType::SYS_HTTP_ENABLED, "sys_http_enabled", "HTTP协议启用", "协议"},
    {SystemEventType::SYS_COAP_ENABLED, "sys_coap_enabled", "CoAP协议启用", "协议"},
    // 系统服务事件
    {SystemEventType::SYS_NTP_SYNCED, "sys_ntp_synced", "NTP时间同步完成", "系统"},
    {SystemEventType::SYS_OTA_START, "sys_ota_start", "OTA升级开始", "系统"},
    {SystemEventType::SYS_OTA_SUCCESS, "sys_ota_success", "OTA升级成功", "系统"},
    {SystemEventType::SYS_OTA_FAILED, "sys_ota_failed", "OTA升级失败", "系统"},
    // 配网事件
    {SystemEventType::SYS_AP_PROVISION_START, "sys_ap_provision_start", "AP配网开始", "配网"},
    {SystemEventType::SYS_AP_PROVISION_DONE, "sys_ap_provision_done", "AP配网完成", "配网"},
    {SystemEventType::SYS_BLE_PROVISION_START, "sys_ble_provision_start", "蓝牙配网开始", "配网"},
    {SystemEventType::SYS_BLE_PROVISION_DONE, "sys_ble_provision_done", "蓝牙配网完成", "配网"},
    // 规则引擎事件
    {SystemEventType::SYS_RULE_EXEC_TIME, "sys_rule_exec_time", "规则脚本执行时间", "规则"},
    {SystemEventType::SYS_RULE_EXEC_ERROR, "sys_rule_exec_error", "规则脚本执行错误", "规则"},
    // 系统状态事件
    {SystemEventType::SYS_SYSTEM_BOOT, "sys_boot", "系统启动", "系统"},
    {SystemEventType::SYS_SYSTEM_READY, "sys_ready", "系统就绪", "系统"},
    {SystemEventType::SYS_SYSTEM_ERROR, "sys_error", "系统错误", "系统"},
    {SystemEventType::SYS_FACTORY_RESET, "sys_factory_reset", "恢复出厂设置", "系统"},
    // 按键事件
    {SystemEventType::SYS_BUTTON_CLICK, "sys_button_click", "按键单击", "按键"},
    {SystemEventType::SYS_BUTTON_DOUBLE_CLICK, "sys_button_double_click", "按键双击", "按键"},
    {SystemEventType::SYS_BUTTON_LONG_PRESS_2S, "sys_button_long_press_2s", "按键长按2秒", "按键"},
    {SystemEventType::SYS_BUTTON_LONG_PRESS_5S, "sys_button_long_press_5s", "按键长按5秒", "按键"},
    {SystemEventType::SYS_BUTTON_LONG_PRESS_10S, "sys_button_long_press_10s", "按键长按10秒", "按键"},
    {SystemEventType::SYS_BUTTON_PRESS, "sys_button_press", "按键按下", "按键"},
    {SystemEventType::SYS_BUTTON_RELEASE, "sys_button_release", "按键释放", "按键"},
    // 终止标记
    {SystemEventType::NONE, nullptr, nullptr, nullptr}
};

// 获取系统事件数量
inline size_t getSystemEventCount() {
    size_t count = 0;
    while (SYSTEM_EVENTS[count].id != nullptr) count++;
    return count;
}

// 根据ID查找系统事件
inline const SystemEventDef* findSystemEvent(const char* id) {
    for (size_t i = 0; SYSTEM_EVENTS[i].id != nullptr; i++) {
        if (strcmp(SYSTEM_EVENTS[i].id, id) == 0) {
            return &SYSTEM_EVENTS[i];
        }
    }
    return nullptr;
}

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

// 外设执行结构体
struct PeriphExecRule {
    String id;                  // 唯一标识 (exec_<millis>)
    String name;                // 显示名称
    bool enabled = true;        // 启用状态
    uint8_t triggerType = 0;    // 触发类型: 0=平台触发, 1=定时触发, 2=设备触发, 3=数据接收, 4=数据上报, 5=系统事件触发, 6=按键事件触发
    uint8_t execMode = 0;       // 0=异步执行(默认), 1=同步执行

    // 平台触发/设备触发 共用字段
    uint8_t operatorType = 0;   // ExecOperator 枚举值（输入类型外设不使用）
    String compareValue;        // 比较值（输入类型外设不使用）

    // 设备触发专用字段
    String sourcePeriphId;      // 触发源外设 ID (监听哪个输入外设的状态变化)

    // 系统事件触发专用字段 (triggerType=5)
    String systemEventId;       // 系统事件ID (如 "sys_wifi_connected")
    uint8_t systemEventType = 0;// SystemEventType 枚举值（运行时缓存）

    // 按键事件触发专用字段 (triggerType=6)
    // 使用 sourcePeriphId 指定按键外设
    // 使用 systemEventId 指定按键事件类型 (如 "sys_button_click")
    // 使用 systemEventType 缓存按键事件类型

    // 定时触发字段
    uint8_t timerMode = 0;      // 0=间隔, 1=每日时间点
    uint32_t intervalSec = 60;  // 间隔秒数
    String timePoint;           // HH:MM 格式

    // 数据转换管道字段 (triggerType 3/4)
    uint8_t protocolType = 0;      // ExecProtocolType 枚举值
    String scriptContent;           // 纯文本模板 (${key} 占位符)

    // 动作字段
    String targetPeriphId;      // 目标外设 ID (系统功能时可为空，输入类型外设不应使用GPIO输出动作)
    uint8_t actionType = 0;     // ExecActionType 枚举值
    String actionValue;         // 动作参数 (PWM值/DAC值/闪烁间隔ms等)
    bool inverted = false;      // 电平反转 (GPIO动作时有效)

    // 运行时字段 (不持久化)
    unsigned long lastTriggerTime = 0;
    uint32_t triggerCount = 0;
};

// 配置文件路径
#define PERIPH_EXEC_CONFIG_FILE "/config/periph_exec.json"

#endif // PERIPHERAL_EXECUTION_H
