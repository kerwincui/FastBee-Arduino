#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

#include "core/RuleScriptManager.h"
#include "core/AsyncExecTypes.h"
#include "core/CheckedReportTemplate.h"
#include "core/RuleScriptConfigValidation.h"
#include "core/RuntimeFeatureManager.h"
#include "systems/ConfigStorage.h"
#include "systems/LoggerSystem.h"

RuleScriptManager& RuleScriptManager::getInstance() {
    static RuleScriptManager instance;
    static const FastBee::FeatureInstanceObserver observer{"ruleScript", [](const void* context) {
        return static_cast<const RuleScriptManager*>(context)->observeFeatureInstance();
    }, &instance};
    FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    return instance;
}

FastBee::FeatureInstanceSnapshot RuleScriptManager::observeFeatureInstance() const {
    using namespace FastBee;
    if (_lifetime.retiring()) return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::CleanupPending);
    if (_lifetime.starting()) return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    if (_lifetime.running()) return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Initialized);
    if (_startFailed.load(std::memory_order_acquire))
        return FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::BackendUnavailable,
                                                   FeatureInstanceScope::ServiceLifecycle);
    return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Stopped);
}

bool RuleScriptManager::initialize() {
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ruleScript")) return false;
    if (_lifetime.running()) return true;
    if (!_lifetime.beginStart()) return false;
    _startFailed.store(false, std::memory_order_release);
    _mutex = FastBee::ruleScriptMemoryAvailable(256, 256) ? xSemaphoreCreateMutex() : nullptr;
    bool loaded = false;
    {
        MutexGuard lock(_mutex, 0);
        if (lock.isLocked()) loaded = loadConfigurationLocked();
    }
    if (loaded && FastBee::RuntimeFeatureManager::instance().shouldEnable("ruleScript") &&
        _lifetime.publishStart()) return true;
    // A concurrent stop cancels publication. Starting excludes every reader
    // and cleanup claimant until these unpublished allocations are destroyed.
    _rules.clear();
    if (_mutex) vSemaphoreDelete(_mutex);
    _mutex = nullptr;
    _startFailed.store(!loaded, std::memory_order_release);
    _lifetime.discardStart();
    return false;
}

void RuleScriptManager::cleanupIfIdle() {
    if (!_lifetime.claimCleanup()) return;
    _rules.clear();
    if (_mutex) vSemaphoreDelete(_mutex);
    _mutex = nullptr;
    _startFailed.store(false, std::memory_order_release);
    _lifetime.finishCleanup();
}

bool RuleScriptManager::stop() {
    _lifetime.requestStop();
    cleanupIfIdle();
    return _lifetime.idle();
}

// ========== CRUD（带互斥量保护） ==========

bool RuleScriptManager::addRule(const RuleScript& rule) {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;

    RuleScript newRule;
    if (!FastBee::copyRuleScriptConfig(rule, newRule)) return false;
    if (newRule.id.isEmpty()) {
        newRule.id = generateUniqueId();
    }
    if (_rules.count(newRule.id)) {
        LOGGER.warningf("[RuleScript] Rule ID already exists: %s", newRule.id.c_str());
        return false;
    }
    newRule.lastTriggerTime = 0;
    newRule.triggerCount = 0;
    if (!commitRuleChange(newRule.id, &newRule)) return false;
    LOGGER.infof("[RuleScript] Added rule: %s (%s)", newRule.id.c_str(), newRule.name.c_str());
    return true;
}

bool RuleScriptManager::updateRule(const String& id, const RuleScript& rule, const RuleScript* expected) {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;

    auto it = _rules.find(id);
    if (it == _rules.end()) {
        LOGGER.warningf("[RuleScript] Rule not found for update: %s", id.c_str());
        return false;
    }
    const auto& current = it->second;
    if (expected && (current.id != expected->id || current.name != expected->name ||
        current.enabled != expected->enabled || current.triggerType != expected->triggerType ||
        current.protocolType != expected->protocolType || current.scriptContent != expected->scriptContent ||
        current.sourceTopic != expected->sourceTopic || current.targetTopic != expected->targetTopic)) return false;
    RuleScript updated;
    if (!FastBee::copyRuleScriptConfig(rule, updated) ||
        !FastBee::copyCheckedText(updated.id, id.c_str(), id.length(), 40) ||
        !commitRuleChange(id, &updated)) return false;
    LOGGER.infof("[RuleScript] Updated rule: %s", id.c_str());
    return true;
}

bool RuleScriptManager::removeRule(const String& id) {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;

    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    if (!commitRuleChange(id, nullptr)) return false;
    LOGGER.infof("[RuleScript] Removed rule: %s", id.c_str());
    return true;
}

bool RuleScriptManager::copyRule(const String& id, RuleScript& result) const {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;
    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    RuleScript snapshot;
    if (!FastBee::copyRuleScriptConfig(it->second, snapshot)) return false;
    result = std::move(snapshot);
    return true;
}

std::vector<RuleScript> RuleScriptManager::getAllRules() const {
    std::vector<RuleScript> result;
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return result;
    result.reserve(_rules.size());
    for (const auto& pair : _rules) {
        result.push_back(pair.second);
    }
    return result;
}

size_t RuleScriptManager::getRuleCount() const {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    return lock.isLocked() ? _rules.size() : 0;
}

bool RuleScriptManager::enableRule(const String& id) {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;
    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    RuleScript updated;
    if (!FastBee::copyRuleScriptConfig(it->second, updated)) return false;
    updated.enabled = true;
    return commitRuleChange(id, &updated);
}

bool RuleScriptManager::disableRule(const String& id) {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;
    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    RuleScript updated;
    if (!FastBee::copyRuleScriptConfig(it->second, updated)) return false;
    updated.enabled = false;
    return commitRuleChange(id, &updated);
}

// ========== 持久化 ==========

bool RuleScriptManager::commitRuleChange(const String& id, const RuleScript* replacement) {
    if (id.isEmpty() || (replacement && replacement->id != id)) return false;
    if (_rules.size() + (replacement && !_rules.count(id) ? 1 : 0) > FastBee::RuleScriptImportMaxRules) return false;
    FastBee::RuleScriptJsonAllocator allocator;
    JsonDocument doc(&allocator);
    JsonArray rows = doc["rules"].to<JsonArray>();
    for (const auto& pair : _rules) {
        if (pair.first != id) FastBee::appendRuleScriptConfig(rows, pair.second);
    }
    if (replacement) FastBee::appendRuleScriptConfig(rows, *replacement);
    if (doc.overflowed() || measureJson(doc) > FastBee::RuleScriptImportMaxBytes) return false;
    if (!FastBee::ruleScriptCandidateMemory<RuleMap>(doc.as<JsonVariantConst>(), true)) return false;
    RuleMap candidate;
    String error;
    if (!FastBee::parseRuleScriptImport<RuleScript, String>(doc.as<JsonVariantConst>(), candidate, error)) return false;
    for (auto& pair : candidate) {
        const auto previous = _rules.find(pair.first);
        if (previous != _rules.end()) {
            pair.second.lastTriggerTime = previous->second.lastTriggerTime;
            pair.second.triggerCount = previous->second.triggerCount;
        }
    }
    if (!ConfigStorage::getInstance().saveImportedJSONConfig(RULE_SCRIPT_CONFIG_FILE, doc)) return false;
    _rules.swap(candidate);
    return true;
}

bool RuleScriptManager::saveConfiguration() {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;

    FastBee::RuleScriptJsonAllocator allocator;
    JsonDocument doc(&allocator);
    JsonArray arr = doc["rules"].to<JsonArray>();

    for (const auto& pair : _rules) {
        FastBee::appendRuleScriptConfig(arr, pair.second);
    }

    // Use the same single-file disk transaction as imports. A delayed cache
    // save must not later overwrite an imported generation.
    return !doc.overflowed() && measureJson(doc) <= FastBee::RuleScriptImportMaxBytes &&
        FastBee::ruleScriptMemoryAvailable(2048 + 2 * FastBee::ruleScriptJsonMemory(doc.as<JsonVariantConst>()), 2048) &&
        ConfigStorage::getInstance().saveImportedJSONConfig(RULE_SCRIPT_CONFIG_FILE, doc);
}

bool RuleScriptManager::importConfiguration(const JsonDocument& document, String& error) {
    AccessGuard access(*this);
    if (!access.admitted()) { error = "Script rules are unavailable; nothing was applied"; return false; }
    if (document.overflowed() || measureJson(document) > FastBee::RuleScriptImportMaxBytes) {
        error = "Script configuration is incomplete or exceeds 24KB";
        return false;
    }
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) { error = "Script rules are busy; nothing was applied"; return false; }
    if (!FastBee::ruleScriptCandidateMemory<RuleMap>(document.as<JsonVariantConst>(), true)) {
        error = "Insufficient internal memory for script candidate and disk verification; running rules retained";
        return false;
    }
    RuleMap candidate;
    if (!FastBee::parseRuleScriptImport<RuleScript, String>(document.as<JsonVariantConst>(), candidate, error)) return false;
    // No live map is changed while validation or disk commit can still fail.
    if (!ConfigStorage::getInstance().saveImportedJSONConfig(RULE_SCRIPT_CONFIG_FILE, document)) {
        error = "Script disk commit was not confirmed; running rules were retained";
        return false;
    }
    _rules.swap(candidate);
    return true;
}

bool RuleScriptManager::loadConfiguration() {
    AccessGuard access(*this);
    MutexGuard lock(access.mutex(), 0);
    if (!lock.isLocked()) return false;
    return loadConfigurationLocked();
}

bool RuleScriptManager::loadConfigurationLocked() {
    if (!LittleFS.exists(RULE_SCRIPT_CONFIG_FILE) && !LittleFS.exists(RULE_SCRIPT_CONFIG_FILE ".bak")) {
        LOGGER.info("[RuleScript] No config file found, creating empty config");
        FastBee::RuleScriptJsonAllocator allocator;
        JsonDocument doc(&allocator);
        doc["rules"].to<JsonArray>();
        if (doc.overflowed() || !FastBee::ruleScriptMemoryAvailable(4096, 2048) ||
            !ConfigStorage::getInstance().saveImportedJSONConfig(RULE_SCRIPT_CONFIG_FILE, doc)) return false;
        _rules.clear();
        return true;
    }
    // ConfigStorage's generic reader first builds an unconstrained temporary
    // DOM and copies it. Use its same primary/backup generations with a checked
    // allocator here, so loading cannot consume the network reserve.
    FastBee::RuleScriptJsonAllocator allocator;
    JsonDocument doc(&allocator);
    bool recovered = false, accepted = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        File file = LittleFS.open(slot ? RULE_SCRIPT_CONFIG_FILE ".bak" : RULE_SCRIPT_CONFIG_FILE, FILE_READ);
        if (!file) continue;
        if (file.size() > FastBee::RuleScriptImportMaxBytes) {
            file.close();
            LOGGER.error("[RuleScript] Configuration exceeds 24KB; files and running rules retained");
            return false;
        }
        doc.clear();
        const auto parseError = deserializeJson(doc, file);
        file.close();
        if (parseError == DeserializationError::NoMemory) {
            LOGGER.error("[RuleScript] Configuration parsing lacks internal memory; files retained");
            return false;
        }
        if (parseError || !doc.is<JsonObject>() || !doc.containsKey("rules") ||
            (!doc["rules"].isNull() && !doc["rules"].is<JsonArray>())) continue;
        recovered = slot != 0;
        accepted = true;
        break;
    }
    if (!accepted || doc.overflowed() || !doc.is<JsonObject>() || !doc.containsKey("rules") ||
        (!doc["rules"].isNull() && !doc["rules"].is<JsonArray>()) ||
        measureJson(doc) > FastBee::RuleScriptImportMaxBytes) return false;
    RuleMap candidate;
    String error;
    if (!FastBee::ruleScriptCandidateMemory<RuleMap>(doc.as<JsonVariantConst>(), false) ||
        !FastBee::parseRuleScriptImport<RuleScript, String>(doc.as<JsonVariantConst>(), candidate, error, true)) {
        LOGGER.error("[RuleScript] Invalid, oversized or memory-limited configuration; files and running rules retained");
        return false;
    }
    // Restore the accepted backup generation just as ConfigStorage does;
    // invalid/oversized candidates never modify either disk generation.
    if (recovered) {
        if (LittleFS.exists(RULE_SCRIPT_CONFIG_FILE) && !LittleFS.remove(RULE_SCRIPT_CONFIG_FILE)) return false;
        if (!LittleFS.rename(RULE_SCRIPT_CONFIG_FILE ".bak", RULE_SCRIPT_CONFIG_FILE)) return false;
    }
    _rules.swap(candidate);
    LOGGER.infof("[RuleScript] Loaded %d rules", (int)_rules.size());
    return true;
}

// ========== 模板引擎 ==========

String RuleScriptManager::applyTemplate(const String& templateStr, const String& jsonInput) {
    if (templateStr.isEmpty() || jsonInput.isEmpty()) return jsonInput;

    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, jsonInput.c_str(), jsonInput.length());
    if (err) return jsonInput;

    struct KV { String key; String value; };
    std::vector<KV> kvPairs;
    kvPairs.reserve(16);

    if (inDoc.is<JsonArray>()) {
        for (JsonObject item : inDoc.as<JsonArray>()) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;
            if (kvPairs.size() >= 32) break;
            kvPairs.push_back({item["id"].as<String>(), item["value"].as<String>()});
        }
    } else if (inDoc.is<JsonObject>()) {
        for (JsonPair p : inDoc.as<JsonObject>()) {
            if (kvPairs.size() >= 32) break;
            kvPairs.push_back({String(p.key().c_str()), p.value().as<String>()});
        }
    } else {
        return jsonInput;
    }

    if (kvPairs.empty()) return jsonInput;

    String result = templateStr;
    for (const auto& kv : kvPairs) {
        String placeholder = "${" + kv.key + "}";
        result.replace(placeholder, kv.value);
    }

    LOGGER.infof("[RuleScript] Template applied: %d vars, %d->%d bytes",
                 (int)kvPairs.size(), (int)jsonInput.length(), (int)result.length());
    return result;
}

// ========== 数据转换管道 ==========

String RuleScriptManager::applyReceiveTransform(uint8_t protocolType, const String& rawData) {
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ruleScript")) return rawData;
    AccessGuard access(*this);
    if (!access.admitted()) return rawData;
    String scriptCopy;
    {
        MutexGuard lock(access.mutex(), 0);
        if (!lock.isLocked()) return rawData;

        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 0) continue;  // 0=DATA_RECEIVE
            if (rule.protocolType != protocolType) continue;
            if (rule.scriptContent.isEmpty()) continue;

            rule.lastTriggerTime = millis();
            rule.triggerCount++;
            scriptCopy = rule.scriptContent;
            break;
        }
    }

    if (scriptCopy.isEmpty()) return rawData;
    return applyTemplate(scriptCopy, rawData);
}

String RuleScriptManager::applyReportTransform(uint8_t protocolType, const String& rawData) {
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ruleScript")) return rawData;
    AccessGuard access(*this);
    if (!access.admitted()) return rawData;
    String scriptCopy;
    {
        MutexGuard lock(access.mutex(), 0);
        if (!lock.isLocked()) return rawData;

        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 1) continue;  // 1=DATA_REPORT
            if (rule.protocolType != protocolType) continue;
            if (rule.scriptContent.isEmpty()) continue;

            rule.lastTriggerTime = millis();
            rule.triggerCount++;
            scriptCopy = rule.scriptContent;
            break;
        }
    }

    if (scriptCopy.isEmpty()) return rawData;
    return applyTemplate(scriptCopy, rawData);
}

bool RuleScriptManager::tryApplyReportTransform(uint8_t protocolType, const String& rawData,
                                               String& output, size_t maxBytes) {
    output = "";
    auto& features = FastBee::RuntimeFeatureManager::instance();
    if (!features.isUserEnabled("ruleScript"))
        return FastBee::copyCheckedText(output, rawData.c_str(), rawData.length(), maxBytes);
    if (!features.shouldEnable("ruleScript")) return false;
    AccessGuard access(*this);
    if (!access.admitted()) return false;
    String script;
    {
        MutexGuard lock(access.mutex(), 0);
        if (!lock.isLocked()) return false;
        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 1 || rule.protocolType != protocolType ||
                rule.scriptContent.isEmpty()) continue;
            if (!FastBee::copyCheckedText(script, rule.scriptContent.c_str(), rule.scriptContent.length(), maxBytes))
                return false;
            rule.lastTriggerTime = millis();
            if (rule.triggerCount != UINT32_MAX) ++rule.triggerCount;
            break;
        }
    }
    return FastBee::applyCheckedReportTemplate(script, rawData, output, maxBytes);
}

// ========== MQTT 主题匹配 ==========

bool RuleScriptManager::matchTopic(const String& pattern, const String& topic) {
    // 空模式匹配所有主题
    if (pattern.isEmpty()) return true;
    // 完全相等快速路径
    if (pattern == topic) return true;
    // 没有通配符，不相等则不匹配
    if (pattern.indexOf('+') < 0 && pattern.indexOf('#') < 0) return false;

    // 按 / 分割逐级比较
    int pLen = pattern.length();
    int tLen = topic.length();
    int pi = 0, ti = 0;

    while (pi < pLen && ti < tLen) {
        // 提取 pattern 当前级
        int pSlash = pattern.indexOf('/', pi);
        if (pSlash < 0) pSlash = pLen;
        String pPart = pattern.substring(pi, pSlash);

        if (pPart == "#") {
            // # 匹配剩余所有级
            return true;
        }

        // 提取 topic 当前级
        int tSlash = topic.indexOf('/', ti);
        if (tSlash < 0) tSlash = tLen;
        String tPart = topic.substring(ti, tSlash);

        if (pPart != "+" && pPart != tPart) {
            return false;
        }

        pi = pSlash + 1;
        ti = tSlash + 1;
    }

    // pattern 末尾是 # 时匹配
    if (pi < pLen && pattern.substring(pi) == "#") return true;

    // 两边都结束才匹配
    return (pi >= pLen && ti >= tLen);
}

// ========== 主题感知接收转换 ==========

String RuleScriptManager::applyReceiveTransform(uint8_t protocolType, const String& rawData,
                                                 const String& topic, String* outTargetTopic) {
    if (outTargetTopic) *outTargetTopic = "";
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ruleScript")) return rawData;
    AccessGuard access(*this);
    if (!access.admitted()) return rawData;
    String scriptCopy;
    String targetTopicCopy;
    {
        MutexGuard lock(access.mutex(), 0);
        if (!lock.isLocked()) return rawData;

        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 0) continue;  // 0=DATA_RECEIVE
            if (rule.protocolType != protocolType) continue;
            if (rule.scriptContent.isEmpty()) continue;

            // 主题过滤：sourceTopic 为空时匹配所有
            if (!rule.sourceTopic.isEmpty() && !matchTopic(rule.sourceTopic, topic)) {
                continue;
            }

            rule.lastTriggerTime = millis();
            rule.triggerCount++;
            scriptCopy = rule.scriptContent;
            targetTopicCopy = rule.targetTopic;
            break;
        }
    }

    if (scriptCopy.isEmpty()) return rawData;

    String result = applyTemplate(scriptCopy, rawData);

    // 写入目标主题（调用方根据此值决定是否重定向发布）
    if (outTargetTopic && !targetTopicCopy.isEmpty()) {
        *outTargetTopic = targetTopicCopy;
    }

    return result;
}

// ========== 工具 ==========

String RuleScriptManager::generateUniqueId() {
    return "rs_" + String(millis());
}
#endif // FASTBEE_ENABLE_RULE_SCRIPT
