#ifndef PERIPH_EXEC_MANAGER_H
#define PERIPH_EXEC_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "PeripheralExecution.h"
#include "PeripheralManager.h"

class PeriphExecManager {
public:
    static PeriphExecManager& getInstance();

    // 禁止拷贝
    PeriphExecManager(const PeriphExecManager&) = delete;
    PeriphExecManager& operator=(const PeriphExecManager&) = delete;

    // 初始化（加载配置）
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

    // 定时器检查（由 TaskManager 定时任务调用）
    void checkTimers();

private:
    PeriphExecManager() = default;

    std::map<String, PeriphExecRule> rules;
    unsigned long lastTimerCheck = 0;

    // 条件评估
    bool evaluateCondition(const String& value, uint8_t op, const String& compareValue);

    // 动作执行
    bool executeAction(PeriphExecRule& rule);
    bool executePeripheralAction(const PeriphExecRule& rule);
    bool executeSystemAction(const PeriphExecRule& rule);

    // ID 生成
    String generateUniqueId();
};

#endif // PERIPH_EXEC_MANAGER_H
