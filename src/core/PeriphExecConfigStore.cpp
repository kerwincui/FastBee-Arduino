/**
 * Streaming execution-rule configuration persistence and migration.
 * Runtime admission, scheduling and execution remain in PeriphExecManager.
 */
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#include "core/PeriphExecRemoteBindings.h"
#include "core/PeriphExecConfigValidation.h"
#include "core/RuleOutputIdentity.h"
#include "core/MemoryBudget.h"
#include "systems/LoggerSystem.h"
#include "utils/FileUtils.h"
#include "utils/SharedJsonSerializer.h"
#include "utils/SharedJsonDeserializer.h"
#include "network/FixedJsonField.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
constexpr uint32_t PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS = 2000;
constexpr size_t PERIPH_EXEC_CONFIG_MAX_BYTES = FastBee::PeriphExecConfig::kMaxBytes;

// Stream one JSON value at a time. Scalar look-ahead must not consume the next
// root delimiter (ArduinoJson's own reader may read one byte beyond a number).
class ConfigValueReader {
public:
    explicit ConfigValueReader(File& file) : file_(file) {}
    int read() {
        if (done_) return -1;
        const int next = file_.peek();
        if (next < 0) return -1;
        if (started_ && depth_ == 0 && !quote_ &&
            (next == ',' || next == '}' || next == ']' || isspace(next))) return -1;
        const char c = static_cast<char>(file_.read());
        if (!started_) started_ = true;
        if (quote_) {
            if (escaped_) escaped_ = false;
            else if (c == '\\') escaped_ = true;
            else if (c == quote_) { quote_ = 0; if (depth_ == 0) done_ = true; }
        } else if (c == '"' || c == '\'') quote_ = c;
        else if (c == '{' || c == '[') ++depth_;
        else if (c == '}' || c == ']') { if (--depth_ <= 0) done_ = true; }
        return static_cast<unsigned char>(c);
    }
    size_t readBytes(char* buffer, size_t count) {
        size_t copied = 0;
        while (copied < count) { int c = read(); if (c < 0) break; buffer[copied++] = static_cast<char>(c); }
        return copied;
    }
private:
    File& file_;
    int depth_ = 0;
    char quote_ = 0;
    bool started_ = false, escaped_ = false, done_ = false;
};

static int configNext(File& file) {
    int c;
    while ((c = file.peek()) >= 0 && isspace(c)) file.read();
    return c;
}

static bool configError(String& error, const char* code) { error = code; return false; }

static bool configMemoryAvailable(size_t bytes, size_t contiguous) {
    // Preserve the existing 8-bit Web severe-pressure floor. Worker stacks
    // and lazy result history still have their own later admission checks.
    constexpr size_t reserve = FastBee::MemoryBudget::GUARD8_SEVERE_DRAM_FREE;
    return FastBee::MemoryBudget::dram8Free() >= bytes + reserve &&
           FastBee::MemoryBudget::dram8Largest() >= contiguous;
}

char* allocateConfigEventId(size_t bytes) {
    // Recheck current DRAM before EACH exact-ID copy, not just once per rule.
    // The post-allocation floor check also accounts for allocator metadata.
    if (!configMemoryAvailable(bytes, bytes)) return nullptr;
    std::unique_ptr<char[]> copy(new (std::nothrow) char[bytes]);
    if (!copy || !configMemoryAvailable(0, 0)) return nullptr;
    return copy.release();
}

class ConfigJsonAllocator final : public ArduinoJson::Allocator {
    struct alignas(std::max_align_t) Header { size_t bytes; };
public:
    void* allocate(size_t size) override {
        if (!configMemoryAvailable(size + sizeof(Header), size + sizeof(Header))) return nullptr;
        auto* block = static_cast<Header*>(malloc(size + sizeof(Header)));
        if (!block) return nullptr;
        block->bytes = size;
        return block + 1;
    }
    void deallocate(void* pointer) override {
        if (pointer) free(static_cast<Header*>(pointer) - 1);
    }
    void* reallocate(void* pointer, size_t size) override {
        if (!pointer) return allocate(size);
        auto* block = static_cast<Header*>(pointer) - 1;
        // ArduinoJson shrinks its pool after parsing, including error paths.
        // A shrink must keep the live allocation even if pressure just rose.
        if (size <= block->bytes) return pointer;
        if (!configMemoryAvailable(size + sizeof(Header), size + sizeof(Header))) return nullptr;
        block = static_cast<Header*>(realloc(block, size + sizeof(Header)));
        if (!block) return nullptr;
        block->bytes = size;
        return block + 1;
    }
};

static bool readConfigValue(File& file, JsonDocument& doc, String& error) {
    ConfigValueReader input(file);
    const auto parsed = FastBee::deserializeJsonShared(doc, input);
    if (!parsed) return true;
    return configError(error, parsed == DeserializationError::NoMemory ? "CONFIG_MEMORY_PRESSURE" : "CONFIG_JSON");
}

using ConfigRuleVisitor = bool (*)(JsonObjectConst, void*, String&);

class ConfigStringAppender {
public:
    explicit ConfigStringAppender(String& value) : value_(value) {}
    size_t write(uint8_t c) {
        const size_t before = value_.length();
        value_ += static_cast<char>(c);
        return value_.length() - before;
    }
    size_t write(const uint8_t* data, size_t size) {
        const size_t before = value_.length();
        for (size_t i = 0; i < size; ++i) value_ += static_cast<char>(data[i]);
        return value_.length() - before;
    }
private:
    String& value_;
};

// One temporary rule document plus a fixed ID index; validation never builds
// a rule map. The optional visitor moves each decoded rule into a candidate map.
static bool visitConfigurationFile(const char* path, ConfigRuleVisitor visitor,
                                   void* context, String* irKeys, String& error,
                                   bool legacyIr = false,
                                   FastBee::PeriphExecConfig::RuleReferenceIndex* capturedIds = nullptr) {
    using namespace FastBee::PeriphExecConfig;
    File file = LittleFS.open(path, "r");
    if (!file) return configError(error, "CONFIG_OPEN");
    if (file.size() > kMaxBytes) return configError(error, "CONFIG_TOO_LARGE");
    if (configNext(file) != '{') return configError(error, "CONFIG_OBJECT");
    file.read();
    char ids[FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES][FastBee::ResourceProfile::MAX_RULE_ID_LEN + 1];
    size_t count = 0;
    size_t residentEstimate = 0;
    size_t irEstimate = 0;
    bool sawRules = false, sawIr = false;
    while (configNext(file) != '}') {
        if (file.peek() != '"') return configError(error, "CONFIG_JSON");
        ConfigJsonAllocator allocator;
        JsonDocument doc(&allocator);
        if (!readConfigValue(file, doc, error)) return false;
        const char* key = doc.as<const char*>();
        if (!key || doc.as<JsonString>().size() != strlen(key)) return configError(error, "CONFIG_JSON");
        const bool isRules = !legacyIr && strcmp(key, "rules") == 0;
        const bool isIr = strcmp(key, legacyIr ? "keys" : "irKeys") == 0;
        if (configNext(file) != ':') return configError(error, "CONFIG_JSON");
        file.read();
        configNext(file);
        if (isRules || isIr) {
            if ((isRules && sawRules) || (isIr && sawIr)) return configError(error, "CONFIG_DUPLICATE_FIELD");
            if (isRules) sawRules = true; else sawIr = true;
            // Missing/null irKeys in older files means no mappings.
            if (isIr && file.peek() == 'n') {
                if (!readConfigValue(file, doc, error)) return false;
                if (!doc.isNull()) return configError(error, "CONFIG_IR_ARRAY");
            } else {
                if (file.read() != '[') return configError(error, isRules ? "CONFIG_RULES_ARRAY" : "CONFIG_IR_ARRAY");
                if (isIr && irKeys) *irKeys = "[";
                bool first = true;
                while (configNext(file) != ']') {
                    if (isRules && count >= FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES)
                        return configError(error, "CONFIG_RULE_COUNT");
                    if (!readConfigValue(file, doc, error)) return false;
                    if (isRules) {
                        const JsonObjectConst rule = doc.as<JsonObjectConst>();
                        if (const char* issue = validateRule(rule)) return configError(error, issue);
                        // Deliberately conservative: include node/string allocator
                        // overhead and both event indexes, not just JSON file size.
                        const size_t triggerCount = rule["triggers"].size();
                        const size_t actionCount = rule["actions"].size();
                        const size_t estimate = measureJson(rule) + sizeof(PeriphExecRule) + 64 +
                            triggerCount * (sizeof(ExecTrigger) + 96) +
                            actionCount * (sizeof(ExecAction) + 32);
                        residentEstimate += estimate;
                        if (!configMemoryAvailable(visitor ? estimate : residentEstimate + irEstimate,
                                                   std::max(triggerCount * sizeof(ExecTrigger),
                                                       actionCount * sizeof(ExecAction))))
                            return configError(error, "CONFIG_MEMORY_PRESSURE");
                        const char* id = rule["id"].as<const char*>();
                        for (size_t i = 0; i < count; ++i)
                            if (strcmp(ids[i], id) == 0) return configError(error, "CONFIG_DUPLICATE_ID");
                        strcpy(ids[count++], id);
                        if (capturedIds) {
                            if (!capturedIds->add(rule["id"])) return configError(error, "CONFIG_RULE_ID");
                            if (const char* issue = capturedIds->captureRuleEmissions(
                                    rule, allocateConfigEventId))
                                return configError(error, issue);
                        }
                        if (visitor && !visitor(rule, context, error)) return false;
                    } else {
                        const size_t expected = measureJson(doc);
                        irEstimate += expected + 1;
                        if (!configMemoryAvailable((visitor ? 0 : residentEstimate) + irEstimate, irEstimate))
                            return configError(error, "CONFIG_MEMORY_PRESSURE");
                        if (irKeys) {
                            const size_t before = irKeys->length();
                            irKeys->reserve(before + expected + 1);
                            if (!first) *irKeys += ',';
                            // ArduinoJson's String writer clears its target;
                            // explicitly append each IR item to the array.
                            ConfigStringAppender appender(*irKeys);
                            FastBee::serializeJsonShared(doc, appender);
                            if (irKeys->length() != before + expected + (first ? 0 : 1))
                                return configError(error, "CONFIG_MEMORY_PRESSURE");
                        }
                    }
                    first = false;
                    const int delimiter = configNext(file);
                    if (delimiter == ']') break;
                    if (delimiter != ',') return configError(error, "CONFIG_JSON");
                    file.read();
                    if (configNext(file) == ']') return configError(error, "CONFIG_JSON");
                }
                file.read();
                if (isIr && irKeys) {
                    const size_t expected = irKeys->length() + 1;
                    *irKeys += ']';
                    if (irKeys->length() != expected) return configError(error, "CONFIG_MEMORY_PRESSURE");
                }
            }
        } else {
            if (!readConfigValue(file, doc, error)) return false;
        }
        const int delimiter = configNext(file);
        if (delimiter == '}') break;
        if (delimiter != ',') return configError(error, "CONFIG_JSON");
        file.read();
        if (configNext(file) == '}') return configError(error, "CONFIG_JSON");
    }
    file.read();
    if (configNext(file) != -1) return configError(error, "CONFIG_JSON");
    if (!(legacyIr ? sawIr : sawRules)) return configError(error, "CONFIG_RULES_ARRAY");
    return true;
}

static bool copyConfigText(String& target, JsonVariantConst source) {
    const char* value = source.as<const char*>();
    target = value ? value : "";
    return target.length() == (value ? strlen(value) : 0);
}

static bool configBool(JsonVariantConst value, bool fallback) {
    return value.isNull() ? fallback : value.as<bool>();
}

static bool decodeConfigRule(JsonObjectConst obj, PeriphExecRule& r, String& error) {
    if (!copyConfigText(r.id, obj["id"]) || !copyConfigText(r.name, obj["name"]) ||
        !copyConfigText(r.scriptContent, obj["scriptContent"])) return configError(error, "CONFIG_MEMORY_PRESSURE");
    if (r.scriptContent == "null") r.scriptContent = "";
    r.enabled = configBool(obj["enabled"], true);
    r.execMode = obj["execMode"] | 0;
    r.protocolType = obj["protocolType"] | 0;
    r.reportAfterExec = configBool(obj["reportAfterExec"], true);
    r.allowPanelControl = configBool(obj["allowPanelControl"], false);
    r.recordHistory = configBool(obj["recordHistory"], false);
    r.triggers.reserve(obj["triggers"].size());
    for (JsonObjectConst tObj : obj["triggers"].as<JsonArrayConst>()) {
        ExecTrigger t;
        if (!copyConfigText(t.triggerPeriphId, tObj["triggerPeriphId"]) ||
            !copyConfigText(t.compareValue, tObj["compareValue"]) ||
            !copyConfigText(t.timePoint, tObj["timePoint"]) ||
            !copyConfigText(t.eventId, tObj["eventId"])) return configError(error, "CONFIG_MEMORY_PRESSURE");
        t.triggerType = tObj["triggerType"] | 0;
        t.operatorType = tObj["operatorType"] | 0;
        t.timerMode = tObj["timerMode"] | 0;
        t.intervalSec = tObj["intervalSec"] | uint32_t(60);
        t.pollResponseTimeout = tObj["pollResponseTimeout"] | 1000;
        t.pollMaxRetries = tObj["pollMaxRetries"] | 2;
        t.pollInterPollDelay = tObj["pollInterPollDelay"] | 100;
        r.triggers.push_back(std::move(t));
    }
    r.actions.reserve(obj["actions"].size());
    for (JsonObjectConst aObj : obj["actions"].as<JsonArrayConst>()) {
        ExecAction a;
        if (!copyConfigText(a.targetPeriphId, aObj["targetPeriphId"]) ||
            !copyConfigText(a.actionValue, aObj["actionValue"])) return configError(error, "CONFIG_MEMORY_PRESSURE");
        a.actionType = aObj["actionType"] | 0;
        a.useReceivedValue = configBool(aObj["useReceivedValue"], false);
        a.syncDelayMs = aObj["syncDelayMs"] | 0;
        a.execMode = aObj["execMode"] | 0;
        r.actions.push_back(std::move(a));
    }
    return true;
}

static bool validateRuleProfileLimits(const PeriphExecRule& rule, const char* operation, String* errorMsg = nullptr) {
    const char* issue = FastBee::PeriphExecConfig::validateRule(rule);
    if (!issue) return true;
    LOGGER.warningf("[PeriphExec] %s: %s", operation, issue);
    if (errorMsg) *errorMsg = issue;
    return false;
}

} // namespace

// ========== 持久化 ==========

bool PeriphExecManager::saveConfiguration() {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) {
        LOGGER.warning("[PeriphExec] Save skipped: rule configuration is locked");
        return false;
    }
    return saveConfigurationLocked();
}

bool PeriphExecManager::saveConfigurationLocked() {
    if (configurationWriteError()) return false;
    return writeConfigurationLocked(rules, _irKeysJson);
}

bool PeriphExecManager::writeConfigurationLocked(const RuleMap& sourceRules, const String& irKeys) {
    if (sourceRules.size() > FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) return false;
    if (const char* issue = FastBee::validateRuleCompletionGraph(sourceRules)) {
        LOGGER.errorf("[PeriphExec] Save rejected: %s", issue); return false;
    }
    for (const auto& pair : sourceRules) {
        if (!validateRuleProfileLimits(pair.second, "saveConfiguration")) return false;
    }
    // 流式保存：逐规则构建小 JsonDocument（单条约 1~2KB）并直接序列化进临时文件，
    // 避免把整份配置表（20+KB）构建进单个 JsonDocument，在堆碎片化
    // （freeHeap~30KB / maxAlloc~27KB）下直接 overflow 失败。
    String tmpPath = String(PERIPH_EXEC_CONFIG_FILE) + ".tmp";
    File out = FileUtils::openAtomicWrite(PERIPH_EXEC_CONFIG_FILE);
    if (!out) {
        LOGGER.errorf("[PeriphExec] Save aborted: cannot open temp file (freeHeap=%u, maxAlloc=%u)",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        return false;
    }

    size_t written = out.print("{\"rules\":[");
    bool complete = written == 10;
    bool first = true;

    for (const auto& pair : sourceRules) {
        const PeriphExecRule& r = pair.second;

        JsonDocument doc; // 只容纳单条规则，峰值内存与规则总数无关
        JsonObject obj = doc.to<JsonObject>();
        FastBee::setFixedJsonField(obj, "id", r.id);
        FastBee::setFixedJsonField(obj, "name", r.name);
        FastBee::setFixedJsonField(obj, "enabled", r.enabled);
        FastBee::setFixedJsonField(obj, "execMode", r.execMode);

        // 触发器数组
        JsonArray trigArr = obj["triggers"].to<JsonArray>();
        for (const auto& t : r.triggers) {
            JsonObject tObj = trigArr.add<JsonObject>();
            FastBee::setFixedJsonField(tObj, "triggerType", t.triggerType);
            FastBee::setFixedJsonField(tObj, "triggerPeriphId", t.triggerPeriphId);
            FastBee::setFixedJsonField(tObj, "operatorType", t.operatorType);
            FastBee::setFixedJsonField(tObj, "compareValue", t.compareValue);
            FastBee::setFixedJsonField(tObj, "timerMode", t.timerMode);
            FastBee::setFixedJsonField(tObj, "intervalSec", t.intervalSec);
            FastBee::setFixedJsonField(tObj, "timePoint", t.timePoint);
            FastBee::setFixedJsonField(tObj, "eventId", t.eventId);
            // 轮询触发通信参数
            FastBee::setFixedJsonField(tObj, "pollResponseTimeout", t.pollResponseTimeout);
            FastBee::setFixedJsonField(tObj, "pollMaxRetries", t.pollMaxRetries);
            FastBee::setFixedJsonField(tObj, "pollInterPollDelay", t.pollInterPollDelay);
        }

        // 动作数组
        JsonArray actArr = obj["actions"].to<JsonArray>();
        for (const auto& a : r.actions) {
            JsonObject aObj = actArr.add<JsonObject>();
            FastBee::setFixedJsonField(aObj, "targetPeriphId", a.targetPeriphId);
            FastBee::setFixedJsonField(aObj, "actionType", a.actionType);
            FastBee::setFixedJsonField(aObj, "actionValue", a.actionValue);
            FastBee::setFixedJsonField(aObj, "useReceivedValue", a.useReceivedValue);
            FastBee::setFixedJsonField(aObj, "syncDelayMs", a.syncDelayMs);
            FastBee::setFixedJsonField(aObj, "execMode", a.execMode);
        }

        // 数据转换管道
        FastBee::setFixedJsonField(obj, "protocolType", r.protocolType);
        FastBee::setFixedJsonField(obj, "scriptContent", r.scriptContent);

        // 数据上报控制
        FastBee::setFixedJsonField(obj, "reportAfterExec", r.reportAfterExec);
        FastBee::setFixedJsonField(obj, "allowPanelControl", r.allowPanelControl);
        FastBee::setFixedJsonField(obj, "recordHistory", r.recordHistory);

        // 构建过程中若堆不足，ArduinoJson 会置位 overflowed，此时该规则不完整，
        // 拒绝保存以防写入残缺配置（保留原文件不动）
        if (doc.overflowed()) {
            out.close();
            LittleFS.remove(tmpPath);
            LOGGER.errorf("[PeriphExec] Save aborted: rule '%s' serialization overflow (freeHeap=%u, maxAlloc=%u)",
                          r.id.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            return false;
        }

        size_t expected = measureJson(doc);
        if (!first) {
            const size_t separator = out.print(",");
            complete = complete && separator == 1;
            written += separator;
        }
        // 流式序列化直写 Flash；written != expected 即检出截断（堆/Flash 不足）
        size_t w = serializeJson(doc, out);
        if (w != expected) {
            out.close();
            LittleFS.remove(tmpPath);
            LOGGER.errorf("[PeriphExec] Save aborted: truncated at rule '%s' (written=%u, expected=%u, fsFree=%u)",
                          r.id.c_str(), (unsigned)w, (unsigned)expected,
                          (unsigned)(LittleFS.totalBytes() > LittleFS.usedBytes()
                                     ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0));
            return false;
        }
        written += w;
        first = false;
    }

    size_t part = out.print("]");
    complete = complete && part == 1;
    written += part;

    // IR 按键映射（红外对码结果）随本配置文件统一持久化，不再单独建 ir_keys.json；
    // irKeys 本身即 serializeJson 产出的合法 JSON 数组串，直接透传写入
    if (!irKeys.isEmpty()) {
        part = out.print(",\"irKeys\":");
        complete = complete && part == 10;
        written += part;
        part = out.print(irKeys);
        complete = complete && part == irKeys.length();
        written += part;
    }

    part = out.print("}");
    complete = complete && part == 1;
    written += part;
    out.flush();
    out.close();

    // 超限检查：加载端会拒绝超过上限的配置，写入无意义
    if (!complete || written > PERIPH_EXEC_CONFIG_MAX_BYTES) {
        LittleFS.remove(tmpPath);
        LOGGER.errorf("[PeriphExec] Save aborted: config too large (%u bytes, max=%u)",
                      (unsigned)written, (unsigned)PERIPH_EXEC_CONFIG_MAX_BYTES);
        return false;
    }

    // 以文件长度复核总写入字节，兜底检出 Flash 写满导致的静默截断
    File check = LittleFS.open(tmpPath, "r");
    size_t actual = check ? check.size() : 0;
    if (check) check.close();
    if (actual != written) {
        LittleFS.remove(tmpPath);
        LOGGER.errorf("[PeriphExec] Save aborted: size mismatch (file=%u, written=%u)",
                      (unsigned)actual, (unsigned)written);
        return false;
    }

    // 原子替换：失败时 tmp 已被 commit 内部清理，原配置保留不动
    if (!FileUtils::commitAtomicWrite(tmpPath, PERIPH_EXEC_CONFIG_FILE)) {
        LOGGER.errorf("[PeriphExec] Failed to save config: commit failed (json=%u bytes)", (unsigned)written);
        return false;
    }

    LOGGER.infof("[PeriphExec] Saved %d rules (%u bytes, v3-streaming, freeHeap=%u)",
                 (int)sourceRules.size(), (unsigned)written, (unsigned)ESP.getFreeHeap());
    return true;
}

bool PeriphExecManager::validateConfigurationFile(const char* path, String& errorMsg) {
    errorMsg = "";
    try {
        FastBee::PeriphExecConfig::RuleReferenceIndex ids;
        if (!visitConfigurationFile(path, nullptr, nullptr, nullptr, errorMsg, false, &ids)) return false;
        if (!visitConfigurationFile(path, [](JsonObjectConst rule, void* opaque, String& error) {
            auto& index = *static_cast<FastBee::PeriphExecConfig::RuleReferenceIndex*>(opaque);
            const char* issue = index.recordCompletionRule(rule);
            return !issue || configError(error, issue);
        }, &ids, nullptr, errorMsg)) return false;
        const char* issue = ids.completion.error();
        return !issue || configError(errorMsg, issue);
    } catch (const std::bad_alloc&) {
        return configError(errorMsg, "CONFIG_MEMORY_PRESSURE");
    }
}

bool PeriphExecManager::reviewConfigurationFile(const char* path, ConfigurationReviewVisitor visitor,
                                               void* context, String& errorMsg) {
    try {
        FastBee::PeriphExecConfig::RuleReferenceIndex ids;
        if (!visitConfigurationFile(path, nullptr, nullptr, nullptr, errorMsg, false, &ids)) return false;
        struct Review { FastBee::PeriphExecConfig::RuleReferenceIndex& ids;
            ConfigurationReviewVisitor visitor; void* context; } review{ids, visitor, context};
        if (!visitConfigurationFile(path, [](JsonObjectConst raw, void* opaque, String& error) {
            auto& state = *static_cast<Review*>(opaque);
            if (const char* issue = FastBee::PeriphExecConfig::validateRuleReferences(raw, state.ids))
                return configError(error, issue);
            if (const char* issue = state.ids.recordCompletionRule(raw)) return configError(error, issue);
            if (!state.visitor) return true;
            PeriphExecRule rule;
            return decodeConfigRule(raw, rule, error) && state.visitor(rule, state.context, error);
        }, &review, nullptr, errorMsg)) return false;
        const char* issue = ids.completion.error();
        return !issue || configError(errorMsg, issue);
    } catch (const std::bad_alloc&) { return configError(errorMsg, "CONFIG_MEMORY_PRESSURE"); }
}

bool PeriphExecManager::readConfigurationIrKeys(const char* path, bool legacy, String& keys, String& errorMsg) {
    keys = "";
    try { return visitConfigurationFile(path, nullptr, nullptr, &keys, errorMsg, legacy); }
    catch (const std::bad_alloc&) { return configError(errorMsg, "CONFIG_MEMORY_PRESSURE"); }
}

bool PeriphExecManager::importConfigurationFile(const char* stagedPath, String& errorMsg) {
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS));
    if (!lock.isLocked()) return configError(errorMsg, "RULE_SERVICE_BUSY");
    if (_configurationRestartPending) return configError(errorMsg, "CONFIG_PENDING_RESTART");
    if (_weatherReferenceLease) return configError(errorMsg, "WEATHER_MIGRATION_BUSY");
    if (!stagedPath || strcmp(stagedPath, PERIPH_EXEC_CONFIG_FILE) == 0)
        return configError(errorMsg, "CONFIG_STAGING_PATH");
    try {
        errorMsg = "";
        FastBee::PeriphExecConfig::RuleReferenceIndex importedIds;
        // First pass keeps aggregate memory/schema admission and captures the
        // complete replacement's IDs. Live rules are NOT valid import targets.
        if (!visitConfigurationFile(stagedPath, nullptr, nullptr, nullptr, errorMsg, false, &importedIds)) return false;
        const auto checkReferences = [](JsonObjectConst rule, void* opaque, String& error) -> bool {
            auto& ids = *static_cast<FastBee::PeriphExecConfig::RuleReferenceIndex*>(opaque);
            const char* issue = FastBee::PeriphExecConfig::validateRuleReferences(rule, ids);
            if (issue) return configError(error, issue);
            issue = ids.recordCompletionRule(rule);
            return !issue || configError(error, issue);
        };
        // A second streaming pass allows forward/self/mutual references without
        // constructing a second rule map or altering legacy load/save behavior.
        if (!visitConfigurationFile(stagedPath, checkReferences, &importedIds, nullptr, errorMsg)) return false;
        if (const char* issue = importedIds.completion.error()) return configError(errorMsg, issue);
        if (!FileUtils::commitAtomicWrite(stagedPath, PERIPH_EXEC_CONFIG_FILE))
            return configError(errorMsg, "CONFIG_COMMIT_FAILED");
        _configurationRestartPending = true;
        return true;
    } catch (const std::bad_alloc&) {
        return configError(errorMsg, "CONFIG_MEMORY_PRESSURE");
    }
}

bool PeriphExecManager::loadConfiguration() {
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS));
    if (!lock.isLocked() || _configurationRestartPending || _weatherReferenceLease) return false;
    _configurationLoadFailed = true;
    String error;
    try {
        if (!LittleFS.exists(PERIPH_EXEC_CONFIG_FILE)) {
            const bool saved = writeConfigurationLocked(rules, _irKeysJson);
            _configurationLoadFailed = !saved;
            if (saved) refreshRuleRuntimeIndexesLocked(false);
            return saved;
        }
        RuleMap candidate;
        String candidateIr;
        bool normalizedLoadedConfig = false;
        bool hasRetiredRemoteAction = false;
        struct Context {
            PeriphExecManager* owner;
            RuleMap* candidate;
            bool* normalized;
            bool* retired;
        } context{this, &candidate, &normalizedLoadedConfig, &hasRetiredRemoteAction};
        const auto readRule = [](JsonObjectConst obj, void* opaque, String& issue) -> bool {
            auto& ctx = *static_cast<Context*>(opaque);
            PeriphExecRule r;
            if (!decodeConfigRule(obj, r, issue)) return false;
            for (const auto& trigger : r.triggers)
                if (hasRetiredRemoteSource(trigger)) *ctx.retired = true;
            for (const auto& action : r.actions)
                if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) *ctx.retired = true;
            *ctx.normalized = ctx.owner->sanitizeRuleForSafety(r) || *ctx.normalized;
            // Move vectors and strings; no second full rule/map copy.
            auto inserted = ctx.candidate->emplace(r.id, std::move(r));
            if (!inserted.second || inserted.first->first != (obj["id"] | ""))
                return configError(issue, "CONFIG_MEMORY_PRESSURE");
            return true;
        };
        if (!visitConfigurationFile(PERIPH_EXEC_CONFIG_FILE, readRule, &context, &candidateIr, error)) {
            LOGGER.errorf("[PeriphExec] Configuration retained, load failed: %s", error.c_str());
            return false;
        }

        // Build indexes before publishing the map, so even allocation failure
        // here cannot expose a partially replaced configuration.
        if (const char* issue = FastBee::validateRuleCompletionGraph(candidate)) {
            LOGGER.errorf("[PeriphExec] Configuration retained, load failed: %s", issue);
            return false;
        }
        decltype(_buttonEventCache) buttonCache;
        decltype(_dataSourceCache) sourceCache;
        const auto memoryFailure = []() {
            LOGGER.error("[PeriphExec] CONFIG_MEMORY_PRESSURE: configuration retained");
            return false;
        };
        for (const auto& pair : candidate) {
            if (!pair.second.enabled) continue;
            for (const auto& trigger : pair.second.triggers) {
                if (!configMemoryAvailable(trigger.eventId.length() + trigger.triggerPeriphId.length() + 128, 128))
                    return memoryFailure();
                if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) {
                    if (trigger.eventId.startsWith("button_")) {
                        String key = trigger.triggerPeriphId.isEmpty() ? trigger.eventId :
                            trigger.triggerPeriphId + ":" + trigger.eventId;
                        const size_t expected = trigger.eventId.length() +
                            (trigger.triggerPeriphId.isEmpty() ? 0 : trigger.triggerPeriphId.length() + 1);
                        if (key.length() != expected) return memoryFailure();
                        buttonCache.insert(std::move(key));
                    }
                    if (trigger.eventId.startsWith("ds:")) {
                        String key = trigger.eventId.substring(3);
                        if (key.length() != trigger.eventId.length() - 3) return memoryFailure();
                        sourceCache.insert(std::move(key));
                    }
                }
                if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER) &&
                    !trigger.triggerPeriphId.isEmpty()) {
                    String key = trigger.triggerPeriphId;
                    if (key.length() != trigger.triggerPeriphId.length()) return memoryFailure();
                    sourceCache.insert(std::move(key));
                }
            }
        }

        // Reserve identities before migration can change durable files. Failed
        // loads may consume unused actor numbers, but never retire live work or
        // reuse an actor. Publishing below requires no index/identity allocation.
        const uint32_t outputEpoch = FastBee::RuleOutputIdentity::currentEpoch();
        if (!outputEpoch || outputEpoch == UINT32_MAX || _ruleRuntimeRevision >= UINT64_MAX - 1) {
            LOGGER.error("[PeriphExec] Configuration retained: runtime identity exhausted");
            return false;
        }
        for (auto& entry : candidate) {
            entry.second.outputActor = FastBee::RuleOutputIdentity::allocateActor();
            if (!entry.second.outputActor) {
                LOGGER.error("[PeriphExec] Configuration retained: output actor exhausted");
                return false;
            }
        }

        bool migratedLegacyIrKeys = false;
        const bool hasLegacyIr = LittleFS.exists("/config/ir_keys.json");
        if (hasLegacyIr && (candidateIr.isEmpty() || candidateIr == "[]")) {
            candidateIr = "";
            if (!visitConfigurationFile("/config/ir_keys.json", nullptr, nullptr, &candidateIr, error, true)) {
                LOGGER.errorf("[PeriphExec] Legacy IR file retained: %s", error.c_str());
                return false;
            }
            migratedLegacyIrKeys = true;
        }

        const char* remoteMigrationBackup = "/config/periph_exec.pre-screen-remote.json";
        bool canonicalRewriteSafe = true;
        if (hasRetiredRemoteAction && !LittleFS.exists(remoteMigrationBackup) &&
            !FileUtils::copyFile(PERIPH_EXEC_CONFIG_FILE, remoteMigrationBackup)) {
            canonicalRewriteSafe = false;
            LOGGER.warning("[PeriphExec] Remote migration deferred; original files retained");
        }
        if ((normalizedLoadedConfig || migratedLegacyIrKeys) && canonicalRewriteSafe) {
            if (!writeConfigurationLocked(candidate, candidateIr)) return false;
        }
        // Existing main-file IR mappings are already durable. For migrated
        // mappings, removal is allowed only after the main-file commit above.
        if (hasLegacyIr && canonicalRewriteSafe &&
            (!migratedLegacyIrKeys || !candidateIr.isEmpty())) LittleFS.remove("/config/ir_keys.json");

        FastBee::RuleOutputIdentity::invalidate();
        rules.swap(candidate);
        if (_ruleRuntimeRevision != UINT64_MAX) ++_ruleRuntimeRevision;
        for (auto& entry : rules) entry.second.runtimeRevision = _ruleRuntimeRevision;
        _irKeysJson = std::move(candidateIr);
        _buttonEventCache.swap(buttonCache);
        _dataSourceCache.swap(sourceCache);
        _configurationLoadFailed = false;
        LOGGER.infof("[PeriphExec] Loaded %u complete rules", static_cast<unsigned>(rules.size()));
        return true;
    } catch (const std::bad_alloc&) {
        LOGGER.error("[PeriphExec] CONFIG_MEMORY_PRESSURE: configuration retained");
        return false;
    }
}

// ========== IR 按键映射（随 periph_exec.json 统一持久化） ==========

String PeriphExecManager::getIrKeysJson() const {
    String result;
    copyIrKeysJson(result);
    return result;
}

bool PeriphExecManager::copyIrKeysJson(String& output) const {
    output = "";
    if (!_rulesMutex) return false;
    // begin/check may be reached under the peripheral lock: never wait for
    // a rules operation which may itself need that peripheral lock.
    MutexGuard lock(_rulesMutex, 0);
    if (!lock.isLocked() || configurationWriteError()) return false;
    output = _irKeysJson;
    if (output != _irKeysJson || output.length() != _irKeysJson.length()) { output = ""; return false; }
    return true;
}

bool PeriphExecManager::setIrKeysJson(const String& jsonArray, const String* expectedJson) {
    if (!_rulesMutex) return false;
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked() || configurationWriteError()) return false;
    if (expectedJson && (*expectedJson != _irKeysJson || expectedJson->length() != _irKeysJson.length())) return false;
    try {
        String normalized;
        if (!jsonArray.isEmpty()) {
            if (jsonArray.length() > PERIPH_EXEC_CONFIG_MAX_BYTES) return false;
            ConfigJsonAllocator allocator;
            JsonDocument checkDoc(&allocator);
            if (deserializeJson(checkDoc, jsonArray.c_str(), jsonArray.length()) || !checkDoc.is<JsonArray>()) return false;
            const size_t expected = measureJson(checkDoc);
            serializeJson(checkDoc, normalized);
            if (normalized.length() != expected) return false;
        }
        if (!writeConfigurationLocked(rules, normalized)) return false;
        _irKeysJson = std::move(normalized);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
