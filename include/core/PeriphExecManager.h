#ifndef PERIPH_EXEC_MANAGER_H
#define PERIPH_EXEC_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "PeripheralExecution.h"
#include "PeripheralManager.h"
#include "ScriptEngine.h"
#include "AsyncExecTypes.h"

// ========== 触发事件配置结构体 ==========

// 按键事件检测参数（用于触发事件中的按键类型事件）
struct ButtonEventConfig {
    uint16_t debounceMs = 50;       // 消抖时间(ms)
    uint16_t clickIntervalMs = 300; // 双击间隔时间(ms)
    uint16_t longPress2sMs = 2000;  // 长按2秒阈值
    uint16_t longPress5sMs = 5000;  // 长按5秒阈值
    uint16_t longPress10sMs = 10000;// 长按10秒阈值
};

// 单个按键的运行时状态（用于触发事件中的按键类型事件）
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

class PeriphExecManager {
public:
    static PeriphExecManager& getInstance();

    // 禁止拷贝
    PeriphExecManager(const PeriphExecManager&) = delete;
    PeriphExecManager& operator=(const PeriphExecManager&) = delete;

    // 初始化（加载配置、创建信号量）
    bool initialize();

    // ========== CRUD ==========
    bool addRule(const PeriphExecRule& rule);
    bool updateRule(const String& id, const PeriphExecRule& rule);
    bool removeRule(const String& id);
    PeriphExecRule* getRule(const String& id);
    std::vector<PeriphExecRule> getAllRules() const;
    bool enableRule(const String& id);
    bool disableRule(const String& id);
    size_t getRuleCount() const { return rules.size(); }

    // ========== 持久化 ==========
    bool saveConfiguration();
    bool loadConfiguration();

    // ========== 执行引擎 ==========

    // MQTT 消息处理入口（由 ProtocolManager messageCallback 调用）
    void handleMqttMessage(const String& topic, const String& message);

    // 数据下发命令处理：匹配 trigger.triggerPeriphId == item["id"]，执行规则并返回上报 JSON
    // 注意：此方法始终同步执行，立即返回结果
    String handleDataCommand(const String& message);

    // 定时器检查（由 TaskManager 定时任务调用）
    void checkTimers();

    // ========== 触发事件 ==========

    // 触发事件（由各系统模块调用）
    void triggerEvent(EventType eventType, const String& eventData = "");

    // 触发事件（通过事件ID）
    void triggerEventById(const String& eventId, const String& eventData = "");

    // 获取静态事件列表（用于前端配置）
    static String getStaticEventsJson();

    // 获取动态事件列表（包含外设执行规则事件）
    String getDynamicEventsJson();

    // Modbus 一次性读取回调（由 ProtocolManager 注册）
    void setModbusReadCallback(std::function<String(const String&)> callback);

    // ========== 异步执行 ==========

    // 记录异步执行结果（由异步任务完成时调用）
    void recordResult(const AsyncExecResult& result);

    // 获取最近的执行结果（最多保留 MAX_ASYNC_TASKS * 2 条）
    std::vector<AsyncExecResult> getRecentResults();

    // ========== 手动执行 ==========

    // 手动执行规则（用于"执行一次"按钮）
    bool runOnce(const String& id);

    // ========== 动作执行后数据上报 ==========

    // 尝试上报设备数据（检查网络和协议连接状态后上报）
    // 返回：true=上报成功或无需上报，false=上报失败
    bool tryReportDeviceData();

    // ========== 触发事件检测 ==========

    // 按键事件检测（由 TaskManager 定时任务调用，用于检测按键类型事件）
    void checkButtonEvents();

    // 触发按键事件（内部使用）
    void triggerButtonEvent(const String& periphId, EventType eventType);

    // 触发外设执行事件（内部使用）
    void triggerPeriphExecEvent(const String& ruleId, const String& eventData = "");

    // ========== 配置辅助方法 ==========

    // 获取有效触发类型列表（JSON数组格式）
    static String getValidTriggerTypes();

    // 获取触发事件分类列表（JSON数组格式，用于前端下拉选择）
    static String getEventCategoriesJson();

    // 获取外设支持的有效动作类型列表（JSON数组格式）
    // periphId: 外设ID，为空则返回所有动作类型
    static String getValidActionTypes(const String& periphId = "");

private:
    PeriphExecManager() = default;

    // ========== 规则存储 ==========
    std::map<String, PeriphExecRule> rules;
    unsigned long lastTimerCheck = 0;
    unsigned long _lastButtonCheck = 0;

    // ========== FreeRTOS 同步原语 ==========
    SemaphoreHandle_t _rulesMutex = nullptr;       // 保护 rules map 的互斥量
    SemaphoreHandle_t _taskSlotSemaphore = nullptr; // 并发任务槽计数信号量
    SemaphoreHandle_t _resultsMutex = nullptr;      // 保护 executionResults 的互斥量

    // ========== 异步执行结果 ==========
    std::vector<AsyncExecResult> executionResults;

    // ========== Modbus 读取回调 ==========
    std::function<String(const String&)> _modbusReadCallback;

    // ========== 按键状态跟踪 ==========
    std::map<String, ButtonRuntimeState> buttonStates;
    ButtonEventConfig buttonConfig;

    // ========== 条件评估 ==========
    bool evaluateCondition(const String& value, uint8_t op, const String& compareValue);

    // ========== 动作执行（同步） ==========
    bool executeActionItem(const ExecAction& action, const String& effectiveValue);
    std::vector<ActionExecResult> executeAllActions(const PeriphExecRule& rule, const String& receivedValue);
    bool executePeripheralAction(const ExecAction& action, const String& effectiveValue);
    bool executeSystemAction(const ExecAction& action);
    bool executeScriptAction(const ExecAction& action);

    // ========== 动作执行后上报 ==========
    void reportActionResults(const std::vector<ActionExecResult>& results);

    // ========== 异步调度 ==========

    // 异步分发：创建 FreeRTOS 任务执行规则，失败则回退同步
    void dispatchAsync(const PeriphExecRule& rule, const String& receivedValue);

    // FreeRTOS 任务入口函数（静态）
    static void asyncExecTaskFunc(void* pvParameters);

    // 判断是否应使用异步执行（堆内存 >= MIN_HEAP_FOR_ASYNC 且有空闲任务槽）
    bool shouldRunAsync() const;

    // 获取 MQTTClient 指针
    MQTTClient* getMqttClient();

    // ========== 动作执行后数据上报辅助方法 ==========

    // 收集所有外设状态数据（JSON数组格式）
    String collectPeripheralData();

    // 检查网络和协议连接状态
    // connectedProtocols: 输出已连接的协议类型列表
    // 返回：是否有至少一个可用连接
    bool checkNetworkAndProtocolStatus(uint8_t& connectedProtocols);

    // ID 生成
    String generateUniqueId();
};

#endif // PERIPH_EXEC_MANAGER_H
