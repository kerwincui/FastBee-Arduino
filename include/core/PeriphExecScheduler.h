#ifndef PERIPH_EXEC_SCHEDULER_H
#define PERIPH_EXEC_SCHEDULER_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <set>
#include "PeripheralExecution.h"

// 前向声明
class PeriphExecManager;
class PeriphExecExecutor;
class MQTTClient;

// ========== 按键事件配置结构体 ==========
struct ButtonEventConfig {
    uint16_t debounceMs = 50;       // 消抖时间(ms)
    uint16_t clickIntervalMs = 300; // 双击间隔时间(ms)
    uint16_t longPress2sMs = 2000;  // 长按2秒阈值
    uint16_t longPress5sMs = 5000;  // 长按5秒阈值
    uint16_t longPress10sMs = 10000;// 长按10秒阈值
};

// ========== 单个按键的运行时状态 ==========
struct ButtonRuntimeState {
    String periphId;                // 外设ID
    bool lastState = true;          // 上一次状态（上拉默认高电平，按下为低）
    bool currentState = true;       // 当前状态
    unsigned long lastChangeTime = 0;  // 最后状态变化时间
    unsigned long pressStartTime = 0;  // 按下开始时间
    uint8_t clickCount = 0;         // 点击计数（用于双击检测）
    unsigned long lastClickTime = 0;   // 最后一次点击时间
    bool longPress2sTriggered = false; // 长按2秒已触发
    bool longPress5sTriggered = false; // 长按5秒已触发
    bool longPress10sTriggered = false;// 长按10秒已触发
};

// ========== 调度器类 ==========
// 负责定时触发、事件触发、MQTT消息处理、Modbus轮询数据处理、数据上报
class PeriphExecScheduler {
public:
    PeriphExecScheduler();
    ~PeriphExecScheduler();

    // 禁止拷贝
    PeriphExecScheduler(const PeriphExecScheduler&) = delete;
    PeriphExecScheduler& operator=(const PeriphExecScheduler&) = delete;

    // 初始化
    void initialize(PeriphExecManager* manager, PeriphExecExecutor* executor);

    // ========== 定时触发 ==========

    // 定时器检查（由 TaskManager 定时任务调用）
    void checkTimers();

    // ========== 事件触发 ==========

    // 触发事件（由各系统模块调用）
    void triggerEvent(EventType eventType, const String& eventData = "");

    // 触发事件（通过事件ID）
    void triggerEventById(const String& eventId, const String& eventData = "");

    // 触发外设执行事件（内部使用）
    void triggerPeriphExecEvent(const String& ruleId, const String& eventData = "");

    // ========== 消息/数据处理 ==========

    // MQTT 消息处理入口（由 ProtocolManager messageCallback 调用）
    void handleMqttMessage(const String& topic, const String& message);

    // 轮询数据处理入口（由 Modbus/其他本地数据源回调调用）
    void handlePollData(const String& source, const String& data);

    // 数据下发命令处理：匹配 trigger.triggerPeriphId == item["id"]，执行规则并返回上报 JSON
    // 注意：此方法始终同步执行，立即返回结果
    String handleDataCommand(const String& message);

    // ========== 数据上报 ==========

    // 尝试上报设备数据（检查网络和协议连接状态后上报）
    // 返回：true=上报成功或无需上报，false=上报失败
    bool tryReportDeviceData();

    // ========== 按键事件检测 ==========

    // 按键事件检测（由 TaskManager 定时任务调用，用于检测按键类型事件）
    void checkButtonEvents();

    // 触发按键事件（内部使用）
    void triggerButtonEvent(const String& periphId, EventType eventType);

    // ========== 配置辅助方法 ==========

    // 获取静态事件列表（用于前端配置）
    static String getStaticEventsJson();

    // 获取动态事件列表（包含外设执行规则事件）
    String getDynamicEventsJson();

    // 获取有效触发类型列表（JSON数组格式）
    static String getValidTriggerTypes();

    // 获取触发事件分类列表（JSON数组格式，用于前端下拉选择）
    static String getEventCategoriesJson();

private:
    PeriphExecManager* _manager = nullptr;
    PeriphExecExecutor* _executor = nullptr;

    // 定时器状态
    unsigned long _lastTimerCheck = 0;

    // 按键状态
    unsigned long _lastButtonCheck = 0;
    std::map<String, ButtonRuntimeState> _buttonStates;
    ButtonEventConfig _buttonConfig;

    // ========== 内部方法 ==========

    // 数据源类型枚举（用于区分 MQTT 消息和轮询数据）
    enum class DataSourceType : uint8_t { MQTT = 0, POLL = 1 };

    // 数据事件统一处理方法（提取公共 JSON 解析和规则匹配逻辑）
    void handleDataEvent(const String& source, const String& data, DataSourceType sourceType);

    // 条件评估（委托给 PeriphExecManager::evaluateCondition）
    static bool evaluateCondition(const String& value, uint8_t op, const String& compareValue);

    // 收集所有外设状态数据（JSON数组格式）
    String collectPeripheralData();

    // 检查网络和协议连接状态
    bool checkNetworkAndProtocolStatus(uint8_t& connectedProtocols);

    // 获取MQTTClient指针
    MQTTClient* getMqttClient();
};

#endif // PERIPH_EXEC_SCHEDULER_H
