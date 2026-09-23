/**
 * @file PeriphExecManager.cpp
 * @brief 外设执行管理器实现
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "core/PeriphExecManager.h"
#include "core/PeriphExecConfigValidation.h"
#include "core/MemoryBudget.h"
#include "core/PeriphExecExecutor.h"
#include "core/PeriphExecScheduler.h"
#include "core/PeriphExecWorkerPool.h"
#include "core/PointHistory.h"
#include "core/RuleScript.h"
#include "core/FastBeeFramework.h"
#include "core/ChipConfig.h"
#include "systems/LoggerSystem.h"
#include "systems/HealthMonitor.h"
#include "core/PeripheralManager.h"
#include "core/SensorConfigEpoch.h"
#include "utils/StringUtils.h"
#include "utils/FileUtils.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
constexpr uint32_t PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC = 1;
constexpr uint32_t PERIPH_EXEC_MIN_POLL_INTERVAL_SEC = 5;
constexpr uint32_t PERIPH_EXEC_MAX_INTERVAL_SEC = 86400UL;
constexpr uint16_t PERIPH_EXEC_MIN_POLL_TIMEOUT_MS = 100;
constexpr uint16_t PERIPH_EXEC_MAX_POLL_TIMEOUT_MS = 5000;
constexpr uint16_t PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS = 3000;
constexpr uint8_t PERIPH_EXEC_MAX_POLL_RETRIES = 3;
constexpr uint8_t PERIPH_EXEC_HEAVY_POLL_RETRIES = 2;
constexpr uint16_t PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS = 20;
constexpr uint16_t PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS = 1000;
constexpr uint16_t PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS = 100;
constexpr unsigned long PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS = 1000;
constexpr unsigned long PERIPH_EXEC_HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS = 2000;
constexpr unsigned long PERIPH_EXEC_MODBUS_POLL_INGRESS_MIN_INTERVAL_MS = 1000;
constexpr unsigned long PERIPH_EXEC_POLL_THROTTLE_LOG_INTERVAL_MS = 5000;
constexpr uint32_t PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS = 2000;
constexpr size_t PERIPH_EXEC_CONFIG_MAX_BYTES = FastBee::PeriphExecConfig::kMaxBytes;
constexpr unsigned long SENSOR_CACHE_LOCK_WARN_INTERVAL_MS = 5000;

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
    const auto parsed = deserializeJson(doc, input);
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
                                   bool legacyIr = false) {
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
                            serializeJson(doc, appender);
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

#if FASTBEE_ENABLE_REMOTE_RESOURCES
static bool canonicalRemoteRuleNumber(const String& text) {
    if (text.isEmpty() || text.length() > 24 || text.charAt(0) == '+') return false;
    size_t index = text.charAt(0) == '-' ? 1U : 0U;
    if (index >= text.length()) return false;
    const size_t integerStart = index;
    while (index < text.length() &&
           isdigit(static_cast<unsigned char>(text.charAt(index)))) {
        ++index;
    }
    if (index == integerStart ||
        (index - integerStart > 1U && text.charAt(integerStart) == '0')) {
        return false;
    }
    if (index < text.length() && text.charAt(index) == '.') {
        const size_t fractionStart = ++index;
        while (index < text.length() &&
               isdigit(static_cast<unsigned char>(text.charAt(index)))) {
            ++index;
        }
        if (index == fractionStart || index - fractionStart > 6U) return false;
    }
    if (index != text.length()) return false;
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    return end != text.c_str() && !*end && std::isfinite(value);
}
#endif

static bool hasWorkerPoolMemoryHeadroom(bool logDeferred) {
#if defined(ESP_PLATFORM)
    const uint32_t workerReserve =
        static_cast<uint32_t>(PeriphExecWorkerPool::WORKER_STACK *
                              PeriphExecWorkerPool::WORKER_COUNT + 4096U);
    const uint32_t webReserveAfterWorker =
        (CHIP_IS_CLASSIC_ESP32 && FASTBEE_USE_PSRAM == 0)
            ? FastBee::MemoryBudget::WEB_GUARD_RECOVERY_DRAM_FREE
            : 0U;
    const uint32_t requiredFree8 = workerReserve + webReserveAfterWorker;
    const uint32_t free8 = FastBee::MemoryBudget::dram8Free();
    const uint32_t largest8 = FastBee::MemoryBudget::dram8Largest();
    const bool admitted =
        free8 >= requiredFree8 && largest8 >= PeriphExecWorkerPool::WORKER_STACK;

    if (!admitted && logDeferred) {
        LOGGER.warningf(
            "[PeriphExec] Worker pool memory admission deferred: free8=%lu largest8=%lu needFree8=%lu needLargest=%lu",
            (unsigned long)free8,
            (unsigned long)largest8,
            (unsigned long)requiredFree8,
            (unsigned long)PeriphExecWorkerPool::WORKER_STACK);
    }
    return admitted;
#else
    (void)logDeferred;
    return true;
#endif
}

static bool validateRuleProfileLimits(const PeriphExecRule& rule, const char* operation, String* errorMsg = nullptr) {
    const char* issue = FastBee::PeriphExecConfig::validateRule(rule);
    if (!issue) return true;
    LOGGER.warningf("[PeriphExec] %s: %s", operation, issue);
    if (errorMsg) *errorMsg = issue;
    return false;
}

static bool actionRequiresPeripheralTarget(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
        case ExecActionType::ACTION_CALL_PERIPHERAL:
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
        case ExecActionType::ACTION_STEPPER_CONTROL:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
        case ExecActionType::ACTION_SENSOR_READ:
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
        case ExecActionType::ACTION_OLED_DISPLAY:
        case ExecActionType::ACTION_RF_SEND:
        case ExecActionType::ACTION_UART_SEND:
        case ExecActionType::ACTION_GPIO_TOGGLE:
            return true;
        default:
            return false;
    }
}

static bool isTerminalSystemAction(uint8_t actionType) {
    return actionType == static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART) ||
           actionType == static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE) ||
           actionType == static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE);
}

static bool isPanelSafeAction(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
        case ExecActionType::ACTION_STEPPER_CONTROL:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
        case ExecActionType::ACTION_SENSOR_READ:
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
        case ExecActionType::ACTION_OLED_DISPLAY:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_REMOTE_CHANNEL:
        case ExecActionType::ACTION_REMOTE_READ:
            return true;
        default:
            return false;
    }
}

static bool isTerminalExecStatus(AsyncExecStatus status) {
    return status == AsyncExecStatus::COMPLETED ||
           status == AsyncExecStatus::FAILED ||
           status == AsyncExecStatus::CANCELLED ||
           status == AsyncExecStatus::REJECTED ||
           status == AsyncExecStatus::TIMED_OUT;
}

static bool isRuleSensorTarget(PeripheralType type) {
    return isGpioDigitalInputType(type) ||
           type == PeripheralType::GPIO_ANALOG_INPUT ||
           type == PeripheralType::ADC ||
           type == PeripheralType::SENSOR_GENERIC ||
           type == PeripheralType::ONE_WIRE ||
           type == PeripheralType::ENCODER ||
           type == PeripheralType::RADAR_SENSOR ||
           type == PeripheralType::RF_MODULE;
}

static bool actionMatchesPeripheralTarget(uint8_t actionType,
                                          const PeripheralConfig& target) {
    if (target.isExpanderChannel()) {
        const auto action = static_cast<ExecActionType>(actionType);
        if (action == ExecActionType::ACTION_SET_PWM || action == ExecActionType::ACTION_SET_DAC ||
            action == ExecActionType::ACTION_BREATHE) return false;
    }
    const PeripheralType type = target.type;
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_GPIO_TOGGLE:
            return type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
                   type == PeripheralType::GPIO_PWM_OUTPUT ||
                   type == PeripheralType::GPIO_ANALOG_OUTPUT;
        case ExecActionType::ACTION_BLINK:
            return type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
                   type == PeripheralType::GPIO_PWM_OUTPUT;
        case ExecActionType::ACTION_BREATHE:
            return type == PeripheralType::GPIO_PWM_OUTPUT ||
                   type == PeripheralType::GPIO_ANALOG_OUTPUT;
        case ExecActionType::ACTION_SET_PWM:
            return type == PeripheralType::GPIO_PWM_OUTPUT ||
                   type == PeripheralType::GPIO_ANALOG_OUTPUT ||
                   type == PeripheralType::PWM_SERVO ||
                   (type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
                    target.params.gpio.passive);
        case ExecActionType::ACTION_SET_DAC:
            return type == PeripheralType::DAC;
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
            return type == PeripheralType::NEO_PIXEL;
        case ExecActionType::ACTION_STEPPER_CONTROL:
            return type == PeripheralType::STEPPER_MOTOR;
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
            return target.isModbusPeripheral();
        case ExecActionType::ACTION_SENSOR_READ:
            return isRuleSensorTarget(type);
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
            return type == PeripheralType::SEVEN_SEGMENT_TM1637;
        case ExecActionType::ACTION_OLED_DISPLAY:
            return type == PeripheralType::LCD;
        case ExecActionType::ACTION_RF_SEND:
            return type == PeripheralType::RF_MODULE;
        case ExecActionType::ACTION_UART_SEND:
            return type == PeripheralType::UART;
        case ExecActionType::ACTION_CALL_PERIPHERAL:
            // This generic action has a command-dependent capability check in
            // the executor. The shared invariant here is a real, stable target.
            return true;
        default:
            return true;
    }
}

// 判断字符串是否为有效数值（整数或浮点数，含负数）
static bool isNumericString(const String& s) {
    if (s.isEmpty()) return false;
    bool hasDecimal = false;
    size_t start = 0;
    if (s.charAt(0) == '-' || s.charAt(0) == '+') {
        if (s.length() == 1) return false;
        start = 1;
    }
    for (size_t i = start; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '.') {
            if (hasDecimal) return false;
            hasDecimal = true;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

static bool tryParseBoolLike(const String& rawValue, bool& outValue) {
    return StringUtils::tryParseBoolLike(rawValue, outValue);
}

static bool executeDirectOutputCommand(PeripheralManager& pm,
                                       const PeripheralConfig& config,
                                       const String& requestedValue,
                                       String& actualValue) {
    if (!config.enabled) return false;

    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
        bool state = false;
        if (!tryParseBoolLike(requestedValue, state)) return false;
        pm.stopActionTicker(config.id);
        actualValue = state ? "1" : "0";
        return pm.writePin(config.id, state ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW);
    }

    if (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
        config.type == PeripheralType::GPIO_ANALOG_OUTPUT) {
        uint32_t duty = 0;
        bool state = false;
        if (tryParseBoolLike(requestedValue, state)) {
            duty = state ? ((1U << config.params.gpio.pwmResolution) - 1U) : 0U;
        } else {
            duty = (uint32_t)requestedValue.toInt();
        }
        actualValue = String(duty);
        return pm.writePWM(config.id, duty);
    }

    return false;
}

static bool triggersMatch(const ExecTrigger& actual, const ExecTrigger& expected) {
    return actual.triggerType == expected.triggerType &&
           actual.triggerPeriphId == expected.triggerPeriphId &&
           actual.operatorType == expected.operatorType &&
           actual.compareValue == expected.compareValue &&
           actual.timerMode == expected.timerMode &&
           actual.intervalSec == expected.intervalSec &&
           actual.timePoint == expected.timePoint &&
           actual.eventId == expected.eventId &&
           actual.pollResponseTimeout == expected.pollResponseTimeout &&
           actual.pollMaxRetries == expected.pollMaxRetries &&
           actual.pollInterPollDelay == expected.pollInterPollDelay &&
           actual.lastTriggerTime == expected.lastTriggerTime &&
           actual.triggerCount == expected.triggerCount &&
           actual._cachedHour == expected._cachedHour &&
           actual._cachedMinute == expected._cachedMinute;
}

static bool actionsMatch(const ExecAction& actual, const ExecAction& expected) {
    return actual.targetPeriphId == expected.targetPeriphId &&
           actual.actionType == expected.actionType &&
           actual.actionValue == expected.actionValue &&
           actual.useReceivedValue == expected.useReceivedValue &&
           actual.syncDelayMs == expected.syncDelayMs &&
           actual.execMode == expected.execMode;
}

static bool rulesMatch(const PeriphExecRule& actual,
                       const PeriphExecRule& expected) {
    if (actual.id != expected.id ||
        actual.name != expected.name ||
        actual.enabled != expected.enabled ||
        actual.execMode != expected.execMode ||
        actual.protocolType != expected.protocolType ||
        actual.scriptContent != expected.scriptContent ||
        actual.reportAfterExec != expected.reportAfterExec ||
        actual.allowPanelControl != expected.allowPanelControl ||
        actual.recordHistory != expected.recordHistory ||
        actual._cachedNeedsModbus != expected._cachedNeedsModbus ||
        actual._cachedHasPollCollectionAction != expected._cachedHasPollCollectionAction ||
        actual.triggers.size() != expected.triggers.size() ||
        actual.actions.size() != expected.actions.size()) {
        return false;
    }

    for (size_t i = 0; i < actual.triggers.size(); ++i) {
        if (!triggersMatch(actual.triggers[i], expected.triggers[i])) return false;
    }
    for (size_t i = 0; i < actual.actions.size(); ++i) {
        if (!actionsMatch(actual.actions[i], expected.actions[i])) return false;
    }
    return true;
}

}

// This is the single save/preflight contract for remote-channel rule actions.
// RemoteResourceManager still revalidates the value and live channel state at
// execution time before it can enqueue a publish.
bool validateRemoteChannelRuleAction(const ExecAction& action,
                                     String& errorCode,
                                     String& errorMessage) {
    errorCode = "";
    errorMessage = "";
    if (action.targetPeriphId.isEmpty()) {
        errorCode = "REMOTE_TARGET_REQUIRED";
        errorMessage = "Remote channel ID is required";
        return false;
    }

#if !FASTBEE_ENABLE_REMOTE_RESOURCES
    errorCode = "REMOTE_RESOURCES_NOT_COMPILED";
    errorMessage = "Remote resources are not compiled into this firmware";
    return false;
#else
    String channelId = action.targetPeriphId;
    if (channelId.startsWith("remote.")) channelId = channelId.substring(7);

    JsonDocument remoteConfig;
    FastBee::RemoteResourceManager::instance().getConfig(
        remoteConfig.to<JsonObject>());
    JsonObjectConst channel;
    for (JsonObjectConst candidate :
         remoteConfig["remoteChannels"].as<JsonArrayConst>()) {
        if (channelId == String(candidate["id"] | "")) {
            channel = candidate;
            break;
        }
    }
    if (channel.isNull()) {
        errorCode = "REMOTE_TARGET_NOT_FOUND";
        errorMessage = String("Remote channel '") + channelId + "' does not exist";
        return false;
    }
    if (!(channel["enabled"] | false)) {
        errorCode = "REMOTE_TARGET_DISABLED";
        errorMessage = String("Remote channel '") + channelId + "' is disabled";
        return false;
    }

    const char* channelType = channel["type"] | "";
    const bool numericChannel = strcmp(channelType, "number") == 0;
    const bool commandChannel = strcmp(channelType, "switch") == 0 ||
                                strcmp(channelType, "action") == 0;
    if (!numericChannel && !commandChannel) {
        errorCode = "REMOTE_CHANNEL_TYPE_UNSUPPORTED";
        errorMessage = String("Remote channel '") + channelId +
                       "' has an unsupported type";
        return false;
    }
    if (numericChannel && String(channel["commandTopic"] | "").isEmpty()) {
        errorCode = "REMOTE_TARGET_NOT_WRITABLE";
        errorMessage = String("Remote number channel '") + channelId +
                       "' has no command topic";
        return false;
    }

    if (action.useReceivedValue) {
        if (action.actionValue.isEmpty() || action.actionValue == "$value") return true;
        errorCode = "INVALID_REMOTE_VALUE";
        errorMessage = "Received-value remote action only supports an empty value or $value";
        return false;
    }
    if (numericChannel) {
        if (canonicalRemoteRuleNumber(action.actionValue)) return true;
        errorCode = "INVALID_REMOTE_VALUE";
        errorMessage = "Remote number value must be a canonical finite decimal";
        return false;
    }
    if (action.actionValue == "on" || action.actionValue == "off" ||
        action.actionValue == "trigger") {
        return true;
    }
    errorCode = "INVALID_REMOTE_VALUE";
    errorMessage = "Remote switch/action value must be on, off or trigger";
    return false;
#endif
}

PeriphExecManager& PeriphExecManager::getInstance() {
    static PeriphExecManager instance;
    return instance;
}

bool PeriphExecManager::suspendWorkersForMemory() {
    if (_workersSuspendedForMemory) return true;
    // Gate new dispatches before asking workers to drain. A timed-out
    // shutdown is only "pending"; the fully-suspended flag is set after every
    // worker acknowledges exit so health reporting and retry logic remain
    // truthful.
    _workerSuspensionPending = true;
    if (_workerPool) {
        // Do not destroy the pool until every worker has acknowledged exit.
        // A long-running rule may outlive the first wait window; retaining the
        // object lets the next health sample retry safely.
        if (!_workerPool->shutdown()) {
            LOGGER.warning("[PeriphExec] Worker pool shutdown deferred; worker still running");
            return false;
        }
        _workerPool.reset();
    }
    _workerSuspensionPending = false;
    _workersSuspendedForMemory = true;
    LOGGER.warning("[PeriphExec] Worker pool suspended for memory pressure");
    return true;
}

bool PeriphExecManager::resumeWorkersAfterMemory() {
    if (!_workersSuspendedForMemory && !_workerSuspensionPending) return true;

    // A prior suspend can time out while a long-running rule is finishing.
    // Do not allocate a replacement pool until the old workers are confirmed
    // exited; shutdown() is idempotent and safely retries the wait.
    if (_workerPool) {
        if (!_workerPool->shutdown()) {
            LOGGER.warning("[PeriphExec] Worker pool restore deferred (old worker still active)");
            return false;
        }
        _workerPool.reset();
    }

    // A pool that finished its deferred shutdown is now fully suspended. Keep
    // the state explicit before attempting allocation of the replacement.
    _workersSuspendedForMemory = true;

    // Recovery must preserve the same Web/WiFi reserve as startup.  The
    // HealthMonitor's aggregate internal-heap level can be NORMAL while the
    // allocatable 8-bit pool is still below the worker stack requirement on a
    // classic no-PSRAM target.  Repeated xTaskCreate attempts in that state
    // fragment the remaining heap and keep WebMemoryGuard active.
    if (!hasWorkerPoolMemoryHeadroom(true)) {
        return false;
    }

    std::unique_ptr<PeriphExecWorkerPool> candidate(new (std::nothrow) PeriphExecWorkerPool());
    if (!candidate || !candidate->begin()) {
        LOGGER.warning("[PeriphExec] Worker pool restore deferred (insufficient memory)");
        return false;
    }
    _workerPool = std::move(candidate);
    _workersSuspendedForMemory = false;
    _workerSuspensionPending = false;
    LOGGER.info("[PeriphExec] Worker pool restored after memory recovery");
    return true;
}

void PeriphExecManager::cancelWorkerJob(AsyncExecContext* ctx) {
    if (!ctx) return;

    // The manager pointer is normally this object. If a malformed context was
    // queued, still return its task slot instead of leaking an async permit.
    PeriphExecManager* owner = ctx->manager ? ctx->manager : this;
    if (owner->_runningRulesMutex) {
        MutexGuard runLock(owner->_runningRulesMutex);
        owner->_runningRuleIds.erase(ctx->ruleCopy.id);
        owner->_runningStartTime.erase(ctx->ruleCopy.id);
    }

    AsyncExecResult result;
    result.operationId = ctx->operationId;
    result.ruleId = ctx->ruleCopy.id;
    result.ruleName = ctx->ruleCopy.name;
    result.status = AsyncExecStatus::CANCELLED;
    result.errorCode = AsyncExecError::CANCELLED_BY_SYSTEM;
    result.queuedAt = ctx->queuedAt;
    result.startTime = 0;
    result.endTime = millis();
    result.deadlineAt = ctx->deadlineAt;
    result.queuePosition = ctx->queuePosition;
    result.asyncExecution = true;
    result.actionCount = static_cast<uint8_t>(
        ctx->ruleCopy.actions.size() > 255 ? 255 : ctx->ruleCopy.actions.size());
    owner->recordResult(result);
    if (FastBee::MemoryBudget::dramFree() > 10000) {
        owner->dispatchPeriphExecEvent(ctx->ruleCopy.id, "cancelled");
    }
    if (ctx->taskSlot) xSemaphoreGive(ctx->taskSlot);
    owner->_contextPool.release(ctx);
}

bool PeriphExecManager::initialize() {
    // 创建 FreeRTOS 同步原语
    _rulesMutex = xSemaphoreCreateMutex();
    _resultsMutex = xSemaphoreCreateMutex();
    _taskSlotSemaphore = xSemaphoreCreateCounting(MAX_ASYNC_TASKS, MAX_ASYNC_TASKS);
    _runningRulesMutex = xSemaphoreCreateMutex();
    _pollIngressMutex = xSemaphoreCreateMutex();
    _sensorCacheMutex = xSemaphoreCreateMutex();
#if FASTBEE_ENABLE_POINT_HISTORY
    FastBee::PointHistory::instance().initialize();
#endif
    // Result history is allocated only when the first operation is admitted.
    // Unused execution capability does not retain six full action traces.

    // 创建子模块
    _executor.reset(new PeriphExecExecutor());
    _executor->initialize(this);

    _scheduler.reset(new PeriphExecScheduler());
    _scheduler->initialize(this, _executor.get());

    bool loaded = loadConfiguration();

    // 启动常驻 Worker 任务池（D 方案）。在内存已经紧张的设备上，
    // 先保留 Web/WiFi 的连续 DRAM；轻量规则仍可同步执行，重规则会被
    // dispatchAsync 的内存门控跳过，避免启动期一次性分配栈导致 OOM。
    // On the no-PSRAM classic F4R0, a resident worker stack competes directly
    // with AsyncTCP/Web/MQTT. Rules remain configured when admission is
    // deferred; dispatchAsync uses its existing safe degradation path.
    const bool workerAdmission = hasWorkerPoolMemoryHeadroom(true);
    if (workerAdmission) {
        _workerPool.reset(new (std::nothrow) PeriphExecWorkerPool());
    }
    if (!workerAdmission || !_workerPool || !_workerPool->begin()) {
        LOGGER.warning("[PeriphExec] WorkerPool unavailable at startup; async dispatch will degrade safely");
        _workerPool.reset();  // dispatchAsync will use sync/skip fallback
        _workersSuspendedForMemory = true;
        _workerSuspensionPending = false;
    }

    // 注册 AsyncExecContext 对象池到 HealthMonitor（碎片预防监控）
    {
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor) {
            HealthMonitor::PoolStatsEntry poolEntry;
            poolEntry.name = "async_ctx";
            poolEntry.blockSize = sizeof(AsyncExecContext);
            poolEntry.capacity = AsyncExecContextPool::POOL_SIZE;
            poolEntry.getUsed = []() -> uint8_t {
                return static_cast<uint8_t>(
                    PeriphExecManager::getInstance().getContextPool().usedCount());
            };
            poolEntry.getExhaust = []() -> uint32_t {
                return PeriphExecManager::getInstance().getContextPool().exhaustCount();
            };
            monitor->registerPool(poolEntry);
            LOGGER.info("[PeriphExec] Context pool registered with HealthMonitor");
        }
    }

    // 配置加载后安全校验：检测并修正危险的任务数/轮询间隔组合
    if (loaded && _scheduler) {
        _scheduler->validateLoadedConfig();
    }

    LOGGER.infof("[PeriphExec] Initialized, loaded %d rules (async: max %d tasks)",
                 (int)rules.size(), MAX_ASYNC_TASKS);

    // 初始化完成：调度一次性启动上报（实际上报等 MQTT 连接成功后由 processBootReport 执行）
    scheduleBootReport();

    return loaded;
}

// ========== CRUD（带互斥量保护） ==========

bool PeriphExecManager::ruleNeedsModbus(const PeriphExecRule& rule) const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_COIL_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE)) {
            return true;
        }
        // 检查目标外设是否为 Modbus 类型
        if (!action.targetPeriphId.isEmpty()) {
            PeripheralConfig cfg;
            if (pm.getPeripheralSnapshot(action.targetPeriphId, cfg) && cfg.isModbusPeripheral()) return true;
        }
    }
    return false;
}

bool PeriphExecManager::ruleHasPollCollectionAction(const PeriphExecRule& rule) const {
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            return true;
        }
    }
    return false;
}

bool PeriphExecManager::ruleHasReportableStateAction(const PeriphExecRule& rule) const {
    // 与 Executor::reportRuleCurrentState 的采集范围保持一致：
    //   传感器读取 / Modbus 轮询 / 可无副作用回读的 GPIO 输出控制。
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        switch (static_cast<ExecActionType>(action.actionType)) {
            case ExecActionType::ACTION_SENSOR_READ:
            case ExecActionType::ACTION_MODBUS_POLL:
            case ExecActionType::ACTION_HIGH:
            case ExecActionType::ACTION_LOW:
            case ExecActionType::ACTION_HIGH_INVERTED:
            case ExecActionType::ACTION_LOW_INVERTED:
            case ExecActionType::ACTION_SET_PWM:
            case ExecActionType::ACTION_SET_DAC:
            case ExecActionType::ACTION_GPIO_TOGGLE:
                return true;
            case ExecActionType::ACTION_CALL_PERIPHERAL: {
                if (action.targetPeriphId.isEmpty()) break;
                PeripheralConfig cfg;
                if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg)) break;
                if (cfg.type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
                    cfg.type == PeripheralType::GPIO_PWM_OUTPUT ||
                    cfg.type == PeripheralType::GPIO_ANALOG_OUTPUT) {
                    return true;
                }
                break;
            }
            default:
                break;
        }
    }
    return false;
}

bool PeriphExecManager::shouldAvoidSyncFallback(const PeriphExecRule& rule) const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_COIL_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            return true;
        }
        // 检查目标外设是否为 Modbus 类型（Modbus 操作需要异步执行避免阻塞）
        if (!action.targetPeriphId.isEmpty()) {
            PeripheralConfig cfg;
            if (pm.getPeripheralSnapshot(action.targetPeriphId, cfg) && cfg.isModbusPeripheral()) return true;
        }
    }
    return false;
}

bool PeriphExecManager::sanitizeTriggerForSafety(ExecTrigger& trigger,
                                                 bool hasPollCollectionAction,
                                                 const String& ruleName) const {
    uint32_t originalIntervalSec = trigger.intervalSec;
    uint16_t originalTimeout = trigger.pollResponseTimeout;
    uint8_t originalRetries = trigger.pollMaxRetries;
    uint16_t originalInterDelay = trigger.pollInterPollDelay;

    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER)) {
        if (trigger.intervalSec < PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC;
        } else if (trigger.intervalSec > PERIPH_EXEC_MAX_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MAX_INTERVAL_SEC;
        }
        // 缓存 timePoint 解析结果，避免 checkTimerTriggers 每秒在锁内重复解析字符串
        if (trigger.timerMode == 1 && trigger.timePoint.length() >= 5) {
            int colonIdx = trigger.timePoint.indexOf(':');
            if (colonIdx > 0) {
                trigger._cachedHour = static_cast<int8_t>(trigger.timePoint.substring(0, colonIdx).toInt());
                trigger._cachedMinute = static_cast<int8_t>(trigger.timePoint.substring(colonIdx + 1).toInt());
            } else {
                trigger._cachedHour = -1;
                trigger._cachedMinute = -1;
            }
        } else {
            trigger._cachedHour = -1;
            trigger._cachedMinute = -1;
        }
    }

    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
        // Poll 会占用串行总线和 worker；与 Scheduler 的 5 秒下限保持一致，
        // 使保存值、API 回显和实际运行值不再出现 1s/5s 分裂。
        if (trigger.intervalSec < PERIPH_EXEC_MIN_POLL_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MIN_POLL_INTERVAL_SEC;
        } else if (trigger.intervalSec > PERIPH_EXEC_MAX_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MAX_INTERVAL_SEC;
        }

        if (trigger.pollResponseTimeout < PERIPH_EXEC_MIN_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = PERIPH_EXEC_MIN_POLL_TIMEOUT_MS;
        } else if (trigger.pollResponseTimeout > PERIPH_EXEC_MAX_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = PERIPH_EXEC_MAX_POLL_TIMEOUT_MS;
        }

        if (trigger.pollMaxRetries > PERIPH_EXEC_MAX_POLL_RETRIES) {
            trigger.pollMaxRetries = PERIPH_EXEC_MAX_POLL_RETRIES;
        }

        if (trigger.pollInterPollDelay < PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS;
        } else if (trigger.pollInterPollDelay > PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS;
        }

        if (hasPollCollectionAction) {
            if (trigger.pollResponseTimeout > PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS) {
                trigger.pollResponseTimeout = PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS;
            }
            if (trigger.pollMaxRetries > PERIPH_EXEC_HEAVY_POLL_RETRIES) {
                trigger.pollMaxRetries = PERIPH_EXEC_HEAVY_POLL_RETRIES;
            }
            if (trigger.pollInterPollDelay < PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS) {
                trigger.pollInterPollDelay = PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS;
            }
        }
    }

    const bool modified = originalIntervalSec != trigger.intervalSec ||
                          originalTimeout != trigger.pollResponseTimeout ||
                          originalRetries != trigger.pollMaxRetries ||
                          originalInterDelay != trigger.pollInterPollDelay;
    if (modified) {
        LOGGER.warningf("[PeriphExec] Sanitized trigger params for rule '%s' (type=%d, interval=%lu, timeout=%u, retries=%u, delay=%u)",
                        ruleName.c_str(),
                        trigger.triggerType,
                        static_cast<unsigned long>(trigger.intervalSec),
                        static_cast<unsigned int>(trigger.pollResponseTimeout),
                        static_cast<unsigned int>(trigger.pollMaxRetries),
                        static_cast<unsigned int>(trigger.pollInterPollDelay));
    }
    return modified;
}

bool PeriphExecManager::sanitizeRuleForSafety(PeriphExecRule& rule) const {
    bool hasPollCollectionAction = ruleHasPollCollectionAction(rule);
    bool modified = false;
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL) && rule.enabled) {
            rule.enabled = false;
            modified = true;
        }
    }
    for (auto& trigger : rule.triggers) {
        if (trigger.triggerPeriphId.startsWith("remote.") && rule.enabled) {
            rule.enabled = false; modified = true;
        }
        modified = sanitizeTriggerForSafety(trigger, hasPollCollectionAction,
                                             rule.name) || modified;
    }
    // 缓存规则级别标志，避免热路径重复遍历 actions
    rule._cachedHasPollCollectionAction = hasPollCollectionAction;
    rule._cachedNeedsModbus = ruleNeedsModbus(rule);
    return modified;
}

unsigned long PeriphExecManager::getPollTriggerCooldownMs(const PeriphExecRule& rule, const String& source) const {
    if ((source == "modbus" || source == "modbus_poll") && rule._cachedHasPollCollectionAction) {
        return PERIPH_EXEC_HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS;
    }
    return PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS;
}

bool PeriphExecManager::shouldThrottlePollIngress(const String& source, unsigned long now) {
    if (source != "modbus_poll" || _pollIngressMutex == nullptr) {
        return false;
    }

    MutexGuard lock(_pollIngressMutex);
    if (!lock.isLocked()) {
        return false;
    }

    unsigned long& lastAccepted = _pollSourceLastAccepted[source];
    if (lastAccepted > 0 && (now - lastAccepted) < PERIPH_EXEC_MODBUS_POLL_INGRESS_MIN_INTERVAL_MS) {
        unsigned long& lastLog = _pollSourceLastThrottleLog[source];
        if (lastLog == 0 || (now - lastLog) >= PERIPH_EXEC_POLL_THROTTLE_LOG_INTERVAL_MS) {
            LOGGER.warningf("[PeriphExec] Throttle poll ingress from %s (%lums since last accept)",
                            source.c_str(),
                            now - lastAccepted);
            lastLog = now;
        }
        return true;
    }

    lastAccepted = now;
    return false;
}

bool PeriphExecManager::addRule(const PeriphExecRule& rule) {
    String errorMsg;
    return addRule(rule, errorMsg);
}

bool PeriphExecManager::addRule(const PeriphExecRule& rule, String& errorMsg) {
    errorMsg = "";
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) {
        errorMsg = "Rule service is busy, try again";
        return false;
    }

    const bool added = addRuleLocked(rule, errorMsg);
    if (added) refreshRuleRuntimeIndexesLocked();
    return added;
}

bool PeriphExecManager::validateRuleActionsLocked(
    const PeriphExecRule& rule, String& errorMsg) const {
    for (const auto& trigger : rule.triggers) if (trigger.triggerPeriphId.startsWith("remote.")) {
        errorMsg = "REMOTE_CHANNEL_RETIRED: replace the retired remote source"; return false;
    }
    PeripheralManager& peripherals = PeripheralManager::getInstance();
    bool hasRecordableAction = false;

    for (size_t index = 0; index < rule.actions.size(); ++index) {
        const ExecAction& action = rule.actions[index];
        const String actionPath = String("Action ") + String(index + 1U);

        if (action.actionType >
                static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ) ||
            action.actionType ==
                static_cast<uint8_t>(ExecActionType::ACTION_RESERVED_20)) {
            errorMsg = "ACTION_TYPE_UNSUPPORTED: " + actionPath;
            return false;
        }

        if (rule.allowPanelControl && !isPanelSafeAction(action.actionType)) {
            errorMsg = "PANEL_ACTION_NOT_ALLOWED: " + actionPath;
            return false;
        }

        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ)) {
            hasRecordableAction = true;
        }

        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
            errorMsg = "REMOTE_CHANNEL_RETIRED: replace the retired remote channel action";
            return false;
        }
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ)) {
            if (action.targetPeriphId.isEmpty() || !action.actionValue.isEmpty() ||
                action.useReceivedValue) {
                errorMsg = "ACTION_VALUE_INVALID: remote read requires an ID and no value";
                return false;
            }
            continue;
        }

        // Factory reset is deliberately not an automatable rule action. It is
        // only legal through the dedicated reset endpoint with fresh password
        // authentication and a one-time RESET confirmation.
        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_SYS_FACTORY_RESET)) {
            errorMsg = "ACTION_NOT_AUTOMATABLE: factory reset cannot be saved in a rule";
            return false;
        }
        if (isTerminalSystemAction(action.actionType) &&
            index + 1U != rule.actions.size()) {
            errorMsg = "ACTION_ORDER_INVALID: restart and feature actions must be last";
            return false;
        }

        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_TRIGGER_EVENT)) {
            if (action.targetPeriphId.isEmpty()) {
                errorMsg = "ACTION_TARGET_REQUIRED: trigger-event action requires an event ID";
                return false;
            }
            continue;
        }

        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_ENABLE_EXEC_RULE) ||
            action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_DISABLE_EXEC_RULE)) {
            if (action.targetPeriphId.isEmpty()) {
                errorMsg = "ACTION_TARGET_REQUIRED: rule-control action requires a rule ID";
                return false;
            }
            if (rules.count(action.targetPeriphId) == 0) {
                errorMsg = String("ACTION_TARGET_NOT_FOUND: target rule '") +
                           action.targetPeriphId + "' does not exist";
                return false;
            }
            continue;
        }

        const bool requiresPeripheral =
            actionRequiresPeripheralTarget(action.actionType);
        if (requiresPeripheral && action.targetPeriphId.isEmpty()) {
            errorMsg = "ACTION_TARGET_REQUIRED: " + actionPath +
                       " requires a target peripheral";
            return false;
        }

        // SENSOR_READ and CALL_PERIPHERAL can carry an embedded periphId.
        // Never allow that secondary field to redirect execution away from the
        // target which was validated and shown to the user.
        String jsonValue = action.actionValue;
        jsonValue.trim();
        const bool jsonAction = !jsonValue.isEmpty() && jsonValue[0] == '{';
        const bool sensorRead = action.actionType == static_cast<uint8_t>(
                                    ExecActionType::ACTION_SENSOR_READ);
        const bool jsonRequired = sensorRead;
        String embeddedPeriphId;
        if (jsonAction) {
            JsonDocument actionDocument;
            const DeserializationError jsonError =
                deserializeJson(actionDocument, jsonValue);
            if (jsonError) {
                if (jsonRequired || action.actionType == static_cast<uint8_t>(
                        ExecActionType::ACTION_CALL_PERIPHERAL)) {
                    errorMsg = "ACTION_VALUE_INVALID: " + actionPath +
                               " contains invalid JSON";
                    return false;
                }
            } else if (actionDocument["periphId"].is<const char*>()) {
                embeddedPeriphId =
                    actionDocument["periphId"].as<const char*>();
            }
        } else if (jsonRequired) {
            errorMsg = "ACTION_VALUE_INVALID: sensor-read action requires JSON parameters";
            return false;
        }

        if (sensorRead && embeddedPeriphId.isEmpty()) {
            errorMsg = "ACTION_TARGET_REQUIRED: sensor-read actionValue.periphId is required";
            return false;
        }
        if (!embeddedPeriphId.isEmpty() &&
            embeddedPeriphId != action.targetPeriphId) {
            errorMsg = "ACTION_TARGET_CONFLICT: actionValue.periphId must match targetPeriphId";
            return false;
        }

        if (!requiresPeripheral) continue;

        PeripheralConfig target;
        if (!peripherals.getPeripheralSnapshot(action.targetPeriphId, target)) {
            errorMsg = String("ACTION_TARGET_NOT_FOUND: peripheral '") +
                       action.targetPeriphId + "' does not exist";
            return false;
        }
        if (!actionMatchesPeripheralTarget(action.actionType, target)) {
            errorMsg = String("ACTION_TARGET_TYPE_MISMATCH: action ") +
                       String(action.actionType) + " is incompatible with " +
                       getPeripheralTypeName(target.type);
            return false;
        }
        // A disabled target is valid for pre-configuration. Runtime execution
        // remains fail-closed until the peripheral is enabled and initialized.
    }
    if (rule.recordHistory && !hasRecordableAction) {
        errorMsg = "HISTORY_SOURCE_REQUIRED: recordHistory requires a numeric collection action";
        return false;
    }
    return true;
}

bool PeriphExecManager::addRuleLocked(const PeriphExecRule& rule,
                                      String& errorMsg,
                                      String* storedId) {
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
    PeriphExecRule newRule = rule;
    if (newRule.id.isEmpty()) {
        newRule.id = generateUniqueId();
    }
    if (rules.count(newRule.id)) {
        LOGGER.warningf("[PeriphExec] Rule ID already exists: %s", newRule.id.c_str());
        errorMsg = "Rule ID already exists";
        return false;
    }

    // Rules and their indexes are resident for the lifetime of the device. A
    // hard profile cap keeps RAM use bounded and matches loadConfiguration().
    if (rules.size() >= FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) {
        LOGGER.warningf("[PeriphExec] Rule limit reached for profile %s (max=%u)",
                        FastBee::ResourceProfile::NAME,
                        static_cast<unsigned int>(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES));
        errorMsg = String("Rule limit reached for profile ") +
                   FastBee::ResourceProfile::NAME + " (max " +
                   String(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) + ")";
        return false;
    }

    // 上限检查
    if (newRule.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        LOGGER.warningf("[PeriphExec] Too many triggers: %d", (int)newRule.triggers.size());
        errorMsg = String("Too many triggers (max ") + String(MAX_TRIGGERS_PER_RULE) + ")";
        return false;
    }
    if (newRule.actions.size() > MAX_ACTIONS_PER_RULE) {
        LOGGER.warningf("[PeriphExec] Too many actions: %d", (int)newRule.actions.size());
        errorMsg = String("Too many actions (max ") + String(MAX_ACTIONS_PER_RULE) + ")";
        return false;
    }
    if (!validateRuleProfileLimits(newRule, "addRule", &errorMsg)) {
        return false;
    }
    if (!validateRuleActionsLocked(newRule, errorMsg)) return false;
    sanitizeRuleForSafety(newRule);
    // 重置运行时字段
    for (auto& t : newRule.triggers) {
        t.lastTriggerTime = 0;
        t.triggerCount = 0;
    }
    rules[newRule.id] = newRule;
    if (storedId) *storedId = newRule.id;
    LOGGER.infof("[PeriphExec] Added rule: %s (%s)", newRule.id.c_str(), newRule.name.c_str());
    return true;
}

bool PeriphExecManager::updateRule(const String& id, const PeriphExecRule& rule) {
    String errorMsg;
    return updateRule(id, rule, errorMsg);
}

bool PeriphExecManager::updateRule(const String& id, const PeriphExecRule& rule, String& errorMsg) {
    errorMsg = "";
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) {
        errorMsg = "Rule service is busy, try again";
        return false;
    }

    const bool updated = updateRuleLocked(id, rule, errorMsg);
    if (updated) refreshRuleRuntimeIndexesLocked();
    return updated;
}

bool PeriphExecManager::updateRuleLocked(const String& id,
                                         const PeriphExecRule& rule,
                                         String& errorMsg) {
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
    auto it = rules.find(id);
    if (it == rules.end()) {
        LOGGER.warningf("[PeriphExec] Rule not found for update: %s", id.c_str());
        errorMsg = "Rule not found";
        return false;
    }
    // 上限检查
    if (rule.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        errorMsg = String("Too many triggers (max ") + String(MAX_TRIGGERS_PER_RULE) + ")";
        return false;
    }
    if (rule.actions.size() > MAX_ACTIONS_PER_RULE) {
        errorMsg = String("Too many actions (max ") + String(MAX_ACTIONS_PER_RULE) + ")";
        return false;
    }
    if (!validateRuleProfileLimits(rule, "updateRule", &errorMsg)) return false;
    if (!validateRuleActionsLocked(rule, errorMsg)) return false;

    // 字段校验：名称不能为空，至少有一个触发器和一个动作
    if (rule.name.isEmpty()) {
        LOGGER.warning("[PeriphExec] updateRule: name is empty");
        errorMsg = "Rule name is required";
        return false;
    }
    if (rule.triggers.empty()) {
        LOGGER.warning("[PeriphExec] updateRule: no triggers");
        errorMsg = "At least one trigger is required";
        return false;
    }
    if (rule.actions.empty()) {
        LOGGER.warning("[PeriphExec] updateRule: no actions");
        errorMsg = "At least one action is required";
        return false;
    }
    // 校验触发器类型有效性 (0~4)
    for (const auto& t : rule.triggers) {
        if (t.triggerType > 5) {
            LOGGER.warningf("[PeriphExec] updateRule: invalid triggerType %d", (int)t.triggerType);
            errorMsg = "Invalid trigger type";
            return false;
        }
    }

    // 保留 per-trigger 运行时字段：按索引匹配旧触发器
    const auto& oldTriggers = it->second.triggers;
    PeriphExecRule updated = rule;
    updated.id = id;
    sanitizeRuleForSafety(updated);
    for (size_t i = 0; i < updated.triggers.size(); i++) {
        if (i < oldTriggers.size()) {
            updated.triggers[i].lastTriggerTime = oldTriggers[i].lastTriggerTime;
            updated.triggers[i].triggerCount = oldTriggers[i].triggerCount;
        } else {
            updated.triggers[i].lastTriggerTime = 0;
            updated.triggers[i].triggerCount = 0;
        }
    }
    rules[id] = updated;
    LOGGER.infof("[PeriphExec] Updated rule: %s", id.c_str());
    return true;
}

bool PeriphExecManager::removeRule(const String& id) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;

    String errorMsg;
    const bool removed = removeRuleLocked(id, errorMsg);
    if (removed) refreshRuleRuntimeIndexesLocked();
    return removed;
}

bool PeriphExecManager::removeRuleLocked(const String& id, String& errorMsg) {
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
    auto it = rules.find(id);
    if (it == rules.end()) {
        errorMsg = "Rule not found";
        return false;
    }
    rules.erase(it);
    LOGGER.infof("[PeriphExec] Removed rule: %s", id.c_str());
    return true;
}

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
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
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
        if (trigger.triggerPeriphId.startsWith("remote.")) { errorMsg = "REMOTE_CHANNEL_RETIRED: rule remains paused"; return false; }
    }
    it->second.enabled = enabled;
    return true;
}

void PeriphExecManager::refreshRuleRuntimeIndexesLocked(bool validateScheduler) {
    rebuildButtonEventCache();
    rebuildDataSourceCache();
    if (validateScheduler && _scheduler) _scheduler->validateLoadedConfig();
}

bool PeriphExecManager::addRuleAndSave(const PeriphExecRule& rule,
                                       String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::ADD, rule.id,
                                          &rule, errorMsg);
}

bool PeriphExecManager::updateRuleAndSave(const String& id,
                                          const PeriphExecRule& rule,
                                          String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::UPDATE, id,
                                          &rule, errorMsg);
}

bool PeriphExecManager::removeRuleAndSave(const String& id, String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::REMOVE, id,
                                          nullptr, errorMsg);
}

bool PeriphExecManager::enableRuleAndSave(const String& id, String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::ENABLE, id,
                                          nullptr, errorMsg);
}

bool PeriphExecManager::disableRuleAndSave(const String& id, String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::DISABLE, id,
                                          nullptr, errorMsg);
}

bool PeriphExecManager::applyRuleMutationTransactional(
    RuleMutationType operation, const String& id,
    const PeriphExecRule* rule, String& errorMsg) {
    errorMsg = "";
    MutexGuard lock(_rulesMutex,
                    pdMS_TO_TICKS(PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS));
    if (!lock.isLocked()) {
        errorMsg = "RULE_SERVICE_BUSY: rule configuration is locked; retry in 2s";
        return false;
    }
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);

    RuleMutationSnapshot snapshot;
    snapshot.id = id;
    if (!id.isEmpty()) {
        auto existing = rules.find(id);
        if (existing != rules.end()) {
            snapshot.existed = true;
            snapshot.rule = existing->second;
        }
    }

    String resultId = id;
    bool mutated = false;
    switch (operation) {
        case RuleMutationType::ADD:
            if (!rule) {
                errorMsg = "INVALID_RULE_MUTATION: add requires a rule";
                return false;
            }
            mutated = addRuleLocked(*rule, errorMsg, &resultId);
            break;
        case RuleMutationType::UPDATE:
            if (!rule) {
                errorMsg = "INVALID_RULE_MUTATION: update requires a rule";
                return false;
            }
            mutated = updateRuleLocked(id, *rule, errorMsg);
            break;
        case RuleMutationType::REMOVE:
            mutated = removeRuleLocked(id, errorMsg);
            break;
        case RuleMutationType::ENABLE:
            mutated = setRuleEnabledLocked(id, true, errorMsg);
            break;
        case RuleMutationType::DISABLE:
            mutated = setRuleEnabledLocked(id, false, errorMsg);
            break;
    }

    if (!mutated) return false;

    if (saveConfigurationLocked()) {
        // Derived indexes are read without _rulesMutex. Publish the mutation
        // only after the atomic file commit succeeds.
        refreshRuleRuntimeIndexesLocked(false);
        if (_scheduler) _scheduler->validateLoadedConfig();
        return true;
    }

    const bool restored = restoreRuleMutationLocked(snapshot, resultId);
    if (restored) {
        errorMsg = "PERSISTENCE_ROLLBACK_OK: rule was not saved; "
                   "in-memory rule and runtime indexes were restored";
    } else {
        errorMsg = "ROLLBACK_FAILED: rule was not saved and runtime state "
                   "could not be restored completely";
    }
    return false;
}

bool PeriphExecManager::restoreRuleMutationLocked(
    const RuleMutationSnapshot& snapshot, const String& resultId) {
    if (snapshot.existed) {
        // UPDATE/ENABLE/DISABLE keep the map node alive so rollback does not
        // require a fresh allocation on an already memory-constrained device.
        auto existing = rules.find(snapshot.id);
        if (existing != rules.end()) {
            existing->second = snapshot.rule;
        } else {
            rules[snapshot.id] = snapshot.rule;
        }
        if (!resultId.isEmpty() && resultId != snapshot.id) {
            rules.erase(resultId);
        }
    } else {
        if (!resultId.isEmpty()) rules.erase(resultId);
        if (!snapshot.id.isEmpty() && snapshot.id != resultId) {
            rules.erase(snapshot.id);
        }
    }

    bool restored = false;
    if (snapshot.existed) {
        auto it = rules.find(snapshot.id);
        restored = it != rules.end() && rulesMatch(it->second, snapshot.rule);
    } else {
        restored = (resultId.isEmpty() || rules.count(resultId) == 0) &&
                   (snapshot.id.isEmpty() || rules.count(snapshot.id) == 0);
    }

    // The snapshot is the exact pre-mutation runtime state. Scheduler safety
    // validation could modify it again, so rollback only rebuilds derived indexes.
    refreshRuleRuntimeIndexesLocked(false);
    return restored;
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

bool PeriphExecManager::updateSensorReadCache(
    const String& key, const String& label, const String& value, const String& unit,
    const String& dataField, const String& periphId, uint32_t sampleTimestamp,
    uint32_t maxAgeMs, bool recordable, uint32_t sampleIdentity,
    const String& sourceRevision, uint32_t configVersion) {
    ObservationUpdate update;
    update.sourceRef = key;
    update.label = label;
    update.value = value;
    update.unit = unit;
    update.valueType = "number";
    update.dataField = dataField;
    update.periphId = periphId;
    update.sourceRevision = sourceRevision.isEmpty()
        ? periphId + ":" + dataField + ":" + unit : sourceRevision;
    update.quality = ObservationQuality::GOOD;
    update.sampleTimestamp = sampleTimestamp;
    update.maxAgeMs = maxAgeMs;
    update.sampleIdentity = sampleIdentity;
    update.recordable = recordable;
    return upsertObservation(update, configVersion);
}

bool PeriphExecManager::upsertObservation(const ObservationUpdate& update, uint32_t configVersion) {
    static std::atomic<uint32_t> lastLockWarn{0};
    if (update.sourceRef.isEmpty() || update.sourceRef.length() > 96 ||
        !_sensorCacheMutex) return false;
    if (configVersion && (update.label.length() > 128 || update.value.length() > 96 ||
        update.unit.length() > 32 || update.dataField.length() > 64 || update.periphId.length() > 40 ||
        update.sourceRevision.length() > 192 || update.valueType.length() > 16 || update.error.length() > 160)) return false;

    const unsigned long now = millis();
    ObservationUpdate accepted = update;
    uint32_t publicationToken = 0;
    auto commit = [&]() {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        auto existing = _sensorReadCache.find(update.sourceRef);
        if (_sensorReadCache.size() >= SENSOR_CACHE_MAX_ENTRIES &&
            existing == _sensorReadCache.end()) {
            auto oldest = _sensorReadCache.begin();
            for (auto it = _sensorReadCache.begin(); it != _sensorReadCache.end(); ++it) {
                if (it->second.timestamp < oldest->second.timestamp) oldest = it;
            }
            _sensorReadCache.erase(oldest);
        }

        SensorReadCache& cache = _sensorReadCache[update.sourceRef];
        cache.label = update.label;
        cache.unit = update.unit;
        cache.dataField = update.dataField;
        cache.periphId = update.periphId;
        cache.valueType = update.valueType.isEmpty() ? String("number") : update.valueType;
        cache.sourceRevision = update.sourceRevision;
        cache.maxAgeMs = update.maxAgeMs;
        cache.recordable = update.recordable;
        cache.bindingValid = update.bindingValid;
        cache.quality = update.quality;
        cache.error = update.error;
        cache.configVersion = configVersion;
        if (++_nextSensorPublication == 0) ++_nextSensorPublication;
        cache.publicationToken = publicationToken = _nextSensorPublication;
        cache.pendingPublication = true;
        if (update.quality == ObservationQuality::GOOD) {
            cache.value = update.value;
            cache.timestamp = update.sampleTimestamp ? update.sampleTimestamp : now;
            cache.sampleIdentity = update.sampleIdentity
                ? update.sampleIdentity : ++_nextObservationIdentity;
            accepted.sampleTimestamp = cache.timestamp;
            accepted.sampleIdentity = cache.sampleIdentity;
            cache.error = "";
        }
        return true;
    };
    // Only bounded cache work is performed under the configuration guard.
    // Unbound observations (e.g. network sources) keep their existing API.
    const bool cacheUpdated = configVersion
        ? PeripheralManager::getInstance().commitSensorRead(configVersion, commit) : commit();

    if (!cacheUpdated) {
        if (configVersion && !FastBee::SensorConfigEpoch::matches(configVersion)) return false;
        uint32_t lastWarning = lastLockWarn.load(std::memory_order_relaxed);
        if ((lastWarning == 0 || (now - lastWarning) >= SENSOR_CACHE_LOCK_WARN_INTERVAL_MS) &&
            lastLockWarn.compare_exchange_strong(lastWarning, static_cast<uint32_t>(now))) {
            LOGGER.warning("[PeriphExec] Observation update skipped: mutex unavailable");
        }
        return false;
    }

    auto claim = [&]() {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        const auto it = _sensorReadCache.find(update.sourceRef);
        if (it == _sensorReadCache.end() || !it->second.pendingPublication ||
            it->second.configVersion != configVersion ||
            it->second.publicationToken != publicationToken) return false;
        const unsigned long maxAge = it->second.maxAgeMs
            ? it->second.maxAgeMs : SENSOR_CACHE_TTL_MS;
        if (it->second.quality == ObservationQuality::GOOD &&
            static_cast<unsigned long>(millis() - it->second.timestamp) > maxAge) return false;
        it->second.pendingPublication = false;
        return true;
    };
    // Claim is the publication linearization point. Mutations discard events
    // not yet claimed; transport delivery and callbacks occur outside locks.
    const bool claimed = configVersion
        ? PeripheralManager::getInstance().commitSensorRead(configVersion, claim) : claim();
    if (!claimed) return false;

    publishObservation(accepted);
    return true;
}

void PeriphExecManager::publishObservation(const ObservationUpdate& accepted) {
    if (accepted.quality == ObservationQuality::GOOD) {
        notifySensorDataSSE(accepted.sourceRef, accepted.label, accepted.value, accepted.unit);
        if (_scheduler) {
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            JsonObject item = arr.add<JsonObject>();
            item["id"] = accepted.sourceRef;
            item["value"] = accepted.value;
            String payload;
            serializeJson(doc, payload);
            _scheduler->handlePollData("sensor_cache", payload);
        }

        if (accepted.recordable &&
            (accepted.valueType.isEmpty() || accepted.valueType == "number")) {
            const char* begin = accepted.value.c_str();
            char* end = nullptr;
            const double numeric = strtod(begin, &end);
            while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
            if (end && end != begin && *end == '\0' && std::isfinite(numeric)) {
#if FASTBEE_ENABLE_POINT_HISTORY
                FastBee::PointHistory::instance().recordSample(
                    accepted.sourceRef, numeric, accepted.unit,
                    accepted.sourceRevision, accepted.sampleIdentity,
                    accepted.sampleTimestamp);
#endif
            }
        }
    }
}

bool PeriphExecManager::upsertSensorObservationBatch(
    const ObservationUpdate* const* updates, size_t count, uint32_t configVersion) {
    if (!updates || count < 2 || count > 4 || !_sensorCacheMutex ||
        !configVersion || !FastBee::SensorConfigEpoch::matches(configVersion)) return false;
    // Keep preparation off the worker's small stack. Steady-state updates do
    // not allocate map nodes; only first publication needs staged new nodes.
    std::unique_ptr<SensorReadCache[]> prepared(new (std::nothrow) SensorReadCache[count]);
    if (!prepared) return false;
    std::map<String, SensorReadCache> additions;
    bool missing[4] = {};
    {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!updates[i]) return false;
            missing[i] = _sensorReadCache.find(updates[i]->sourceRef) == _sensorReadCache.end();
        }
    }
    try {
        for (size_t i = 0; i < count; ++i) {
            const auto& update = *updates[i];
            if (update.sourceRef.isEmpty() || update.sourceRef.length() > 96 ||
                update.label.length() > 128 || update.value.isEmpty() || update.value.length() > 96 ||
                update.unit.length() > 32 || update.dataField.length() > 64 ||
                update.periphId.isEmpty() || update.periphId.length() > 40 ||
                update.sourceRevision.length() > 192 || update.valueType != "number" ||
                update.quality != ObservationQuality::GOOD || !update.error.isEmpty() ||
                !update.sampleTimestamp || !update.maxAgeMs ||
                update.periphId != updates[0]->periphId ||
                update.sampleTimestamp != updates[0]->sampleTimestamp) return false;
            for (size_t j = 0; j < i; ++j) {
                if (update.sourceRef == updates[j]->sourceRef) return false;
            }
            auto& cache = prepared[i];
            cache.label = update.label; cache.value = update.value; cache.unit = update.unit;
            cache.dataField = update.dataField; cache.periphId = update.periphId;
            cache.valueType = update.valueType; cache.sourceRevision = update.sourceRevision;
            // Arduino String reports allocation failure by becoming empty,
            // whereas STL allocation throws. Reject either before committing.
            if (cache.label != update.label || cache.value != update.value || cache.unit != update.unit ||
                cache.dataField != update.dataField || cache.periphId != update.periphId ||
                cache.valueType != update.valueType || cache.sourceRevision != update.sourceRevision) return false;
            cache.timestamp = update.sampleTimestamp; cache.maxAgeMs = update.maxAgeMs;
            cache.sampleIdentity = update.sampleIdentity;
            cache.recordable = update.recordable; cache.bindingValid = update.bindingValid;
            cache.quality = update.quality; cache.configVersion = configVersion;
            if (missing[i]) {
                additions.try_emplace(update.sourceRef);
                if (additions.find(update.sourceRef) == additions.end()) return false;
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    uint32_t publicationToken = 0;
    auto commit = [&]() {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        size_t newCount = 0;
        const uint32_t now = static_cast<uint32_t>(millis());
        for (size_t i = 0; i < count; ++i) {
            const auto& update = *updates[i];
            if (static_cast<uint32_t>(now - update.sampleTimestamp) > update.maxAgeMs) return false;
            if (_sensorReadCache.find(update.sourceRef) == _sensorReadCache.end()) {
                if (additions.find(update.sourceRef) == additions.end()) return false;
                ++newCount;
            }
        }
        if (_sensorReadCache.size() + newCount > SENSOR_CACHE_MAX_ENTRIES) return false;
        if (++_nextSensorPublication == 0) ++_nextSensorPublication;
        publicationToken = _nextSensorPublication;
        for (size_t i = 0; i < count; ++i) {
            const auto& update = *updates[i];
            auto found = _sensorReadCache.find(update.sourceRef);
            if (found == _sensorReadCache.end()) {
                found = _sensorReadCache.insert(additions.extract(update.sourceRef)).position;
            }
            prepared[i].publicationToken = publicationToken;
            prepared[i].pendingPublication = true;
            // Every allocation completed above. A reader cannot observe a
            // partially replaced group while this cache lock is held.
            found->second = std::move(prepared[i]);
        }
        return true;
    };
    try {
        if (!PeripheralManager::getInstance().commitSensorRead(configVersion, commit)) return false;
    } catch (const std::bad_alloc&) {
        return false; // std::function construction must also precede mutation.
    }
    auto claim = [&]() {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        const uint32_t now = static_cast<uint32_t>(millis());
        for (size_t i = 0; i < count; ++i) {
            const auto found = _sensorReadCache.find(updates[i]->sourceRef);
            if (found == _sensorReadCache.end() || !found->second.pendingPublication ||
                found->second.configVersion != configVersion ||
                found->second.publicationToken != publicationToken ||
                static_cast<uint32_t>(now - found->second.timestamp) > found->second.maxAgeMs) return false;
        }
        for (size_t i = 0; i < count; ++i) {
            _sensorReadCache.find(updates[i]->sourceRef)->second.pendingPublication = false;
        }
        return true;
    };
    try {
        if (!PeripheralManager::getInstance().commitSensorRead(configVersion, claim)) return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) publishObservation(*updates[i]);
    return true;
}

void PeriphExecManager::invalidateSensorReadCache(const String& periphId, const String& reason,
                                                  uint32_t configVersion) {
    if (periphId.isEmpty()) return;
    const String keyPrefix = periphId + "_";
    size_t marked = 0;
    auto invalidate = [&]() {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        for (auto& entry : _sensorReadCache) {
            if (entry.second.periphId == periphId ||
                (entry.second.periphId.isEmpty() && entry.first.startsWith(keyPrefix))) {
                entry.second.quality = ObservationQuality::ERROR;
                entry.second.error = reason;
                entry.second.pendingPublication = false;
                ++marked;
            }
        }
        return true;
    };
    if (configVersion) PeripheralManager::getInstance().commitSensorRead(configVersion, invalidate);
    else invalidate();

    if (marked > 0) {
        LOGGER.warningf("[PeriphExec] Marked %u cached sensor value(s) invalid for %s: %s",
                        static_cast<unsigned>(marked), periphId.c_str(), reason.c_str());
    }
}

void PeriphExecManager::invalidateAllSensorReadCache() {
    MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
    if (!guard.isLocked()) return;
    for (auto it = _sensorReadCache.begin(); it != _sensorReadCache.end();) {
        if (it->second.configVersion) it = _sensorReadCache.erase(it);
        else ++it;
    }
}

std::map<String, PeriphExecManager::SensorReadCache> PeriphExecManager::getSensorReadCacheCopy() const {
    std::map<String, SensorReadCache> copy;
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return copy;
    if (_sensorCacheMutex && xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const unsigned long now = millis();
        for (const auto& entry : _sensorReadCache) {
            const unsigned long maxAge = entry.second.maxAgeMs
                ? min(entry.second.maxAgeMs, SENSOR_CACHE_TTL_MS) : SENSOR_CACHE_TTL_MS;
            if ((!entry.second.configVersion || FastBee::SensorConfigEpoch::matches(entry.second.configVersion)) &&
                entry.second.quality == ObservationQuality::GOOD &&
                static_cast<unsigned long>(now - entry.second.timestamp) <= maxAge) {
                copy.emplace(entry.first, entry.second);
            }
        }
        xSemaphoreGive(_sensorCacheMutex);
    }
    return copy;
}

bool PeriphExecManager::lookupSensorCache(const String& key, SensorReadCache& out) const {
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return false;
    if (!_sensorCacheMutex || xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool found = false;
    auto it = _sensorReadCache.find(key);
    if (it != _sensorReadCache.end()) {
        const unsigned long maxAge = it->second.maxAgeMs
            ? min(it->second.maxAgeMs, SENSOR_CACHE_TTL_MS) : SENSOR_CACHE_TTL_MS;
        if ((!it->second.configVersion || FastBee::SensorConfigEpoch::matches(it->second.configVersion)) &&
            it->second.quality == ObservationQuality::GOOD &&
            static_cast<unsigned long>(millis() - it->second.timestamp) <= maxAge) {
            out = it->second;
            found = true;
        }
    }
    xSemaphoreGive(_sensorCacheMutex);
    return found;
}

bool PeriphExecManager::getObservation(const String& sourceRef,
                                       ObservationSnapshot& out) const {
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return false;
    if (!_sensorCacheMutex || xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const auto it = _sensorReadCache.find(sourceRef);
    if (it == _sensorReadCache.end() || (it->second.configVersion &&
        !FastBee::SensorConfigEpoch::matches(it->second.configVersion))) {
        xSemaphoreGive(_sensorCacheMutex);
        return false;
    }
    const SensorReadCache& cache = it->second;
    out = ObservationSnapshot();
    out.sourceRef = sourceRef;
    out.label = cache.label;
    out.value = cache.value;
    out.unit = cache.unit;
    out.valueType = cache.valueType;
    out.sourceRevision = cache.sourceRevision;
    out.error = cache.error;
    out.dataField = cache.dataField;
    out.periphId = cache.periphId;
    out.quality = cache.quality;
    out.sampleTimestamp = cache.timestamp;
    out.maxAgeMs = cache.maxAgeMs;
    out.sampleIdentity = cache.sampleIdentity;
    out.recordable = cache.recordable;
    out.bindingValid = cache.bindingValid;
    out.hasValue = !cache.value.isEmpty();
    const unsigned long maxAge = cache.maxAgeMs ? cache.maxAgeMs : SENSOR_CACHE_TTL_MS;
    out.stale = cache.timestamp != 0 &&
        static_cast<unsigned long>(millis() - cache.timestamp) > maxAge;
    if (out.stale && out.quality == ObservationQuality::GOOD) {
        out.quality = ObservationQuality::STALE;
    }
    xSemaphoreGive(_sensorCacheMutex);
    return true;
}

bool PeriphExecManager::invalidateObservation(
    const String& sourceRef, const String& reason,
    const String& nextSourceRevision) {
    if (sourceRef.isEmpty() || reason.isEmpty() || reason.length() > 96 ||
        nextSourceRevision.length() > 64) {
        return false;
    }
    bool found = false;
    if (_sensorCacheMutex) {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(20));
        if (!guard.isLocked()) return false;
        auto it = _sensorReadCache.find(sourceRef);
        if (it != _sensorReadCache.end()) {
            it->second.quality = ObservationQuality::OFFLINE;
            it->second.error = reason;
            it->second.pendingPublication = false;
            it->second.bindingValid = false;
            found = true;
        }
    }
#if FASTBEE_ENABLE_POINT_HISTORY
    const bool historyInvalidated = FastBee::PointHistory::instance().invalidateSource(
        sourceRef, reason, nextSourceRevision);
#else
    (void)nextSourceRevision;
    const bool historyInvalidated = false;
#endif
    return found || historyInvalidated;
}

std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(size_t limit) const {
    std::vector<ObservationSnapshot> observations;
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return observations;
    if (limit > SENSOR_CACHE_MAX_ENTRIES) limit = SENSOR_CACHE_MAX_ENTRIES;
    if (!_sensorCacheMutex || xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return observations;
    }
    observations.reserve(limit);
    const unsigned long now = millis();
    for (const auto& entry : _sensorReadCache) {
        if (entry.second.configVersion && !FastBee::SensorConfigEpoch::matches(entry.second.configVersion)) continue;
        if (observations.size() >= limit) break;
        ObservationSnapshot snapshot;
        snapshot.sourceRef = entry.first;
        snapshot.label = entry.second.label;
        snapshot.value = entry.second.value;
        snapshot.unit = entry.second.unit;
        snapshot.valueType = entry.second.valueType;
        snapshot.sourceRevision = entry.second.sourceRevision;
        snapshot.error = entry.second.error;
        snapshot.dataField = entry.second.dataField;
        snapshot.periphId = entry.second.periphId;
        snapshot.quality = entry.second.quality;
        snapshot.sampleTimestamp = entry.second.timestamp;
        snapshot.maxAgeMs = entry.second.maxAgeMs;
        snapshot.sampleIdentity = entry.second.sampleIdentity;
        snapshot.recordable = entry.second.recordable;
        snapshot.bindingValid = entry.second.bindingValid;
        snapshot.hasValue = !entry.second.value.isEmpty();
        const unsigned long maxAge = entry.second.maxAgeMs
            ? entry.second.maxAgeMs : SENSOR_CACHE_TTL_MS;
        snapshot.stale = entry.second.timestamp != 0 &&
            static_cast<unsigned long>(now - entry.second.timestamp) > maxAge;
        if (snapshot.stale && snapshot.quality == ObservationQuality::GOOD) {
            snapshot.quality = ObservationQuality::STALE;
        }
        observations.push_back(std::move(snapshot));
    }
    xSemaphoreGive(_sensorCacheMutex);
    return observations;
}

std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(
    const std::vector<const char*>& sourceRefs) const {
    std::vector<ObservationSnapshot> observations;
    try {
        observations.reserve(std::min(sourceRefs.size(), SENSOR_CACHE_MAX_ENTRIES));
        RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
        if (!configGuard.isLocked()) return observations;
        MutexGuard cacheGuard(_sensorCacheMutex, pdMS_TO_TICKS(20));
        if (!cacheGuard.isLocked()) return observations;
        const uint32_t now = static_cast<uint32_t>(millis());
        for (const char* source : sourceRefs) {
            if (!source || !source[0]) continue;
            bool duplicate = false;
            for (const auto& copied : observations) duplicate |= copied.sourceRef == source;
            if (duplicate) continue;
            const auto found = _sensorReadCache.find(String(source));
            if (found == _sensorReadCache.end() || (found->second.configVersion &&
                !FastBee::SensorConfigEpoch::matches(found->second.configVersion))) continue;
            if (observations.size() >= SENSOR_CACHE_MAX_ENTRIES) {
                observations.clear(); return observations;
            }
            const auto& cache = found->second;
            observations.emplace_back();
            auto& out = observations.back();
            out.sourceRef = source; out.label = cache.label; out.value = cache.value;
            out.unit = cache.unit; out.valueType = cache.valueType;
            out.sourceRevision = cache.sourceRevision; out.error = cache.error;
            out.dataField = cache.dataField; out.periphId = cache.periphId;
            if (out.sourceRef != source || out.label != cache.label || out.value != cache.value ||
                out.unit != cache.unit || out.valueType != cache.valueType ||
                out.sourceRevision != cache.sourceRevision || out.error != cache.error ||
                out.dataField != cache.dataField || out.periphId != cache.periphId) {
                observations.clear(); return observations;
            }
            out.quality = cache.quality; out.sampleTimestamp = cache.timestamp;
            out.maxAgeMs = cache.maxAgeMs; out.sampleIdentity = cache.sampleIdentity;
            out.recordable = cache.recordable; out.bindingValid = cache.bindingValid;
            out.hasValue = !cache.value.isEmpty();
            const uint32_t maxAge = cache.maxAgeMs ? cache.maxAgeMs : SENSOR_CACHE_TTL_MS;
            out.stale = cache.timestamp && static_cast<uint32_t>(now - cache.timestamp) > maxAge;
            if (out.stale && out.quality == ObservationQuality::GOOD) out.quality = ObservationQuality::STALE;
        }
    } catch (const std::bad_alloc&) {
        observations.clear();
    }
    return observations;
}

void PeriphExecManager::evictStaleSensorCache() {
    if (!_sensorCacheMutex) return;
    if (xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (_sensorReadCache.empty()) {
        xSemaphoreGive(_sensorCacheMutex);
        return;
    }

    unsigned long now = millis();
    // Epoch invalidation is immediate even when mutation-time cleanup was
    // contended. Physically reclaim those records during bounded maintenance.
    for (auto it = _sensorReadCache.begin(); it != _sensorReadCache.end();) {
        if (it->second.configVersion && !FastBee::SensorConfigEpoch::matches(it->second.configVersion)) {
            it = _sensorReadCache.erase(it);
        } else ++it;
    }
    for (auto& entry : _sensorReadCache) {
        const unsigned long maxAge = entry.second.maxAgeMs
            ? entry.second.maxAgeMs : SENSOR_CACHE_TTL_MS;
        if (entry.second.timestamp != 0 &&
            static_cast<unsigned long>(now - entry.second.timestamp) > maxAge &&
            entry.second.quality == ObservationQuality::GOOD) {
            entry.second.quality = ObservationQuality::STALE;
            entry.second.error = "STALE";
        }
    }
    xSemaphoreGive(_sensorCacheMutex);
}

void PeriphExecManager::notifySensorDataSSE(const String& key, const String& label, const String& value, const String& unit) {
    if (!_sensorSSECb) return;
    // 构建简单 JSON: {"key":"...","label":"...","value":"...","unit":"..."}
    String json = "{\"key\":\"" + key + "\",\"label\":\"" + label + "\",\"value\":\"" + value + "\",\"unit\":\"" + unit + "\"}";
    _sensorSSECb(json);
}

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
        obj["id"] = r.id;
        obj["name"] = r.name;
        obj["enabled"] = r.enabled;
        obj["execMode"] = r.execMode;

        // 触发器数组
        JsonArray trigArr = obj["triggers"].to<JsonArray>();
        for (const auto& t : r.triggers) {
            JsonObject tObj = trigArr.add<JsonObject>();
            tObj["triggerType"] = t.triggerType;
            tObj["triggerPeriphId"] = t.triggerPeriphId;
            tObj["operatorType"] = t.operatorType;
            tObj["compareValue"] = t.compareValue;
            tObj["timerMode"] = t.timerMode;
            tObj["intervalSec"] = t.intervalSec;
            tObj["timePoint"] = t.timePoint;
            tObj["eventId"] = t.eventId;
            // 轮询触发通信参数
            tObj["pollResponseTimeout"] = t.pollResponseTimeout;
            tObj["pollMaxRetries"] = t.pollMaxRetries;
            tObj["pollInterPollDelay"] = t.pollInterPollDelay;
        }

        // 动作数组
        JsonArray actArr = obj["actions"].to<JsonArray>();
        for (const auto& a : r.actions) {
            JsonObject aObj = actArr.add<JsonObject>();
            aObj["targetPeriphId"] = a.targetPeriphId;
            aObj["actionType"] = a.actionType;
            aObj["actionValue"] = a.actionValue;
            aObj["useReceivedValue"] = a.useReceivedValue;
            aObj["syncDelayMs"] = a.syncDelayMs;
            aObj["execMode"] = a.execMode;
        }

        // 数据转换管道
        obj["protocolType"] = r.protocolType;
        obj["scriptContent"] = r.scriptContent;

        // 数据上报控制
        obj["reportAfterExec"] = r.reportAfterExec;
        obj["allowPanelControl"] = r.allowPanelControl;
        obj["recordHistory"] = r.recordHistory;

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
        return visitConfigurationFile(path, nullptr, nullptr, nullptr, errorMsg);
    } catch (const std::bad_alloc&) {
        return configError(errorMsg, "CONFIG_MEMORY_PRESSURE");
    }
}

bool PeriphExecManager::importConfigurationFile(const char* stagedPath, String& errorMsg) {
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS));
    if (!lock.isLocked()) return configError(errorMsg, "RULE_SERVICE_BUSY");
    if (_configurationRestartPending) return configError(errorMsg, "CONFIG_PENDING_RESTART");
    if (!stagedPath || strcmp(stagedPath, PERIPH_EXEC_CONFIG_FILE) == 0)
        return configError(errorMsg, "CONFIG_STAGING_PATH");
    try {
        if (!validateConfigurationFile(stagedPath, errorMsg)) return false;
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
    if (!lock.isLocked() || _configurationRestartPending) return false;
    _configurationLoadFailed = true;
    String error;
    try {
        if (!LittleFS.exists(PERIPH_EXEC_CONFIG_FILE)) {
            const bool saved = writeConfigurationLocked(rules, _irKeysJson);
            _configurationLoadFailed = !saved;
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
                if (trigger.triggerPeriphId.startsWith("remote.")) *ctx.retired = true;
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

        rules.swap(candidate);
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
    if (!_rulesMutex) return "";
    MutexGuard lock(_rulesMutex);
    return _irKeysJson;
}

bool PeriphExecManager::setIrKeysJson(const String& jsonArray) {
    if (!_rulesMutex) return false;
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked() || configurationWriteError()) return false;
    try {
        String normalized;
        if (!jsonArray.isEmpty()) {
            if (jsonArray.length() > PERIPH_EXEC_CONFIG_MAX_BYTES) return false;
            ConfigJsonAllocator allocator;
            JsonDocument checkDoc(&allocator);
            if (deserializeJson(checkDoc, jsonArray) || !checkDoc.is<JsonArray>()) return false;
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

// ========== 委托给 Scheduler 的方法 ==========

void PeriphExecManager::handleMqttMessage(const String& topic, const String& message) {
    if (_scheduler) _scheduler->handleMqttMessage(topic, message);
}

void PeriphExecManager::handlePollData(const String& source, const String& data) {
    unsigned long now = millis();
    if (shouldThrottlePollIngress(source, now)) {
        return;
    }
    if (_scheduler) _scheduler->handlePollData(source, data);
}

String PeriphExecManager::handleDataCommand(const String& message) {
    if (_scheduler) return _scheduler->handleDataCommand(message);
    return "";
}

void PeriphExecManager::checkTimers() {
    if (_scheduler) _scheduler->checkTimers();
    // 重试待上报数据（MQTT 恢复后自动发送缓存的报告）
    retryPendingReports();
#if FASTBEE_ENABLE_POINT_HISTORY
    FastBee::PointHistory::instance().tick();
#endif
}

void PeriphExecManager::triggerEvent(EventType eventType, const String& eventData) {
    if (_scheduler) _scheduler->triggerEvent(eventType, eventData);
}

void PeriphExecManager::triggerEventById(const String& eventId, const String& eventData) {
    if (_scheduler) _scheduler->triggerEventById(eventId, eventData);
}

String PeriphExecManager::getStaticEventsJson() {
    return PeriphExecScheduler::getStaticEventsJson();
}

String PeriphExecManager::getDynamicEventsJson() {
    if (_scheduler) return _scheduler->getDynamicEventsJson();
    return "[]";
}

void PeriphExecManager::setModbusReadCallback(std::function<String(const String&)> callback) {
    _modbusReadCallback = callback;
    if (_executor) _executor->setModbusReadCallback(callback);
}

void PeriphExecManager::setModbusRawSendCallback(std::function<String(const String&)> callback) {
    _modbusRawSendCallback = callback;
}

void PeriphExecManager::setMqttIsConnectedCallback(MqttIsConnectedCallback cb) {
    _mqttIsConnectedCb = cb;
}

void PeriphExecManager::setMqttQueueReportCallback(MqttQueueReportCallback cb) {
    _mqttQueueReportCb = cb;
}

void PeriphExecManager::setModbusBuildSensorIdCallback(ModbusBuildSensorIdCallback cb) {
    _modbusBuildSensorIdCb = cb;
}

void PeriphExecManager::setModbusDirectControlCallback(ModbusDirectControlCallback cb) {
    _modbusDirectControlCb = cb;
}

void PeriphExecManager::setModbusDynamicEventsCallback(ModbusDynamicEventsCallback cb) {
    _modbusDynamicEventsCb = cb;
}

void PeriphExecManager::setMqttEventPublishCallback(MqttEventPublishCallback cb) {
    _mqttEventPublishCb = cb;
}

bool PeriphExecManager::notifyMqttEventPublish(const String& eventId, const String& eventName, const String& eventData) {
    if (!_mqttEventPublishCb) return false;
    if (eventId.isEmpty()) return false;
    return _mqttEventPublishCb(eventId, eventName, eventData);
}

void PeriphExecManager::checkButtonEvents() {
    if (_scheduler) _scheduler->checkButtonEvents();
}

// ========== 待上报数据缓存与重试 ==========

void PeriphExecManager::queuePendingReport(const String& reportData) {
    // 堆守卫：低堆时跳过缓存，避免 STL 分配导致 abort
    if (FastBee::MemoryBudget::dramFree() < 10000) return;

    if (_pendingReports.size() >= MAX_PENDING_REPORTS) {
        _pendingReports.erase(_pendingReports.begin());  // 移除最旧的
        LOGGER.warning("[PeriphExec] Pending report queue full, dropped oldest");
    }
    _pendingReports.push_back(reportData);
}

void PeriphExecManager::retryPendingReports() {
    if (_pendingReports.empty()) return;

    // 检查重试间隔
    unsigned long now = millis();
    if (_lastPendingReportRetry > 0 && (now - _lastPendingReportRetry) < PENDING_REPORT_RETRY_MS) return;

    // 通过回调检查 MQTT 连接状态
    if (!_mqttIsConnectedCb || !_mqttIsConnectedCb()) return;
    if (!_mqttQueueReportCb) return;

    _lastPendingReportRetry = now;

    // 取出所有待重试数据
    std::vector<String> toRetry;
    toRetry.swap(_pendingReports);

    for (const auto& report : toRetry) {
        bool ok = _mqttQueueReportCb(report);
        LOGGER.infof("[PeriphExec] Retry pending report: %s", ok ? "queued" : "failed");
    }
}

size_t PeriphExecManager::getPendingReportCount() const {
    return _pendingReports.size();
}

// ========== 启动后一次性物模型状态上报 ==========

void PeriphExecManager::scheduleBootReport() {
    // 仅置位标志；实际上报时机由 processBootReport() 在 MQTT 连接成功后把控
    _bootReportPending = true;
    _bootReportDone = false;
    _bootReportListBuilt = false;
    _bootReportQueue.clear();
}

void PeriphExecManager::processBootReport() {
    // 幂等门控：未调度或已完成→直接返回（整个生命周期只上报一次）
    if (!_bootReportPending || _bootReportDone) return;

    // 启动保护：未初始化（mutex 未创建）时跳过
    if (!isInitialized()) return;

    // 上报时机门控：需网络且 MQTT 已连接后才上报（未连接则等待，每周期重入重试）
    if (!_mqttIsConnectedCb || !_mqttIsConnectedCb()) return;

    if (!_executor) {
        _bootReportDone = true;
        _bootReportPending = false;
        return;
    }

    // 首次进入：在持锁下快照 “启用 && 勾选上报 && 含可上报动作” 的规则 ID
    if (!_bootReportListBuilt) {
        if (xSemaphoreTake(_rulesMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return;  // 拿不到锁：下周期重试
        }
        for (auto& kv : rules) {
            const PeriphExecRule& r = kv.second;
            if (!r.enabled || !r.reportAfterExec) continue;
            if (!ruleHasReportableStateAction(r)) continue;
            _bootReportQueue.push_back(r.id);
        }
        xSemaphoreGive(_rulesMutex);
        _bootReportListBuilt = true;
        LOGGER.infof("[PeriphExec] Boot report scheduled for %d rule(s)", (int)_bootReportQueue.size());
    }

    // 队列清空 → 完成
    if (_bootReportQueue.empty()) {
        _bootReportDone = true;
        _bootReportPending = false;
        LOGGER.info("[PeriphExec] Boot report completed");
        return;
    }

    // 每周期只处理一条规则（节流：避免一次性占用主循环与堆，也避免一次性洪水式上报）
    String ruleId = _bootReportQueue.back();
    _bootReportQueue.pop_back();

    PeriphExecRule ruleCopy;
    bool found = false;
    if (xSemaphoreTake(_rulesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = rules.find(ruleId);
        if (it != rules.end() && it->second.enabled && it->second.reportAfterExec) {
            ruleCopy = it->second;  // 锁内拷贝，锁外采集，避免长时间持锁
            found = true;
        }
        xSemaphoreGive(_rulesMutex);
    } else {
        _bootReportQueue.push_back(ruleId);  // 拿不到锁：放回队列下周期重试
        return;
    }

    if (found) {
        _executor->reportRuleCurrentState(ruleCopy);
    }
}

bool PeriphExecManager::tryReportDeviceData() {
    if (_scheduler) return _scheduler->tryReportDeviceData();
    return false;
}

String PeriphExecManager::getValidTriggerTypes() {
    return PeriphExecScheduler::getValidTriggerTypes();
}

String PeriphExecManager::getEventCategoriesJson() {
    return PeriphExecScheduler::getEventCategoriesJson();
}

String PeriphExecManager::getValidActionTypes(const String& periphId) {
    // 此静态方法需要访问 PeripheralManager，保留在 Manager 中
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig configSnapshot;
    const PeripheralConfig* config = nullptr;

    if (!periphId.isEmpty()) {
        if (pm.getPeripheralSnapshot(periphId, configSnapshot)) config = &configSnapshot;
    }

    auto addAction = [&arr](uint8_t type, const char* name, const char* category = "") {
        JsonObject obj = arr.add<JsonObject>();
        obj["type"] = type;
        obj["name"] = name;
        if (strlen(category) > 0) {
            obj["category"] = category;
        }
    };

    if (config) {
        PeripheralType pType = config->type;

        if (pType == PeripheralType::STEPPER_MOTOR) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "步进电机控制(forward/reverse/stop/faster/slower/setSpeed)", "步进电机");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::NEO_PIXEL) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "WS2812B控制(color/off/rainbow/brightness)", "灯珠");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::RF_MODULE) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "射频发送(send code/bits/pulseWidth/repeat)", "射频");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::RADAR_SENSOR) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ), "雷达读取(radar presence)", "雷达");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::UART) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "串口发送(send/text)", "串口");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (isInputType(pType)) {
            // 输入类型外设：不支持GPIO输出动作，只支持系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (isOutputType(pType)) {
            // 输出类型外设：支持所有GPIO输出动作
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH), "设置高电平", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW), "设置低电平", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED), "设置高电平(反转)", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED), "设置低电平(反转)", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_GPIO_TOGGLE), "电平翻转", "GPIO");

            // PWM输出
            if (config->isExpanderChannel()) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁(至少200ms)", "GPIO");
            }
            if (pType == PeripheralType::GPIO_PWM_OUTPUT) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_PWM), "设置PWM占空比", "PWM");
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁", "PWM");
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BREATHE), "呼吸灯", "PWM");
            }

            // DAC输出
            if (pType == PeripheralType::DAC || pType == PeripheralType::GPIO_ANALOG_OUTPUT) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_DAC), "设置DAC值", "DAC");
            }

            // 系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else {
            // 其他类型外设：只支持系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        }
    } else {
        // 未指定外设，返回所有动作类型
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH), "设置高电平", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW), "设置低电平", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_BREATHE), "呼吸灯", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_PWM), "设置PWM占空比", "PWM");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_DAC), "设置DAC值", "DAC");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
#if FASTBEE_ENABLE_OTA
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_OTA), "OTA升级", "系统");
#endif
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED), "设置高电平(反转)", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED), "设置低电平(反转)", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_GPIO_TOGGLE), "电平翻转", "GPIO");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
#if FASTBEE_ENABLE_REMOTE_RESOURCES
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ), "更新心知天气", "天气");
#endif
    }

    String result;
    serializeJson(doc, result);
    return result;
}

// ========== 异步执行结果管理 ==========

bool PeriphExecManager::ensureResultHistoryCapacity() {
    // The caller holds _resultsMutex. Existing records must remain updatable
    // under pressure; only first allocation needs this additional admission.
    if (executionResults.hasCapacity()) return true;
    constexpr size_t storageBytes =
        sizeof(AsyncExecResult) * ASYNC_EXEC_RESULT_HISTORY_LIMIT;
    // dramFree() includes internal regions which cannot back a byte-addressed
    // array on classic ESP32. Reserve the existing 12KB admission headroom in
    // the real 8-bit pool, conservatively assuming allocation uses DRAM even
    // when the platform allocator may select PSRAM.
    if (FastBee::MemoryBudget::dram8Free() < 12000U + storageBytes ||
        FastBee::MemoryBudget::dram8Largest() < storageBytes) return false;
    if (!executionResults.ensureCapacity()) return false;
    // Include allocator overhead and concurrent consumption in the check.
    // Roll back the empty array rather than admitting an untrackable operation.
    if (FastBee::MemoryBudget::dram8Free() < 12000U) {
        executionResults.releaseIfEmpty();
        return false;
    }
    return true;
}

bool PeriphExecManager::recordResult(const AsyncExecResult& result) {
    MutexGuard lock(_resultsMutex);
    if (!lock.isLocked() || result.operationId == 0) return false;

    for (auto& existing : executionResults) {
        if (existing.operationId == result.operationId) {
            // The record already owns its String capacity. All remaining trace
            // fields are fixed-size, so a terminal transition remains possible
            // even after DRAM falls below the admission threshold.
            existing = result;
            return true;
        }
    }

    // Only new operations allocate rule-id/name storage. Refuse their admission
    // under pressure instead of returning an operationId that can never appear.
    if (FastBee::MemoryBudget::dram8Free() < 12000) return false;
    if (!ensureResultHistoryCapacity()) return false;

    while (executionResults.size() >= ASYNC_EXEC_RESULT_HISTORY_LIMIT) {
        auto evict = executionResults.end();
        for (auto it = executionResults.begin(); it != executionResults.end(); ++it) {
            if (isTerminalExecStatus(it->status)) {
                evict = it;
                break;
            }
        }
        // At most MAX_ASYNC_TASKS operations can be active, so this means the
        // lifecycle invariant was violated. Do not hide an active operation.
        if (evict == executionResults.end()) return false;
        executionResults.erase(evict);
    }

    executionResults.push_back(result);
    return true;
}

bool PeriphExecManager::beginOperation(const PeriphExecRule& rule,
                                       bool asyncExecution,
                                       uint8_t queuePosition,
                                       AsyncExecResult& operation) {
    operation = AsyncExecResult();
    {
        MutexGuard lock(_resultsMutex);
        if (!lock.isLocked()) return false;
        // The retained history array does not cover new rule-id/name Strings.
        // Non-byte-addressable internal RAM cannot satisfy their headroom.
        if (FastBee::MemoryBudget::dram8Free() < 12000) return false;
        if (!ensureResultHistoryCapacity()) return false;

        while (executionResults.size() >= ASYNC_EXEC_RESULT_HISTORY_LIMIT) {
            auto evict = executionResults.end();
            for (auto it = executionResults.begin(); it != executionResults.end(); ++it) {
                if (isTerminalExecStatus(it->status)) {
                    evict = it;
                    break;
                }
            }
            if (evict == executionResults.end()) return false;
            executionResults.erase(evict);
        }

        do {
            ++_nextOperationId;
        } while (_nextOperationId == 0);

        const unsigned long now = millis();
        operation.operationId = _nextOperationId;
        operation.ruleId = rule.id;
        operation.ruleName = rule.name;
        operation.status = AsyncExecStatus::PENDING;
        operation.queuedAt = now;
        operation.deadlineAt = now + ASYNC_EXEC_OPERATION_TIMEOUT_MS;
        operation.queuePosition = queuePosition;
        operation.asyncExecution = asyncExecution;
        operation.actionCount = static_cast<uint8_t>(
            rule.actions.size() > 255 ? 255 : rule.actions.size());
        executionResults.push_back(operation);
    }
    return true;
}

std::vector<AsyncExecResult> PeriphExecManager::getRecentResults() {
    MutexGuard lock(_resultsMutex);
    if (!lock.isLocked()) return {};
    if (executionResults.size() == 0) return {};
    return {executionResults.begin(), executionResults.end()};
}

// ========== 手动执行 ==========

bool PeriphExecManager::runOnce(const String& id, const String& receivedValue,
                                String* rejectionReason,
                                AsyncExecResult* admittedOperation) {
    PeriphExecRule ruleCopy;
    if (!getRuleSnapshot(id, ruleCopy)) {
        LOGGER.warning("[PeriphExec] Rule not found for runOnce: " + id);
        if (rejectionReason) *rejectionReason = "rule_not_found";
        return false;
    }

    // 创建副本并异步分发，避免阻塞 Web 请求线程
    String resolvedValue = receivedValue;
    if (resolvedValue.isEmpty()) {
        for (const auto& trigger : ruleCopy.triggers) {
            if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER)) continue;
            if (!trigger.compareValue.isEmpty()) {
                resolvedValue = trigger.compareValue;
                break;
            }
        }
    }
    LOGGER.infof("[PeriphExec] runOnce async dispatch: '%s' (id=%s, value='%s')",
                 ruleCopy.name.c_str(), id.c_str(), resolvedValue.c_str());
    return dispatchAsync(ruleCopy, resolvedValue, rejectionReason, admittedOperation);
}

// ========== 异步调度 ==========

bool PeriphExecManager::shouldRunAsync() const {
    if (_workersSuspendedForMemory || _workerSuspensionPending ||
        !_workerPool || !_workerPool->isStarted()) {
        return false;
    }
    // 检查可用堆内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_ASYNC) {
        return false;
    }
    // 检查是否有空闲任务槽（非阻塞 peek）
    if (uxSemaphoreGetCount(_taskSlotSemaphore) == 0) {
        return false;
    }
    return true;
}

bool PeriphExecManager::dispatchAsync(const PeriphExecRule& rule,
                                      const String& receivedValue,
                                      String* rejectionReason,
                                      AsyncExecResult* admittedOperation) {
    if (rejectionReason) *rejectionReason = "";
    if (admittedOperation) *admittedOperation = AsyncExecResult();
    for (const auto& trigger : rule.triggers) if (trigger.triggerPeriphId.startsWith("remote.")) {
        if (rejectionReason) *rejectionReason = "remote_source_retired"; return false;
    }
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
            if (rejectionReason) *rejectionReason = "remote_channel_retired";
            return false;
        }
    }
    // Defense in depth for legacy/imported files which predate the save-time
    // validator. Destructive reset must never be reachable through automation.
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_SYS_FACTORY_RESET)) {
            LOGGER.warningf("[PeriphExec] Reject rule '%s': factory reset is not automatable",
                            rule.name.c_str());
            if (rejectionReason) *rejectionReason = "action_not_automatable";
            return false;
        }
    }
    for (size_t index = 0; index < rule.actions.size(); ++index) {
        if (isTerminalSystemAction(rule.actions[index].actionType) &&
            index + 1U != rule.actions.size()) {
            if (rejectionReason) *rejectionReason = "action_order_invalid";
            return false;
        }
    }

    // MemGuard SEVERE/CRITICAL pauses timer polling, but event sources (button,
    // MQTT and platform commands) can still arrive.  Do not let a heavy rule
    // fall back to synchronous execution during that window: script, sensor
    // and Modbus paths can hold the Web/network task for seconds and create a
    // second memory spike.  Lightweight GPIO/system actions remain available.
    {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor && monitor->isMemorySevere() && shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule during memory pressure: '%s'",
                            rule.name.c_str());
            MutexGuard runLock(_runningRulesMutex);
            _failureBackoff[rule.id] = millis() + 5000UL;
            if (rejectionReason) *rejectionReason = "memory_pressure";
            return false;
        }
    }

    // 原子检查并预留运行标记。系统动作与普通动作走同一准入路径，
    // 避免两个并发来源同时通过“先检查、后插入”的窗口。
    {
        MutexGuard runLock(_runningRulesMutex);
        if (!runLock.isLocked()) {
            if (rejectionReason) *rejectionReason = "rule_state_busy";
            return false;
        }
        if (_runningRuleIds.count(rule.id) > 0) {
            auto itTime = _runningStartTime.find(rule.id);
            unsigned long now = millis();
            bool startTimeMissing = (itTime == _runningStartTime.end());
            unsigned long elapsed = startTimeMissing ? 0 : (now - itTime->second);
            // 慢 Modbus 从站的合法批次可能超过 60 秒。仅凭墙钟时间删除
            // running 标记会让同一规则重入并并发操作同一硬件。worker 的
            // 正常/取消出口负责唯一清理；这里始终拒绝重复分发并保留诊断。
            LOGGER.warningf("[PeriphExec] Skip dispatch: task '%s' already running (%s, elapsed=%lums)",
                            rule.name.c_str(),
                            startTimeMissing ? "start-time-missing" : "active",
                            elapsed);
            if (rejectionReason) *rejectionReason = "already_running";
            return false;
        }
        _runningRuleIds.insert(rule.id);
        _runningStartTime[rule.id] = millis();
    }
    auto releaseReservation = [&](bool addBackoff) {
        MutexGuard runLock(_runningRulesMutex);
        if (!runLock.isLocked()) return;
        _runningRuleIds.erase(rule.id);
        _runningStartTime.erase(rule.id);
        if (addBackoff && FastBee::MemoryBudget::dramFree() > 10000) {
            _failureBackoff[rule.id] = millis() + 5000UL;
        }
    };
    auto runTrackedSync = [&]() -> bool {
        AsyncExecResult operation;
        if (!beginOperation(rule, false, 0, operation)) {
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "tracking_unavailable";
            return false;
        }
        // Admission and execution outcome are separate contracts. Once the
        // operation record exists, the request is accepted even if an action
        // later reaches a failed terminal state.
        executeSyncWithCompletion(rule, receivedValue, operation);
        if (admittedOperation) *admittedOperation = operation;
        return true;
    };

    // 异步模式：判断资源是否充足
    if (!shouldRunAsync()) {
        if (shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule sync fallback (heap=%d, slots=%d): '%s'",
                            (int)ESP.getFreeHeap(),
                            (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                            rule.name.c_str());
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "async_unavailable";
            return false;
        }
        LOGGER.infof("[PeriphExec] Fallback sync (heap=%d, slots=%d): '%s'",
                     (int)ESP.getFreeHeap(),
                     (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                     rule.name.c_str());
        return runTrackedSync();
    }

    // 获取一个任务槽（非阻塞）
    if (xSemaphoreTake(_taskSlotSemaphore, 0) != pdTRUE) {
        if (shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule without async slot: '%s'", rule.name.c_str());
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "no_task_slot";
            return false;
        }
        LOGGER.warningf("[PeriphExec] No async slot, fallback sync: '%s'", rule.name.c_str());
        return runTrackedSync();
    }

    // 从对象池获取异步上下文（避免频繁堆分配）
    AsyncExecContext* ctx = _contextPool.acquire();
    if (!ctx) {
        if (shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Context pool exhausted, skip heavy rule: '%s'", rule.name.c_str());
            xSemaphoreGive(_taskSlotSemaphore);
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "context_pool_exhausted";
            return false;
        }
        LOGGER.warning("[PeriphExec] Context pool exhausted, fallback to sync execution");
        xSemaphoreGive(_taskSlotSemaphore);
        return runTrackedSync();
    }

    ctx->ruleCopy = rule;
    ctx->receivedValue = receivedValue;
    ctx->manager = this;
    ctx->mqtt = nullptr;  // Will be set by executor if needed
    ctx->taskSlot = _taskSlotSemaphore;

    const size_t pendingBeforeEnqueue = _workerPool
        ? _workerPool->pendingCount() : 0;
    const uint8_t queuePosition = static_cast<uint8_t>(
        pendingBeforeEnqueue >= 254 ? 255 : pendingBeforeEnqueue + 1);
    AsyncExecResult operation;
    if (!beginOperation(rule, true, queuePosition, operation)) {
        _contextPool.release(ctx);
        xSemaphoreGive(_taskSlotSemaphore);
        releaseReservation(true);
        if (rejectionReason) *rejectionReason = "tracking_unavailable";
        return false;
    }
    ctx->operationId = operation.operationId;
    ctx->queuedAt = operation.queuedAt;
    ctx->deadlineAt = operation.deadlineAt;
    ctx->queuePosition = operation.queuePosition;

    // D 方案：投递到常驻 worker pool，避免运行期 xTaskCreate 碎片
    // worker pool 在启动期预分配 3×20KB 栈，运行期只需一次队列投递
    bool dispatched = false;
    if (_workerPool && _workerPool->isStarted()) {
        dispatched = _workerPool->enqueue(ctx);
    }

    if (!dispatched) {
        LOGGER.errorf("[PeriphExec] Failed to dispatch to worker pool: '%s' (pool=%s)",
                      rule.name.c_str(),
                      _workerPool ? "queue-full" : "unavailable");
        _contextPool.release(ctx);  // 归还对象池
        xSemaphoreGive(_taskSlotSemaphore);
        // 重资源规则（脚本/Modbus/多动作）绝对不能同步 fallback：
        // 9s+ 同步执行会阻塞 timer 任务，饯死 web_client_handler / network_update 等 100ms 调度，
        // 导致 Web 服务少响应。此处与前面三个失败分支保持一致：heavy rule 丢入 backoff 跳过本轮。
        if (shouldAvoidSyncFallback(rule)) {
            operation.status = AsyncExecStatus::REJECTED;
            operation.errorCode = AsyncExecError::WORKER_QUEUE_FULL;
            operation.endTime = millis();
            recordResult(operation);
            if (admittedOperation) *admittedOperation = operation;
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "worker_queue_full";
            return false;
        }
        operation.asyncExecution = false;
        operation.queuePosition = 0;
        recordResult(operation);
        executeSyncWithCompletion(rule, receivedValue, operation);
        if (admittedOperation) *admittedOperation = operation;
        return true;
    }

    if (admittedOperation) *admittedOperation = operation;

    LOG_DEBUGF("[PeriphExec] Async dispatched: '%s' (heap=%d, queue=%u/%u)",
                 rule.name.c_str(),
                 (int)ESP.getFreeHeap(),
                 (unsigned)(_workerPool ? _workerPool->pendingCount() : 0),
                 (unsigned)PeriphExecWorkerPool::capacity());
    return true;
}

// 单任务执行（由 PeriphExecWorkerPool 在 worker 线程内调用）
// 等价于原 asyncExecTaskFunc 的业务体，但不再调用 vTaskDelete
void PeriphExecManager::executeWorkerJob(AsyncExecContext* ctx) {
    AsyncExecResult result;
    result.operationId = ctx->operationId;
    result.ruleId = ctx->ruleCopy.id;
    result.ruleName = ctx->ruleCopy.name;
    result.queuedAt = ctx->queuedAt;
    result.startTime = millis();
    result.deadlineAt = ctx->deadlineAt;
    result.queuePosition = ctx->queuePosition;
    result.asyncExecution = true;
    result.actionCount = static_cast<uint8_t>(
        ctx->ruleCopy.actions.size() > 255 ? 255 : ctx->ruleCopy.actions.size());
    result.status = AsyncExecStatus::RUNNING;
    ctx->manager->recordResult(result);

    LOG_DEBUGF("[PeriphExec] Async task started: '%s'", result.ruleName.c_str());

    // 堆守卫：任务开始时检查堆内存，过低时直接放弃执行防止 abort()
    // 2026-08-05 fix: INTERNAL 口径含 IRAM/RTC 虚高 ~28KB，实测崩溃现场
    // INTERNAL 还有 29KB 时 8BIT DRAM 仅剩 <1KB → JSON 解析小分配直接
    // new OOM abort。双口径门控，8BIT 才是真实可分配水位。
    // 阈值校准：MQTT 连接稳态 8BIT 常态 ~7KB，门限取 5KB 保证用户规则
    // 在常态下可执行；窗口内挤出交给 per-action 守卫 + MQTT 强制断开。
    bool allOk = false;
    bool completedAll = false;
    size_t reportCount = 0;
    uint32_t const dram8Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (result.deadlineAt != 0 &&
        static_cast<int32_t>(result.startTime - result.deadlineAt) >= 0) {
        LOGGER.warningf("[PeriphExec] Queued operation expired before start: '%s'",
                        result.ruleName.c_str());
        result.status = AsyncExecStatus::TIMED_OUT;
        result.errorCode = AsyncExecError::DEADLINE_EXCEEDED;
    } else if (FastBee::MemoryBudget::dramFree() < 16384 || dram8Free < 5120) {
        LOGGER.warningf("[PeriphExec] Heap too low (dram=%d dram8=%u), skip execution: '%s'",
                        (int)ESP.getFreeHeap(), (unsigned)dram8Free, result.ruleName.c_str());
        result.status = AsyncExecStatus::FAILED;
        result.errorCode = AsyncExecError::LOW_MEMORY;
    } else {
        // 执行所有动作（reportCount 回填本次实际上报数量）
        auto actionResults = ctx->manager->executeAllActions(
            ctx->ruleCopy, ctx->receivedValue, false, &reportCount, &completedAll,
            result.deadlineAt, &result);

        // 统计结果
        allOk = completedAll && !ctx->ruleCopy.actions.empty();
        for (const auto& ar : actionResults) {
            if (!ar.success) { allOk = false; break; }
        }
        if (result.errorCode == AsyncExecError::DEADLINE_EXCEEDED) {
            result.status = AsyncExecStatus::TIMED_OUT;
        } else {
            result.status = allOk ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;
            if (!allOk && result.errorCode == AsyncExecError::NONE) {
                result.errorCode = completedAll
                    ? AsyncExecError::ACTION_FAILED
                    : AsyncExecError::INCOMPLETE_ACTIONS;
            }
        }
    }

    result.endTime = millis();
    result.reportCount = static_cast<uint8_t>(reportCount > 255 ? 255 : reportCount);

    LOGGER.infof("[PeriphExec] Async task finished: '%s' %s (%lums)",
                 result.ruleName.c_str(),
                 allOk ? "OK" : "FAILED",
                 result.endTime - result.startTime);

    // 记录执行结果
    ctx->manager->recordResult(result);

    // 从运行中集合移除规则ID
    {
        MutexGuard runLock(ctx->manager->_runningRulesMutex);
        ctx->manager->_runningRuleIds.erase(ctx->ruleCopy.id);
        ctx->manager->_runningStartTime.erase(ctx->ruleCopy.id);
    }

    // 如果执行失败，设置退避时间（30秒后才能再次触发）
    // 堆守卫：低堆时跳过 map 插入，防止 abort()
    if (FastBee::MemoryBudget::dramFree() > 10000) {
        if (!allOk) {
            MutexGuard runLock(ctx->manager->_runningRulesMutex);
            ctx->manager->_failureBackoff[ctx->ruleCopy.id] = millis() + 30000;
            LOGGER.infof("[PeriphExec] Rule '%s' failed, backoff for 30s", result.ruleName.c_str());
        } else {
            // 成功时清除退避记录
            MutexGuard runLock(ctx->manager->_runningRulesMutex);
            ctx->manager->_failureBackoff.erase(ctx->ruleCopy.id);
        }
    }

    // 触发外设执行完成事件（低堆时跳过，避免内部 STL 分配导致 abort）
    if (FastBee::MemoryBudget::dramFree() > 10000) {
        ctx->manager->dispatchPeriphExecEvent(ctx->ruleCopy.id, allOk ? "success" : "failed");
    }

    // 释放任务槽
    xSemaphoreGive(ctx->taskSlot);

    // 栈高水位标记（worker 任务自身，跨多个 job 的历史最低可用栈）
    UBaseType_t hwmBytes = uxTaskGetStackHighWaterMark(nullptr);
    LOGGER.infof("[PExec] Worker after '%s' stack HWM: %u bytes free",
                 result.ruleName.c_str(), (unsigned int)hwmBytes);

    // 归还上下文到对象池
    ctx->manager->_contextPool.release(ctx);
    // 注意：D 方案下 worker 是常驻任务，不再 vTaskDelete；
    // 控制权返回 PeriphExecWorkerPool::workerLoop 继续接下一个 job
}

// 执行规则的所有动作（供异步任务调用）
std::vector<ActionExecResult> PeriphExecManager::executeAllActions(
    const PeriphExecRule& rule, const String& receivedValue, bool suppressReport,
    size_t* reportCountOut, bool* completedAllOut, unsigned long deadlineAt,
    AsyncExecResult* traceOut) {
    if (_executor) {
        return _executor->executeAllActions(
            rule, receivedValue, suppressReport, reportCountOut, completedAllOut,
            deadlineAt, traceOut);
    }
    if (completedAllOut) *completedAllOut = false;
    return {};
}

// 同步执行并触发完成事件（用于同步降级路径，确保链式执行正常工作）
bool PeriphExecManager::executeSyncWithCompletion(const PeriphExecRule& rule,
                                                  const String& receivedValue,
                                                  AsyncExecResult& operation) {
    // dispatchAsync 已原子预留运行标记；同步降级只负责执行和统一收尾。
    operation.status = AsyncExecStatus::RUNNING;
    operation.startTime = millis();
    operation.queuePosition = 0;
    operation.asyncExecution = false;
    recordResult(operation);
    size_t reportCount = 0;
    bool completedAll = false;
    auto actionResults = executeAllActions(
        rule, receivedValue, false, &reportCount, &completedAll,
        operation.deadlineAt, &operation);

    // 3. 统计结果
    bool allOk = completedAll && !rule.actions.empty();
    for (const auto& ar : actionResults) {
        if (!ar.success) { allOk = false; break; }
    }

    LOGGER.infof("[PeriphExec] Sync+completion: '%s' %s", rule.name.c_str(), allOk ? "OK" : "FAILED");

    // 4. 从运行中集合移除
    {
        MutexGuard runLock(_runningRulesMutex);
        _runningRuleIds.erase(rule.id);
        _runningStartTime.erase(rule.id);
    }

    // 5. 设置或清除退避记录
    if (FastBee::MemoryBudget::dramFree() > 10000) {
        if (!allOk) {
            MutexGuard runLock(_runningRulesMutex);
            _failureBackoff[rule.id] = millis() + 30000;
        } else {
            MutexGuard runLock(_runningRulesMutex);
            _failureBackoff.erase(rule.id);
        }
    }

    // 6. 触发外设执行完成事件（链式执行关键）
    if (FastBee::MemoryBudget::dramFree() > 10000) {
        dispatchPeriphExecEvent(rule.id, allOk ? "success" : "failed");
    }

    // 7. 记录执行结果
    operation.endTime = millis();
    if (operation.errorCode == AsyncExecError::DEADLINE_EXCEEDED) {
        operation.status = AsyncExecStatus::TIMED_OUT;
    } else {
        operation.status = allOk ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;
        if (!allOk && operation.errorCode == AsyncExecError::NONE) {
            operation.errorCode = completedAll
                ? AsyncExecError::ACTION_FAILED
                : AsyncExecError::INCOMPLETE_ACTIONS;
        }
    }
    operation.reportCount = static_cast<uint8_t>(reportCount > 255 ? 255 : reportCount);
    recordResult(operation);
    return allOk;
}

// ========== 内部调度接口实现 ==========

void PeriphExecManager::checkTimerTriggers(unsigned long now, bool modbusAvailable) {
    // MemGuard CRITICAL 安全底线：Scheduler 已做动态降频，此处仅保留 CRITICAL 防护
    // 防止被其他路径直接调用时跳过内存保护
    {
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor && monitor->isMemoryCritical()) {
            return;  // CRITICAL: 完全跳过轮询
        }
        if (!monitor && FastBee::MemoryBudget::dramFree() < 15000) {
            // 守卫回退：HealthMonitor 不可用时的硬编码阈值
            return;
        }
    }

    // 周期性清理 _failureBackoff 过期 entry：避免长期运行后 map 累积死 entry 导致碎片
    // entry->second 是“下次可执行时间戳”，超过其 5 分钟仍无请求清除时，视为无效
    if (now - _lastBackoffCleanupTime > 60000UL) {
        _lastBackoffCleanupTime = now;
        MutexGuard runLock(_runningRulesMutex, pdMS_TO_TICKS(20));
        if (runLock.isLocked() && !_failureBackoff.empty()) {
            size_t erasedCount = 0;
            for (auto it = _failureBackoff.begin(); it != _failureBackoff.end(); ) {
                if (now > it->second && (now - it->second) > 300000UL) {
                    it = _failureBackoff.erase(it);
                    erasedCount++;
                } else {
                    ++it;
                }
            }
            if (erasedCount > 0) {
                LOGGER.infof("[PeriphExec] Backoff GC: erased %u expired entries, remaining=%u",
                             (unsigned)erasedCount, (unsigned)_failureBackoff.size());
            }
        }
    }

    // 周期性诊断日志：每 5 分钟输出一次运行状态，便于长期追踪退化趋势
    // 关键指标：running 任务数 / backoff entry 数 / context pool 占用 / heap 状况
    if (now - _lastDiagLogTime > 300000UL) {
        _lastDiagLogTime = now;
        size_t runningSize = 0;
        size_t backoffSize = 0;
        {
            MutexGuard runLock(_runningRulesMutex, pdMS_TO_TICKS(20));
            if (runLock.isLocked()) {
                runningSize = _runningRuleIds.size();
                backoffSize = _failureBackoff.size();
            }
        }
        LOGGER.infof("[PeriphExec] Diag: running=%u backoff=%u ctxPool=%u/%u heap=%u largest=%u",
                     (unsigned)runningSize,
                     (unsigned)backoffSize,
                     (unsigned)_contextPool.usedCount(),
                     (unsigned)AsyncExecContextPool::poolSize(),
                     (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap());
    }

    // 第一阶段：收集候选规则（短持锁，避免嵌套锁）
    struct CandidateRule {
        String id;
        String name;
        bool needsModbus;
        size_t triggerIdx;
    };
    std::vector<CandidateRule> candidates;
    candidates.reserve(4);

    {
        MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(100));
        if (!lock.isLocked()) return;  // 获取失败则跳过本轮

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            // 遍历触发器，寻找定时触发或轮询触发类型
            for (size_t ti = 0; ti < rule.triggers.size(); ti++) {
                auto& trigger = rule.triggers[ti];
                uint8_t tt = trigger.triggerType;
                if (tt != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) &&
                    tt != static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) continue;

                bool shouldTrigger = false;

                if (trigger.timerMode == 0) {
                    // 间隔模式
                    if (trigger.intervalSec == 0) continue;
                    unsigned long intervalMs = (unsigned long)trigger.intervalSec * 1000UL;
                    if (trigger.lastTriggerTime == 0 || (now - trigger.lastTriggerTime) >= intervalMs) {
                        shouldTrigger = true;
                    }
                } else if (trigger.timerMode == 1) {
                    // 每日时间点模式：使用缓存的解析值，避免锁内重复解析字符串
                    if (trigger._cachedHour < 0 || trigger._cachedMinute < 0) continue;

                    struct tm timeinfo;
                    if (!getLocalTime(&timeinfo, 0) || timeinfo.tm_year < 100) continue;

                    if (timeinfo.tm_hour == trigger._cachedHour && timeinfo.tm_min == trigger._cachedMinute) {
                        if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 60000) continue;
                        shouldTrigger = true;
                    }
                }

                if (shouldTrigger) {
                    CandidateRule c;
                    c.id = rule.id;
                    c.name = rule.name;
                    c.needsModbus = rule._cachedNeedsModbus;
                    c.triggerIdx = ti;
                    candidates.push_back(c);
                    break;  // 一个触发器匹配即可触发该规则
                }
            }
        }
    }
    // 锁已释放

    // 第二阶段：从上次检查的下一个规则开始轮转。规则存储是按 ID 排序
    // 的 map；若每轮都从头分发，前几个慢任务会持续占满有限 worker/context
    // 槽，后续规则永远只能收到队列拒绝。
    size_t roundRobinStart = 0;
    if (!_lastTimerDispatchRuleId.isEmpty()) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].id.compareTo(_lastTimerDispatchRuleId) > 0) {
                roundRobinStart = i;
                break;
            }
            if (i + 1 == candidates.size()) roundRobinStart = 0;
        }
    }

    // 锁外检查运行状态和退避并直接尝试接纳。只有接纳成功才推进
    // lastTriggerTime/triggerCount；资源拒绝的任务保持 due，下一轮可重试。
    for (size_t offset = 0; offset < candidates.size(); ++offset) {
        const CandidateRule& c =
            candidates[(roundRobinStart + offset) % candidates.size()];

        // 检查 Modbus 可用性
        if (c.needsModbus && !modbusAvailable) {
            LOGGER.warningf("[PeriphExec] Skip timer '%s': Modbus not available", c.name.c_str());
            continue;
        }

        // 检查失败退避和运行状态
        bool skip = false;
        {
            MutexGuard runLock(_runningRulesMutex, pdMS_TO_TICKS(10));
            if (runLock.isLocked()) {
                auto it = _failureBackoff.find(c.id);
                if (it != _failureBackoff.end() && now < it->second) {
                    skip = true;  // 仍在退避期内
                }
                if (!skip && _runningRuleIds.count(c.id) > 0) {
                    LOGGER.warningf("[PeriphExec] Skip timer '%s': task already running", c.name.c_str());
                    skip = true;
                }
            }
        }
        if (skip) continue;

        String rejectionReason;
        const bool accepted = dispatchByRuleId(c.id, "", &rejectionReason);
        if (!accepted) {
            LOGGER.warningf("[PeriphExec] Timer admission rejected: '%s' (%s)",
                            c.name.c_str(),
                            rejectionReason.isEmpty() ? "unavailable" : rejectionReason.c_str());
            continue;
        }

        // The cursor represents the last admitted rule, not the last rule we
        // merely inspected. Advancing it on queue/context rejection makes a
        // full scan end at the same highest map key every time, so the next
        // cycle starts from the first rule again and short-period rules can
        // starve later peers.
        _lastTimerDispatchRuleId = c.id;

        // 接纳成功后必须完成触发时间戳提交。运行标记已经预留，因此这里
        // 可以等待规则锁而不会重复分发；静默跳过会让任务完成后仍保持 due。
        {
            MutexGuard lock(_rulesMutex, portMAX_DELAY);
            if (lock.isLocked()) {
                auto it = rules.find(c.id);
                if (it != rules.end() && c.triggerIdx < it->second.triggers.size()) {
                    it->second.triggers[c.triggerIdx].lastTriggerTime = now;
                    it->second.triggers[c.triggerIdx].triggerCount++;
                }
            }
        }

        LOGGER.infof("[PeriphExec] Timer triggered: '%s'", c.name.c_str());
    }
}

bool PeriphExecManager::dispatchAndCommitTrigger(const String& ruleId,
                                                 size_t triggerIndex,
                                                 const String& receivedValue,
                                                 const char* sourceLabel) {
    String rejectionReason;
    if (!dispatchByRuleId(ruleId, receivedValue, &rejectionReason)) {
        LOGGER.warningf("[PeriphExec] %s admission rejected for rule '%s' (%s)",
                        sourceLabel ? sourceLabel : "trigger",
                        ruleId.c_str(),
                        rejectionReason.isEmpty() ? "unavailable" : rejectionReason.c_str());
        return false;
    }

    // Admission owns the execution slot before cooldown/counter state advances.
    // A rejected dispatch therefore remains eligible for the next source event.
    MutexGuard lock(_rulesMutex, portMAX_DELAY);
    if (!lock.isLocked()) {
        LOGGER.errorf("[PeriphExec] %s trigger commit lock failed for rule '%s'",
                      sourceLabel ? sourceLabel : "trigger", ruleId.c_str());
        return false;
    }
    auto it = rules.find(ruleId);
    if (it == rules.end() || triggerIndex >= it->second.triggers.size()) {
        LOGGER.warningf("[PeriphExec] %s trigger disappeared after admission for rule '%s'",
                        sourceLabel ? sourceLabel : "trigger", ruleId.c_str());
        return false;
    }

    ExecTrigger& trigger = it->second.triggers[triggerIndex];
    trigger.lastTriggerTime = millis();
    trigger.triggerCount++;
    return true;
}

void PeriphExecManager::dispatchEventMatchedRules(const String& eventId, const String& eventData) {
    int motorResultIndex = -1;
    if (eventId.startsWith("mc:")) {
        JsonDocument result;
        if (!deserializeJson(result, eventData) && result["origin"] == "motor-result") motorResultIndex = result["d"] | -1;
    }
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            // A motor acknowledgement must not start that motor again through
            // an event rule. Other targets can still react to the result.
            if (motorResultIndex >= 0) {
                bool feedback = false;
                for (const auto& action : rule.actions) {
                    if (action.targetPeriphId == "modbus:" + String(motorResultIndex)) feedback = true;
                    JsonDocument value;
                    if (!deserializeJson(value, action.actionValue)) {
                        for (JsonObjectConst control : value["ctrl"].as<JsonArrayConst>()) {
                            if ((control["d"] | -1) == motorResultIndex) feedback = true;
                        }
                    }
                }
                if (feedback) continue;
            }

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != eventId) continue;

                // Modbus 控制事件(mc:): 检查 compareValue 中的通道/动作是否匹配
                if (eventId.startsWith("mc:") && !trigger.compareValue.isEmpty()) {
                    bool ctrlMatched = false;
                    JsonDocument trigDoc;
                    if (!deserializeJson(trigDoc, trigger.compareValue) && trigDoc.is<JsonObject>()) {
                        JsonArray trigCtrlArr = trigDoc["ctrl"].as<JsonArray>();
                        if (trigCtrlArr && trigCtrlArr.size() > 0) {
                            JsonObject trigCtrl = trigCtrlArr[0].as<JsonObject>();
                            // 解析事件数据
                            JsonDocument evDoc;
                            if (!deserializeJson(evDoc, eventData) && evDoc.is<JsonObject>()) {
                                JsonObject evObj = evDoc.as<JsonObject>();
                                // 匹配通道和动作
                                // 支持两种事件数据格式：
                                //   标准格式: {"d":0,"c":1,"a":"on"}
                                //   旧格式:   {"d":0,"id":"relay_0_1","v":"1"}
                                uint8_t trigCh = trigCtrl["c"] | 255;  // 255 表示不限制
                                const char* trigAct = trigCtrl["a"] | "";
                                uint8_t evCh = 255;   // 未知状态
                                String evActStr;      // 动作字符串（延用生命用）
                                // 标准格式：直接取 c 和 a 字段
                                if (evObj.containsKey("c")) {
                                    evCh = evObj["c"];
                                }
                                if (evObj.containsKey("a")) {
                                    evActStr = evObj["a"].as<String>();
                                }
                                // 旧格式降级：从 id 字段解析通道号
                                if (evCh == 255 && evObj.containsKey("id")) {
                                    String evId = evObj["id"].as<String>();
                                    int lastUnderscore = evId.lastIndexOf('_');
                                    if (lastUnderscore > 0) {
                                        evCh = evId.substring(lastUnderscore + 1).toInt();
                                    }
                                }
                                // 旧格式降级：从 v 字段推导动作
                                if (evActStr.isEmpty() && evObj.containsKey("v")) {
                                    String evVal = evObj["v"].as<String>();
                                    evActStr = (evVal == "1" || evVal.equalsIgnoreCase("on") || evVal.equalsIgnoreCase("true")) ? "on" : "off";
                                }
                                if (evCh == 255) evCh = 0;  // 无法解析通道时默认为 0
                                const char* evAct = evActStr.c_str();
                                if ((trigCh == 255 || trigCh == evCh) &&
                                    (strlen(trigAct) == 0 || strcmp(trigAct, evAct) == 0)) {
                                    ctrlMatched = true;
                                }
                            }
                        }
                    } else {
                        // compareValue 无法解析，仅匹配 eventId
                        ctrlMatched = true;
                    }
                    if (!ctrlMatched) continue;
                }

                // 红外事件(ir_code_received): compareValue 非空时按编码或按键名过滤
                // 支持格式：
                //   "key:名称"           —— 按已学习按键名匹配（推荐，可跨规则复用）
                //   "NEC:0x00FF6897"     —— 限定协议 + 编码
                //   "0x00FF6897"/"00FF6897" —— 任意协议 + 编码
                if (eventId == "ir_code_received" && !trigger.compareValue.isEmpty()) {
                    bool codeMatched = false;
                    JsonDocument evDoc;
                    if (!deserializeJson(evDoc, eventData) && evDoc.is<JsonObject>()) {
                        String want = trigger.compareValue;
                        want.trim();
                        int keyColon = want.indexOf(':');
                        String wantPrefix = keyColon > 0 ? want.substring(0, keyColon) : String("");
                        if (wantPrefix.equalsIgnoreCase("key")) {
                            // 按键名匹配（大小写不敏感）
                            String wantName = want.substring(keyColon + 1);
                            wantName.trim();
                            String evKey = evDoc["key"] | "";
                            codeMatched = (wantName.length() > 0) && wantName.equalsIgnoreCase(evKey);
                        } else {
                            // 编码匹配（兼容旧规则）
                            String evCode = evDoc["code"] | "";        // 如 "0x00FF6897"
                            String evProto = evDoc["protocol"] | "";   // 如 "NEC"
                            if (keyColon > 0) {
                                if (!wantPrefix.equalsIgnoreCase(evProto)) want = "";  // 协议不符
                                else want = want.substring(keyColon + 1);
                            }
                            if (want.length() > 0) {
                                if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                                codeMatched = want.equalsIgnoreCase(evCode);
                            }
                        }
                    }
                    if (!codeMatched) continue;
                }

                // RF事件(rf_code_received): compareValue 非空时按编码或按键名过滤
                // 支持格式：
                //   "key:名称"           —— 按已学习按键名匹配
                //   "0xABCDEF"/"ABCDEF"  —— 按编码值匹配
                if (eventId == "rf_code_received" && !trigger.compareValue.isEmpty()) {
                    bool rfMatched = false;
                    JsonDocument rfDoc;
                    if (!deserializeJson(rfDoc, eventData) && rfDoc.is<JsonObject>()) {
                        String want = trigger.compareValue;
                        want.trim();
                        if (want.startsWith("key:") || want.startsWith("KEY:")) {
                            String wantName = want.substring(4);
                            wantName.trim();
                            String evKey = rfDoc["key"] | "";
                            rfMatched = (wantName.length() > 0) && wantName.equalsIgnoreCase(evKey);
                        } else {
                            String evCode = rfDoc["code"] | "";
                            if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                            rfMatched = want.equalsIgnoreCase(evCode);
                        }
                    }
                    if (!rfMatched) continue;
                }

                // 防重复触发：同一触发器最小间隔 1 秒
                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 1000) continue;

                LOGGER.infof("[PeriphExec] Event rule matched: '%s' (event=%s)",
                    rule.name.c_str(), eventId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex});
                break;  // 一个触发器匹配即可
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 eventData, "event");
    }
}

void PeriphExecManager::dispatchPeriphExecEvent(const String& ruleId, const String& eventData) {
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != ruleId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 500) continue;

                LOGGER.infof("[PeriphExec] Periph exec event rule matched: '%s' (triggerRule=%s)",
                    rule.name.c_str(), ruleId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex});
                break;
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 eventData, "periph-event");
    }
}

void PeriphExecManager::dispatchButtonEventRules(const String& eventId, const String& periphId) {
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(50));
        if (!lock.isLocked()) {
            LOGGER.debug("[PeriphExec] Button dispatch: mutex busy, will retry next cycle");
            return;  // 下次 checkButtonEvents 会再次触发
        }

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId != eventId) continue;
                // 若规则指定了触发外设，仅匹配该外设产生的按键事件；triggerPeriphId 为空则兼容任意按键
                if (!trigger.triggerPeriphId.isEmpty() && trigger.triggerPeriphId != periphId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 100) continue;

                LOGGER.infof("[PeriphExec] Button event rule matched: '%s' (event=%s, periph=%s)",
                    rule.name.c_str(), eventId.c_str(), periphId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex});
                break;
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 "", "button-event");
    }
}

void PeriphExecManager::processMqttMessageMatch(JsonArray& arr) {
    LOGGER.infof("[PeriphExec] Received MQTT message: %d items (non-DataCommand)", arr.size());
    // 阶段1: 持锁匹配，收集规则ID及其匹配的接收值
    struct MatchedRule {
        String ruleId;
        String receivedValue;
        size_t triggerIndex;
    };
    std::vector<MatchedRule> matchedRules;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        for (JsonObject item : arr) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                bool alreadyMatched = false;
                for (const auto& pending : matchedRules) {
                    if (pending.ruleId == rule.id) {
                        alreadyMatched = true;
                        break;
                    }
                }
                if (alreadyMatched) continue;

                // 遍历所有触发器（OR 关系）
                bool triggerMatched = false;
                size_t matchedTriggerIndex = 0;
                for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                    auto& trigger = rule.triggers[triggerIndex];
                    // 确定要匹配的数据源ID（支持平台触发和事件触发数据源）
                    String matchItemId;
                    bool isPlatformTrigger = (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER));
                    if (isPlatformTrigger) {
                        matchItemId = trigger.triggerPeriphId;
                    } else if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                               trigger.eventId.startsWith("ds:")) {
                        matchItemId = trigger.eventId.substring(3);
                    } else {
                        continue;
                    }

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (matchItemId.isEmpty() || matchItemId == "null") continue;
                    if (matchItemId != itemId) continue;

                    if (trigger.lastTriggerTime > 0 &&
                        (millis() - trigger.lastTriggerTime) < PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS) continue;

                    // 条件评估
                    bool conditionMet = false;
                    if (isPlatformTrigger && trigger.operatorType == 1) {
                        // 平台触发"设置模式"：仅需 ID 匹配即可执行，无需条件评估
                        conditionMet = true;
                    } else if (isPlatformTrigger && trigger.operatorType == 0) {
                        // 平台触发"匹配模式"：字符串精确匹配
                        conditionMet = (itemValue == trigger.compareValue);
                    } else {
                        // 事件触发数据源：使用通用条件评估（支持全部 10 种操作符）
                        conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);
                    }

                    if (conditionMet) {
                        LOGGER.infof("[PeriphExec] Rule '%s' triggered by %s=%s (op=%d, cmp=%s)",
                            rule.name.c_str(), itemId.c_str(), itemValue.c_str(),
                            trigger.operatorType, trigger.compareValue.c_str());

                        matchedTriggerIndex = triggerIndex;
                        triggerMatched = true;
                        break;  // 一个触发器匹配即可
                    }
                }

                if (triggerMatched) {
                    matchedRules.push_back({rule.id, itemValue, matchedTriggerIndex});
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : matchedRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 matched.receivedValue, "mqtt");
    }
}

void PeriphExecManager::processPollDataMatch(JsonArray& arr, const String& source) {
    // 阶段1: 持锁匹配，收集规则ID及其匹配的接收值
    struct MatchedRule {
        String ruleId;
        String receivedValue;
        size_t triggerIndex;
    };
    std::vector<MatchedRule> matchedRules;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        for (JsonObject item : arr) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                bool alreadyMatched = false;
                for (const auto& pending : matchedRules) {
                    if (pending.ruleId == rule.id) {
                        alreadyMatched = true;
                        break;
                    }
                }
                if (alreadyMatched) continue;
                bool hasPollCollectionAction = rule._cachedHasPollCollectionAction;

                // 遍历所有触发器（OR 关系）
                bool triggerMatched = false;
                size_t matchedTriggerIndex = 0;
                for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                    auto& trigger = rule.triggers[triggerIndex];
                    // 确定要匹配的数据源ID（支持轮询触发和事件触发数据源）
                    String matchItemId;
                    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
                        if (source == "modbus_poll" && hasPollCollectionAction) continue;
                        matchItemId = trigger.triggerPeriphId;
                    } else if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                               trigger.eventId.startsWith("ds:")) {
                        matchItemId = trigger.eventId.substring(3);
                    } else {
                        continue;
                    }

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (matchItemId.isEmpty() || matchItemId == "null") continue;
                    if (matchItemId != itemId) continue;

                    unsigned long minTriggerIntervalMs = getPollTriggerCooldownMs(rule, source);
                    if (trigger.lastTriggerTime > 0 &&
                        (millis() - trigger.lastTriggerTime) < minTriggerIntervalMs) continue;

                    // 条件评估 - 使用通用方法（自动检测数值/字符串类型）
                    bool conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);

                    if (conditionMet) {
                        LOGGER.infof("[PeriphExec] Poll rule '%s' triggered by %s: %s=%s (op=%d, cmp=%s)",
                            rule.name.c_str(), source.c_str(), itemId.c_str(), itemValue.c_str(),
                            trigger.operatorType, trigger.compareValue.c_str());

                        matchedTriggerIndex = triggerIndex;
                        triggerMatched = true;
                        break;  // 一个触发器匹配即可
                    }
                }

                if (triggerMatched) {
                    matchedRules.push_back({rule.id, itemValue, matchedTriggerIndex});
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : matchedRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 matched.receivedValue, "poll-data");
    }
}

String PeriphExecManager::processDataCommandMatch(JsonArray& cmdArr, const std::vector<int>& processedIndices) {
    // 打印完整 DATA_COMMAND 内容
    {
        static uint32_t _dcSeq = 0;
        uint32_t seq = ++_dcSeq;
        int n = 0;
        for (JsonObject item : cmdArr) {
            String iid = item["id"].as<String>();
            String ival = item["value"].as<String>();
            LOGGER.infof("[PeriphExec] Received command #%u [%d]: id='%s', value='%s'", seq, n, iid.c_str(), ival.c_str());
            n++;
        }
    }

    // 预处理阶段：处理 modbus_read 指令
    JsonDocument modbusReportDoc;
    JsonArray modbusReportArr = modbusReportDoc.to<JsonArray>();

    if (_modbusReadCallback) {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_read") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: modbus_read command detected");
                String modbusResult = _modbusReadCallback(itemValue);
                JsonDocument tmpDoc;
                if (!deserializeJson(tmpDoc, modbusResult) && tmpDoc.is<JsonArray>()) {
                    for (JsonVariant v : tmpDoc.as<JsonArray>()) {
                        modbusReportArr.add(v);
                    }
                }
            }
            idx++;
        }
    }

    // 预处理阶段：处理 modbus_raw_send 指令（透传模式：平台下发 HEX 帧）
    String transparentHexResult;
    if (_modbusRawSendCallback) {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_raw_send") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: modbus_raw_send command detected, hex='%s'", itemValue.c_str());
                String rawResult = _modbusRawSendCallback(itemValue);
                JsonDocument tmpDoc;
                if (!deserializeJson(tmpDoc, rawResult) && tmpDoc.is<JsonArray>()) {
                    for (JsonVariant v : tmpDoc.as<JsonArray>()) {
                        modbusReportArr.add(v);
                    }
                } else if (rawResult.length() > 0) {
                    // 透传模式：返回的是纯 HEX 字符串，非 JSON 数组
                    transparentHexResult = rawResult;
                }
            }
            idx++;
        }
    }

    // 阶段1: 持锁匹配，收集规则ID（避免深拷贝整个 PeriphExecRule）
    struct MatchedItem {
        String ruleId;
        String itemId;
        String itemValue;
    };
    std::vector<MatchedItem> matchedItems;
    std::vector<String> unmatchedIds;    // 未匹配的 item id
    std::vector<String> unmatchedValues; // 未匹配的 item value

    {
        MutexGuard lock(_rulesMutex);

        int itemIdx = 0;
        for (JsonObject item : cmdArr) {
            // 跳过已在预处理阶段处理的 modbus_read 项
            bool skip = false;
            for (int pi : processedIndices) { if (pi == itemIdx) { skip = true; break; } }
            itemIdx++;
            if (skip) continue;

            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();
            bool matched = false;

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                // 遍历触发器匹配数据源
                for (auto& trigger : rule.triggers) {
                    if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER)) continue;
                    if (trigger.triggerPeriphId.isEmpty() || trigger.triggerPeriphId == "null") continue;
                    if (trigger.triggerPeriphId != itemId) continue;

                    // 条件评估 - 区分匹配模式和设置模式
                    bool conditionMet = false;
                    if (trigger.operatorType == 1) {
                        // 设置模式：仅需 ID 匹配即可执行
                        conditionMet = true;
                    } else if (trigger.operatorType == 0) {
                        // 匹配模式：字符串精确匹配
                        conditionMet = (itemValue == trigger.compareValue);
                    } else {
                        // 其他操作符：使用通用条件评估（支持全部 10 种）
                        conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);
                    }

                    if (!conditionMet) continue;

                    matched = true;
                    trigger.lastTriggerTime = millis();
                    trigger.triggerCount++;
                    matchedItems.push_back({rule.id, itemId, itemValue});
                    break;  // 一个触发器匹配即可
                }
            }

            if (!matched) {
                unmatchedIds.push_back(itemId);
                unmatchedValues.push_back(itemValue);
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁同步执行并构建响应
    JsonDocument reportDoc;
    JsonArray reportArr = reportDoc.to<JsonArray>();

    // 先添加 Modbus 读取结果
    for (JsonVariant v : modbusReportArr) {
        reportArr.add(v);
    }

    // 获取 Modbus sensorId 构建回调（用于动作结果中的 sensorId 映射）

    for (auto& mi : matchedItems) {
        // 锁外逐个获取规则副本并同步执行（每次只持有一份拷贝）
        PeriphExecRule ruleCopy;
        {
            MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(50));
            if (!lock.isLocked()) continue;
            auto it = rules.find(mi.ruleId);
            if (it == rules.end()) continue;
            ruleCopy = it->second;
        }

        LOGGER.infof("[PeriphExec] Matched rule '%s': trigger id='%s', value='%s', actions=%d",
            ruleCopy.name.c_str(), mi.itemId.c_str(), mi.itemValue.c_str(), ruleCopy.actions.size());

        // 使用 suppressReport 抑制 executeAllActions 内部的 reportActionResults 上报，
        // 由 processDataCommandMatch 统一构建响应并返回给调用方上报，避免双重上报
        auto results = executeAllActions(ruleCopy, mi.itemValue, true);

        // 构建响应：每个动作的结果（Modbus 目标映射 sensorId）
        for (const auto& ar : results) {
            String reportId = ar.targetPeriphId.isEmpty() ? mi.itemId : ar.targetPeriphId;
            String reportValue = ar.actualValue.isEmpty() ? mi.itemValue : ar.actualValue;

            // 上报 ID 通用规则：默认用外设配置 id（与平台物模型标识符对应）；
            // 仅当动作配置显式指定 dataField（多字段外设如 DHT11 温/湿）时才覆盖，
            // 类别推断出的默认字段不作为上报 ID，避免与物模型标识符不匹配
            if (!ar.dataField.isEmpty()) {
                reportId = ar.dataField;
            }

            // Modbus 目标 sensorId 映射（通过回调）
            if (_modbusBuildSensorIdCb && ar.targetPeriphId.startsWith("modbus:")) {
                PeripheralManager& periMgr = PeripheralManager::getInstance();
                PeripheralConfig pcfg;
                if (periMgr.getPeripheralSnapshot(ar.targetPeriphId, pcfg) &&
                    pcfg.isModbusPeripheral() && pcfg.params.modbus.sensorId[0] != '\0') {
                    uint16_t channel = 0;
                    int sepIdx = ar.actualValue.indexOf(':');
                    if (sepIdx > 0) {
                        channel = ar.actualValue.substring(0, sepIdx).toInt();
                        reportValue = ar.actualValue.substring(sepIdx + 1);
                    }
                    String sid = _modbusBuildSensorIdCb(pcfg.params.modbus.deviceIndex, channel);
                    if (!sid.isEmpty()) reportId = sid;
                }
            }

            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = reportId;
            reportItem["value"] = reportValue;
            reportItem["remark"] = ar.success ? "success" : "execute failed";
        }
        if (results.empty()) {
            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = mi.itemId;
            reportItem["value"] = mi.itemValue;
            reportItem["remark"] = "no actions";
        }
    }

    // 未匹配项：尝试通过 Modbus 直接控制回调处理，或回退到直接外设控制
    PeripheralManager& periMgr = PeripheralManager::getInstance();
    for (size_t i = 0; i < unmatchedIds.size(); i++) {
        // 优先尝试 Modbus 直接控制回调（协议层注入）
        if (_modbusDirectControlCb && _modbusDirectControlCb(unmatchedIds[i], unmatchedValues[i], reportArr)) {
            continue;  // Modbus 回调已处理
        }

        // 回退：尝试直接外设控制
        PeripheralConfig directSnapshot;
        PeripheralConfig* directCfg = periMgr.getPeripheralSnapshot(unmatchedIds[i], directSnapshot)
            ? &directSnapshot : nullptr;
        if (!directCfg) {
            // 既无规则匹配、也不存在对应外设（如平台属性下发附带的
            // temperature/humidity 等只读项）：跳过，不回显到 property/post。
            // 原样回显会被平台当成设备新上报的传感器数据（假数据污染），
            // 频繁下发时还会带来无意义的 JSON 构建+MQTT 发布内存开销。
            LOGGER.warningf("[PeriphExec] DataCommand item '%s' has no rule/peripheral, skip echo",
                            unmatchedIds[i].c_str());
            continue;
        }
        String actualValue = unmatchedValues[i];
        bool directOk = executeDirectOutputCommand(periMgr, *directCfg, unmatchedValues[i], actualValue);

        JsonObject reportItem = reportArr.add<JsonObject>();
        reportItem["id"] = unmatchedIds[i];
        reportItem["value"] = actualValue;
        reportItem["remark"] = directOk ? "direct_peripheral" : "no matching rule";
    }

    // 透传模式：若返回的是纯 HEX 字符串且无其他 JSON 内容，直接返回 HEX
    if (transparentHexResult.length() > 0 && reportArr.size() == 0) {
        return transparentHexResult;
    }

    String result;
    serializeJson(reportDoc, result);
    return result;
}

// 按规则ID安全分发（锁内拷贝规则，锁外调用dispatchAsync，避免死锁）
bool PeriphExecManager::dispatchByRuleId(const String& ruleId,
                                         const String& receivedValue,
                                         String* rejectionReason) {
    if (rejectionReason) *rejectionReason = "";
    PeriphExecRule ruleCopy;
    bool ruleAcquired = false;
    bool ruleMissing = false;
    // 最多尝试 3 次（每次 50ms，总超时约 150ms）避免按键等一次性事件因瞬时锁竞争
    // （例如 checkTimerTriggers 正持 rulesMutex 100ms）而被直接丢弃
    for (int attempt = 0; attempt < 3 && !ruleAcquired && !ruleMissing; ++attempt) {
        MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(50));
        if (lock.isLocked()) {
            auto it = rules.find(ruleId);
            if (it == rules.end() || !it->second.enabled) {
                ruleMissing = true;  // 规则不存在或被禁用，不再重试
                break;
            }
            ruleCopy = it->second;  // 锁内做一次深拷贝
            ruleAcquired = true;
        }
    }
    if (ruleMissing) {
        if (rejectionReason) *rejectionReason = "rule_not_found_or_disabled";
        return false;
    }
    if (!ruleAcquired) {
        LOGGER.warningf("[PeriphExec] dispatchByRuleId: rulesMutex busy, drop event for rule '%s'",
                        ruleId.c_str());
        if (rejectionReason) *rejectionReason = "rule_service_busy";
        return false;
    }
    // 锁已释放，安全调用 dispatchAsync
    return dispatchAsync(ruleCopy, receivedValue, rejectionReason);
}

String PeriphExecManager::getDynamicEventsJsonInternal() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    // 添加外设执行规则作为动态事件
    MutexGuard lock(_rulesMutex);

    for (const auto& pair : rules) {
        const PeriphExecRule& rule = pair.second;
        if (!rule.enabled) continue;

        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = rule.id;
        obj["name"] = rule.name;
        obj["category"] = "外设执行";
        obj["type"] = static_cast<uint8_t>(EventType::EVENT_PERIPH_EXEC_COMPLETED);
        obj["isDynamic"] = true;
    }

    // 通过回调添加 Modbus 控制类子设备作为动态事件（mc:deviceIndex 格式）
    if (_modbusDynamicEventsCb) {
        _modbusDynamicEventsCb(arr);
    }

    String result;
    serializeJson(doc, result);
    return result;
}

// ========== 通用条件评估 ==========

bool PeriphExecManager::evaluateCondition(const String& value, uint8_t op, const String& compareValue) {
    ExecOperator oper = static_cast<ExecOperator>(op);

    // 字符串操作符（不需要数值转换）
    if (oper == ExecOperator::CONTAIN) {
        return value.indexOf(compareValue) >= 0;
    }
    if (oper == ExecOperator::NOT_CONTAIN) {
        return value.indexOf(compareValue) < 0;
    }

    // EQ / NEQ：自动检测数值/字符串类型
    if (oper == ExecOperator::EQ || oper == ExecOperator::NEQ) {
        bool bothNumeric = isNumericString(value) && isNumericString(compareValue);
        if (bothNumeric) {
            float val = value.toFloat();
            float cmp = compareValue.toFloat();
            return (oper == ExecOperator::EQ) ? (val == cmp) : (val != cmp);
        }
        // 非数值：字符串精确比较
        return (oper == ExecOperator::EQ) ? (value == compareValue) : (value != compareValue);
    }

    // 数值操作符（GT/LT/GTE/LTE/BETWEEN/NOT_BETWEEN）
    float val = value.toFloat();

    switch (oper) {
        case ExecOperator::GT:  return val > compareValue.toFloat();
        case ExecOperator::LT:  return val < compareValue.toFloat();
        case ExecOperator::GTE: return val >= compareValue.toFloat();
        case ExecOperator::LTE: return val <= compareValue.toFloat();
        case ExecOperator::BETWEEN:
        case ExecOperator::NOT_BETWEEN: {
            int commaIdx = compareValue.indexOf(',');
            if (commaIdx < 0) return false;
            float minVal = compareValue.substring(0, commaIdx).toFloat();
            float maxVal = compareValue.substring(commaIdx + 1).toFloat();
            bool inRange = (val >= minVal && val <= maxVal);
            return (oper == ExecOperator::BETWEEN) ? inRange : !inRange;
        }
        default: return false;
    }
}

// ========== 工具 ==========

String PeriphExecManager::generateUniqueId() {
    return "exec_" + String(millis());
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
