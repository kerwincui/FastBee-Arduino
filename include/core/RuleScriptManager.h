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
#include "core/FeatureInstanceRegistry.h"
#include "core/RuleScriptLifetime.h"
#include "core/RuleScriptResources.h"

class RuleScriptManager {
public:
    static RuleScriptManager& getInstance();
    FastBee::FeatureInstanceSnapshot observeFeatureInstance() const;

    RuleScriptManager(const RuleScriptManager&) = delete;
    RuleScriptManager& operator=(const RuleScriptManager&) = delete;

    // 初始化（加载配置、创建信号量）
    bool initialize();
    // Stops new access immediately. A busy holder performs final cleanup after
    // unlocking; false means cleanup remains pending, never forced deletion.
    bool stop();
    bool isRunning() const { return _lifetime.running(); }

    class AccessGuard {
    public:
        explicit AccessGuard(const RuleScriptManager& owner)
            : _owner(owner._lifetime.acquire() ? &owner : nullptr) {}
        ~AccessGuard() {
            if (_owner) {
                _owner->_lifetime.release();
                const_cast<RuleScriptManager*>(_owner)->cleanupIfIdle();
            }
        }
        AccessGuard(const AccessGuard&) = delete;
        AccessGuard& operator=(const AccessGuard&) = delete;
        bool admitted() const { return _owner != nullptr; }
        SemaphoreHandle_t mutex() const { return _owner ? _owner->_mutex : nullptr; }
    private:
        const RuleScriptManager* _owner;
    };

    // ========== CRUD ==========
    // Success means disk commit and live replacement both completed. Failure
    // retains the running map; response loss still requires reading back.
    bool addRule(const RuleScript& rule);
    bool updateRule(const String& id, const RuleScript& rule, const RuleScript* expected = nullptr);
    bool removeRule(const String& id);
    bool copyRule(const String& id, RuleScript& result) const;
    std::vector<RuleScript> getAllRules() const;
    // Zero-copy readers must hold AccessGuard, then its mutex. The guard must
    // outlive the lock and every use of this reference.
    using RuleMap = FastBee::RuleScriptMap<String, RuleScript>;
    const RuleMap& getRulesRef() const { return _rules; }
    bool enableRule(const String& id);
    bool disableRule(const String& id);
    size_t getRuleCount() const;

    // ========== 持久化 ==========
    bool saveConfiguration();
    bool loadConfiguration();
    bool importConfiguration(const JsonDocument& document, String& error);

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
    // Bounded queue admission: busy/allocation failure is rejection, not a raw
    // fallback. A successful output is frozen before leaving the caller.
    bool tryApplyReportTransform(uint8_t protocolType, const String& rawData,
                                 String& output, size_t maxBytes);

private:
    RuleScriptManager() = default;

    // 规则存储
    RuleMap _rules;
    // Caller holds _mutex. Null replacement removes an existing identity.
    bool commitRuleChange(const String& id, const RuleScript* replacement);

    // FreeRTOS 互斥量
    SemaphoreHandle_t _mutex = nullptr;
    mutable FastBee::RuleScriptLifetime _lifetime;
    std::atomic<bool> _startFailed{false};
    void cleanupIfIdle();
    bool loadConfigurationLocked();

    // 模板引擎
    static String applyTemplate(const String& templateStr, const String& jsonInput);

    // MQTT 主题匹配（支持 + 单级通配符和 # 多级通配符）
    static bool matchTopic(const String& pattern, const String& topic);

    // ID 生成
    String generateUniqueId();
};

#endif // FASTBEE_ENABLE_RULE_SCRIPT

#endif // RULE_SCRIPT_MANAGER_H
