// Rule snapshots, migration references and indexes. Lock ownership remains at
// the original API boundaries; no configuration writes or execution occur here.
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#include "core/PeriphExecRemoteBindings.h"
#include <algorithm>
#include <new>

PeriphExecRule* PeriphExecManager::getRule(const String& id) {
    // Internal zero-copy accessor. Callers must already hold _rulesMutex.
    auto it = rules.find(id);
    return (it != rules.end()) ? &it->second : nullptr;
}

bool PeriphExecManager::getRuleSnapshot(const String& id, PeriphExecRule& snapshot) const {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    auto it = rules.find(id);
    if (it == rules.end()) return false;
    snapshot = it->second;
    return true;
}

std::vector<PeriphExecRule> PeriphExecManager::getAllRules() const {
    MutexGuard lock(_rulesMutex);
    std::vector<PeriphExecRule> result;
    if (!lock.isLocked()) return result;
    result.reserve(rules.size());
    for (const auto& pair : rules) {
        result.push_back(pair.second);
    }
    return result;
}

size_t PeriphExecManager::getRuleCount() const {
    MutexGuard lock(_rulesMutex);
    return lock.isLocked() ? rules.size() : 0;
}

bool PeriphExecManager::getWeatherRuleReferences(std::vector<String>& references, bool reserve) const {
    references.clear();
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked() || _configurationLoadFailed || _configurationRestartPending || _weatherReferenceLease) return false;
    try {
        const auto append = [&](const String& reference) {
            if (!reference.startsWith("weather.")) return;
            if (std::find(references.begin(),references.end(),reference)==references.end())
                references.push_back(reference);
        };
        for (const auto& pair : rules) {
            // Disabled rules still retain their bindings for later re-enable.
            for (const auto& trigger : pair.second.triggers) {
                append(trigger.triggerPeriphId);
                if (trigger.eventId.startsWith("ds:")) append(trigger.eventId.substring(3));
            }
            for (const auto& action : pair.second.actions) append(action.targetPeriphId);
        }
    } catch (const std::bad_alloc&) {
        references.clear(); return false;
    }
    if (reserve) _weatherReferenceLease = true;
    return true;
}

void PeriphExecManager::releaseWeatherRuleReferences() {
    MutexGuard lock(_rulesMutex, portMAX_DELAY);
    if (lock.isLocked()) _weatherReferenceLease = false;
}

bool PeriphExecManager::enableRule(const String& id) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    String errorMsg;
    const bool enabled = setRuleEnabledLocked(id, true, errorMsg);
    if (enabled) refreshRuleRuntimeIndexesLocked();
    return enabled;
}

bool PeriphExecManager::disableRule(const String& id) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    String errorMsg;
    const bool disabled = setRuleEnabledLocked(id, false, errorMsg);
    if (disabled) refreshRuleRuntimeIndexesLocked();
    return disabled;
}

bool PeriphExecManager::setRuleEnabledLocked(const String& id, bool enabled,
                                            String& errorMsg) {
    if (const char* issue = configurationWriteError()) { errorMsg=issue; return false; }
    auto it = rules.find(id);
    if (it == rules.end()) {
        errorMsg = "Rule not found";
        return false;
    }
    if (enabled) for (const auto& action : it->second.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
            errorMsg = "REMOTE_CHANNEL_RETIRED: rule remains paused"; return false;
        }
    }
    if (enabled) for (const auto& trigger : it->second.triggers) {
        if (hasRetiredRemoteSource(trigger)) { errorMsg = "REMOTE_CHANNEL_RETIRED: rule remains paused"; return false; }
    }
    it->second.enabled = enabled;
    return true;
}

bool PeriphExecManager::hasButtonEventRule(const String& periphId, const String& eventId) {
    // 无锁查询缓存（仅主循环线程调用，线程安全）
    // 缓存中存储了 eventId（兼容任意按键）和 "periphId:eventId"（指定按键）
    return _buttonEventCache.count(eventId) > 0 ||
           _buttonEventCache.count(periphId + ":" + eventId) > 0;
}

bool PeriphExecManager::hasDataSourceRule(const String& sourceId) {
    if (sourceId.isEmpty()) return false;
    // O(log n) 查找缓存（无锁，仅主循环线程调用）
    return _dataSourceCache.count(sourceId) > 0;
}

void PeriphExecManager::rebuildButtonEventCache() {
    // 在已持有 _rulesMutex 的上下文中调用，或在初始化时调用
    _buttonEventCache.clear();
    for (const auto& pair : rules) {
        const PeriphExecRule& rule = pair.second;
        if (!rule.enabled) continue;
        for (const auto& trigger : rule.triggers) {
            if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
            if (!trigger.eventId.startsWith("button_")) continue;
            if (trigger.triggerPeriphId.isEmpty()) {
                // 兼容任意按键
                _buttonEventCache.insert(trigger.eventId);
            } else {
                // 指定按键外设
                _buttonEventCache.insert(trigger.triggerPeriphId + ":" + trigger.eventId);
            }
        }
    }
}

void PeriphExecManager::rebuildDataSourceCache() {
    // 在已持有 _rulesMutex 的上下文中调用，或在初始化时调用
    _dataSourceCache.clear();
    for (const auto& pair : rules) {
        const PeriphExecRule& rule = pair.second;
        if (!rule.enabled) continue;
        for (const auto& trigger : rule.triggers) {
            if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                trigger.eventId.startsWith("ds:")) {
                // ds:sourceId 格式，提取 sourceId
                _dataSourceCache.insert(trigger.eventId.substring(3));
            }
            if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER) &&
                !trigger.triggerPeriphId.isEmpty()) {
                _dataSourceCache.insert(trigger.triggerPeriphId);
            }
        }
    }
}


#endif // FASTBEE_ENABLE_PERIPH_EXEC
