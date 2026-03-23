#ifndef RULE_SCRIPT_MANAGER_H
#define RULE_SCRIPT_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "RuleScript.h"

class RuleScriptManager {
public:
    static RuleScriptManager& getInstance();

    RuleScriptManager(const RuleScriptManager&) = delete;
    RuleScriptManager& operator=(const RuleScriptManager&) = delete;

    // 初始化（加载配置、创建信号量）
    bool initialize();

    // ========== CRUD ==========
    bool addRule(const RuleScript& rule);
    bool updateRule(const String& id, const RuleScript& rule);
    bool removeRule(const String& id);
    RuleScript* getRule(const String& id);
    std::vector<RuleScript> getAllRules() const;
    bool enableRule(const String& id);
    bool disableRule(const String& id);
    size_t getRuleCount() const { return _rules.size(); }

    // ========== 持久化 ==========
    bool saveConfiguration();
    bool loadConfiguration();

    // ========== 数据转换管道 ==========

    // 数据接收转换（由各协议接收回调调用）
    String applyReceiveTransform(uint8_t protocolType, const String& rawData);

    // 数据上报转换（由各协议发送方法调用）
    String applyReportTransform(uint8_t protocolType, const String& rawData);

private:
    RuleScriptManager() = default;

    // 规则存储
    std::map<String, RuleScript> _rules;

    // FreeRTOS 互斥量
    SemaphoreHandle_t _mutex = nullptr;

    // 模板引擎
    static String applyTemplate(const String& templateStr, const String& jsonInput);

    // 默认示例数据
    void populateDefaults();

    // ID 生成
    String generateUniqueId();
};

#endif // RULE_SCRIPT_MANAGER_H
