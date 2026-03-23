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

    // 数据下发命令处理：匹配 targetPeriphId == item["id"]，执行规则并返回上报 JSON
    // 注意：此方法始终同步执行，立即返回结果
    String handleDataCommand(const String& message);

    // 定时器检查（由 TaskManager 定时任务调用）
    void checkTimers();

    // 设备触发检测（轮询输入外设状态，由 TaskManager 定时任务调用）
    void checkDeviceTriggers();

    // Modbus 一次性读取回调（由 ProtocolManager 注册）
    void setModbusReadCallback(std::function<String(const String&)> callback);

    // ========== 异步执行 ==========

    // 记录异步执行结果（由异步任务完成时调用）
    void recordResult(const AsyncExecResult& result);

    // 获取最近的执行结果（最多保留 MAX_ASYNC_TASKS * 2 条）
    std::vector<AsyncExecResult> getRecentResults();

private:
    PeriphExecManager() = default;

    // ========== 规则存储 ==========
    std::map<String, PeriphExecRule> rules;
    unsigned long lastTimerCheck = 0;
    unsigned long _lastDeviceCheck = 0;

    // ========== FreeRTOS 同步原语 ==========
    SemaphoreHandle_t _rulesMutex = nullptr;       // 保护 rules map 的互斥量
    SemaphoreHandle_t _taskSlotSemaphore = nullptr; // 并发任务槽计数信号量
    SemaphoreHandle_t _resultsMutex = nullptr;      // 保护 executionResults 的互斥量

    // ========== 异步执行结果 ==========
    std::vector<AsyncExecResult> executionResults;

    // ========== Modbus 读取回调 ==========
    std::function<String(const String&)> _modbusReadCallback;

    // ========== 条件评估 ==========
    bool evaluateCondition(const String& value, uint8_t op, const String& compareValue);

    // ========== 动作执行（同步） ==========
    bool executeAction(PeriphExecRule& rule);
    bool executePeripheralAction(const PeriphExecRule& rule);
    bool executeSystemAction(const PeriphExecRule& rule);
    bool executeScriptAction(const PeriphExecRule& rule);

    // ========== 异步调度 ==========

    // 异步分发：创建 FreeRTOS 任务执行规则，失败则回退同步
    void dispatchAsync(const PeriphExecRule& rule);

    // FreeRTOS 任务入口函数（静态）
    static void asyncExecTaskFunc(void* pvParameters);

    // 判断是否应使用异步执行（堆内存 >= MIN_HEAP_FOR_ASYNC 且有空闲任务槽）
    bool shouldRunAsync() const;

    // 获取 MQTTClient 指针
    MQTTClient* getMqttClient();

    // ID 生成
    String generateUniqueId();
};

#endif // PERIPH_EXEC_MANAGER_H
