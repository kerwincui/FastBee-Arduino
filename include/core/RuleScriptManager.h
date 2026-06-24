#ifndef RULE_SCRIPT_MANAGER_H
#define RULE_SCRIPT_MANAGER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

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
    // 零拷贝迭代用：调用方须自行持 getMutex() 保证线程安全
    const std::map<String, RuleScript>& getRulesRef() const { return _rules; }
    SemaphoreHandle_t getMutex() const { return _mutex; }
    bool enableRule(const String& id);
    bool disableRule(const String& id);
    size_t getRuleCount() const { return _rules.size(); }

    // ========== 持久化 ==========
    bool saveConfiguration();
    bool loadConfiguration();

    // ========== 数据转换管道 ==========

    // 数据接收转换（由各协议接收回调调用）
    String applyReceiveTransform(uint8_t protocolType, const String& rawData);

    // 主题感知接收转换（MQTT 专用）：
    //   - 按 protocolType + sourceTopic 匹配规则
    //   - 返回转换后 payload
    //   - 若匹配规则含 targetTopic，则写入 *outTargetTopic（调用方负责重定向发布）
    String applyReceiveTransform(uint8_t protocolType, const String& rawData,
                                 const String& topic, String* outTargetTopic);

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

    // MQTT 主题匹配（支持 + 单级通配符和 # 多级通配符）
    static bool matchTopic(const String& pattern, const String& topic);

    // ID 生成
    String generateUniqueId();
};

#endif // FASTBEE_ENABLE_RULE_SCRIPT

#endif // RULE_SCRIPT_MANAGER_H
