#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SCREEN_PANEL

#include "core/ScreenPanelManager.h"
#include "core/ScreenPanelMigration.h"

#include <LittleFS.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <time.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/task.h>
#endif
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "core/FastBeeFramework.h"
#include "network/NetworkManager.h"
#if FASTBEE_ENABLE_OTA
#include "network/OTAManager.h"
#endif

#include "core/AsyncExecTypes.h"
#include "core/MotorControlService.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "core/PointHistory.h"
#include "systems/LoggerSystem.h"
#include "systems/SystemRebooter.h"
#include "screen/ScreenRuntime.h"
#include "screen/ScreenBacklight.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif

namespace {

std::vector<ObservationSnapshot> panelObservationSnapshots(JsonArrayConst pages,
    uint8_t activePage, bool activeOnly) {
    // Copy only sources used by this refresh, never the full cache. Source
    // pointers belong to the parsed layout for the duration of this call.
    std::vector<const char*> sources;
    try {
        auto add = [&](const char* source) {
            if (!source || !source[0] || !strncmp(source, "system.", 7) ||
                !strncmp(source, "remote.", 7) || !strncmp(source, "weather.", 8)) return;
            for (const char* existing : sources) if (!strcmp(existing, source)) return;
            sources.push_back(source);
        };
        uint8_t pageIndex = 0;
        size_t cardCount = 0;
        for (JsonObjectConst page : pages) {
            if (pageIndex >= ScreenPanelSnapshot::MAX_PAGES) break;
            for (JsonObjectConst card : page["cards"].as<JsonArrayConst>()) {
                if (cardCount++ >= ScreenPanelSnapshot::MAX_CARDS) break;
                if (activeOnly && pageIndex != activePage) continue;
                add(card["sourceRef"] | "");
                size_t seriesCount = 0;
                for (JsonVariantConst series : card["sourceRefs"].as<JsonArrayConst>()) {
                    if (seriesCount++ >= 4) break;
                    add(series.as<const char*>());
                }
            }
            ++pageIndex;
        }
        if (sources.empty()) return {};
        return PeriphExecManager::getInstance().getObservationSnapshots(sources);
    } catch (const std::bad_alloc&) {
        return {};
    }
}

class PanelLock {
public:
    explicit PanelLock(SemaphoreHandle_t mutex, TickType_t timeout = pdMS_TO_TICKS(100))
        : _mutex(mutex), _locked(mutex && xSemaphoreTake(mutex, timeout) == pdTRUE) {}
    ~PanelLock() { if (_locked) xSemaphoreGive(_mutex); }
    bool locked() const { return _locked; }

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};

template <size_t N>
void copyText(char (&destination)[N], const String& source) {
    snprintf(destination, N, "%s", source.c_str());
}

template <size_t N>
void copyText(std::array<char, N>& destination, const String& source) {
    snprintf(destination.data(), N, "%s", source.c_str());
}

template <typename Snapshot>
bool fillPanelSystemSource(const String& sourceRef, Snapshot& target) {
    if (!sourceRef.startsWith("system.")) return false;
    if (sourceRef == "system.uptime") {
        snprintf(target.value, sizeof(target.value), "%llu",
                 static_cast<unsigned long long>(esp_timer_get_time() / 1000000));
        if (!target.unit[0]) copyText(target.unit, "s");
    } else if (sourceRef == "system.freeHeap") {
        snprintf(target.value, sizeof(target.value), "%lu", static_cast<unsigned long>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (!target.unit[0]) copyText(target.unit, "B");
    } else if (sourceRef == "system.wifiRssi") {
        if (!target.unit[0]) copyText(target.unit, "dBm");
        if (WiFi.status() != WL_CONNECTED) {
            target.value[0] = '\0';
            target.quality = ScreenValueQuality::OFFLINE;
            target.updatedMs = millis();
            return true;
        }
        snprintf(target.value, sizeof(target.value), "%ld", static_cast<long>(WiFi.RSSI()));
    } else if (sourceRef == "system.network") {
        auto* framework = FastBeeFramework::getInstance();
        auto* manager = framework
            ? static_cast<FBNetworkManager*>(framework->getNetworkManager()) : nullptr;
        if (!manager) return false;
        const auto info = manager->getStatusInfo();
        const char* status = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED: status = "connected"; break;
            case NetworkStatus::DISCONNECTED: status = "disconnected"; break;
            case NetworkStatus::CONNECTING: status = "connecting"; break;
            case NetworkStatus::AP_MODE: status = "ap_mode"; break;
            case NetworkStatus::CONNECTION_FAILED: status = "failed"; break;
            default: break;
        }
        copyText(target.value, status);
    } else {
        return false;
    }
    target.quality = ScreenValueQuality::GOOD;
    target.updatedMs = millis();
    return true;
}

const char* panelAsyncExecErrorName(AsyncExecError error) {
    switch (error) {
        case AsyncExecError::NONE: return "none";
        case AsyncExecError::LOW_MEMORY: return "low_memory";
        case AsyncExecError::DEADLINE_EXCEEDED: return "deadline_exceeded";
        case AsyncExecError::ACTION_FAILED: return "action_failed";
        case AsyncExecError::INCOMPLETE_ACTIONS: return "incomplete_actions";
        case AsyncExecError::CANCELLED_BY_SYSTEM: return "cancelled_by_system";
        case AsyncExecError::WORKER_QUEUE_FULL: return "worker_queue_full";
    }
    return "unknown";
}

bool panelAsyncExecStatusIsTerminal(AsyncExecStatus status) {
    return status == AsyncExecStatus::COMPLETED ||
           status == AsyncExecStatus::FAILED ||
           status == AsyncExecStatus::CANCELLED ||
           status == AsyncExecStatus::REJECTED ||
           status == AsyncExecStatus::TIMED_OUT;
}

template <size_t N>
bool onlyKeys(JsonObjectConst object, const char* const (&allowed)[N],
              const String& path, String& error) {
    for (JsonPairConst pair : object) {
        bool found = false;
        for (const char* key : allowed) {
            if (strcmp(pair.key().c_str(), key) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            error = path + ": unknown field '" + pair.key().c_str() + "'";
            return false;
        }
    }
    return true;
}

bool validIdentifier(const String& value, size_t maxLength = 40) {
    if (value.isEmpty() || value.length() > maxLength) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value.charAt(i);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

bool validReference(const String& value) {
    if (value.isEmpty() || value.length() > 96) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value.charAt(i);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == ':')) return false;
    }
    return true;
}

bool boundedString(JsonVariantConst value, size_t maxLength, bool required,
                   const String& path, String& error) {
    if (value.isNull()) {
        if (!required) return true;
        error = path + " is required";
        return false;
    }
    if (!value.is<const char*>()) {
        error = path + " must be a string";
        return false;
    }
    const char* text = value.as<const char*>();
    if ((required && !text[0]) || strlen(text) > maxLength) {
        error = path + " has an invalid length";
        return false;
    }
    return true;
}

bool oneOf(const String& value, std::initializer_list<const char*> values) {
    for (const char* candidate : values) if (value == candidate) return true;
    return false;
}

bool readBoundedInt(JsonVariantConst value, int minimum, int maximum,
                    int fallback, int& out, const String& path, String& error) {
    if (value.isNull()) {
        out = fallback;
        return true;
    }
    if (!value.is<int>()) {
        error = path + " must be an integer";
        return false;
    }
    out = value.as<int>();
    if (out < minimum || out > maximum) {
        error = path + " is out of range";
        return false;
    }
    return true;
}

bool rectsOverlap(const ScreenPanelRect& a, const ScreenPanelRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

bool parseKind(const String& value, ScreenCardKind& out) {
    if (value == "data") out = ScreenCardKind::DATA;
    else if (value == "chart") out = ScreenCardKind::CHART;
    else if (value == "control") out = ScreenCardKind::CONTROL;
    else if (value == "system") out = ScreenCardKind::SYSTEM;
    else if (value == "clock") out = ScreenCardKind::CLOCK;
    else if (value == "weather") out = ScreenCardKind::WEATHER;
    else if (value == "page-link") out = ScreenCardKind::PAGE_LINK;
    else return false;
    return true;
}

bool parseVisual(const String& value, ScreenVisualType& out) {
    if (value == "value") out = ScreenVisualType::VALUE;
    else if (value == "status") out = ScreenVisualType::STATUS;
    else if (value == "gauge") out = ScreenVisualType::GAUGE;
    else if (value == "bar") out = ScreenVisualType::BAR;
    else if (value == "line") out = ScreenVisualType::LINE;
    else if (value == "weather") out = ScreenVisualType::WEATHER;
    else if (value == "clock") out = ScreenVisualType::CLOCK;
    else if (value == "navigation") out = ScreenVisualType::NAVIGATION;
    else if (value == "switch") out = ScreenVisualType::SWITCH;
    else if (value == "action") out = ScreenVisualType::ACTION;
    else if (value == "number") out = ScreenVisualType::NUMBER_CONTROL;
    else if (value == "motor") out = ScreenVisualType::MOTOR_CONTROL;
    else if (value == "slider") out = ScreenVisualType::SLIDER;
    else if (value == "select") out = ScreenVisualType::SELECT;
    else return false;
    return true;
}

bool visualMatches(ScreenCardKind kind, ScreenVisualType visual) {
    switch (kind) {
        case ScreenCardKind::DATA:
            return visual == ScreenVisualType::VALUE || visual == ScreenVisualType::STATUS ||
                   visual == ScreenVisualType::GAUGE || visual == ScreenVisualType::BAR;
        case ScreenCardKind::CHART:
            return visual == ScreenVisualType::LINE || visual == ScreenVisualType::BAR;
        case ScreenCardKind::CONTROL:
            return visual == ScreenVisualType::SWITCH || visual == ScreenVisualType::ACTION ||
                   visual == ScreenVisualType::NUMBER_CONTROL || visual == ScreenVisualType::MOTOR_CONTROL;
        case ScreenCardKind::SYSTEM:
            return visual == ScreenVisualType::ACTION || visual == ScreenVisualType::STATUS ||
                   visual == ScreenVisualType::VALUE || visual == ScreenVisualType::SLIDER ||
                   visual == ScreenVisualType::SELECT || visual == ScreenVisualType::SWITCH;
        case ScreenCardKind::CLOCK:
            return visual == ScreenVisualType::CLOCK;
        case ScreenCardKind::WEATHER:
            return visual == ScreenVisualType::WEATHER;
        case ScreenCardKind::PAGE_LINK:
            return visual == ScreenVisualType::NAVIGATION;
    }
    return false;
}

const char* qualityName(ScreenValueQuality quality) {
    switch (quality) {
        case ScreenValueQuality::GOOD: return "good";
        case ScreenValueQuality::STALE: return "stale";
        case ScreenValueQuality::OFFLINE: return "offline";
        case ScreenValueQuality::ERROR: return "error";
        default: return "unknown";
    }
}

ScreenValueQuality qualityFromObservation(ObservationQuality quality) {
    switch (quality) {
        case ObservationQuality::GOOD: return ScreenValueQuality::GOOD;
        case ObservationQuality::STALE: return ScreenValueQuality::STALE;
        case ObservationQuality::OFFLINE: return ScreenValueQuality::OFFLINE;
        case ObservationQuality::ERROR: return ScreenValueQuality::ERROR;
        default: return ScreenValueQuality::UNKNOWN;
    }
}

ScreenValueQuality qualityFromText(const String& quality) {
    if (quality == "good") return ScreenValueQuality::GOOD;
    if (quality == "stale") return ScreenValueQuality::STALE;
    if (quality == "offline") return ScreenValueQuality::OFFLINE;
    if (quality == "error") return ScreenValueQuality::ERROR;
    return ScreenValueQuality::UNKNOWN;
}

String jsonScalar(JsonVariantConst value) {
    if (value.isNull()) return "";
    if (value.is<const char*>()) return value.as<const char*>();
    if (value.is<bool>()) return value.as<bool>() ? "true" : "false";
    String result;
    serializeJson(value, result);
    return result;
}

bool panelSafeRule(const PeriphExecRule& rule) {
    if (!rule.allowPanelControl || rule.actions.empty()) return false;
    for (const auto& action : rule.actions) {
        switch (static_cast<ExecActionType>(action.actionType)) {
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
                break;
            default:
                return false;
        }
    }
    return true;
}

enum class PanelRuleInputKind : uint8_t { TRIGGER, BOOLEAN, NUMBER, UNSUPPORTED };

struct PanelNumericConstraints {
    double minimum = -std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::max();
    bool integerOnly = false;
    bool bounded = false;
};

bool canonicalPanelNumber(const String& text, double& value,
                          uint8_t* fractionalDigits = nullptr) {
    if (text.isEmpty() || text.length() > 24 || text.charAt(0) == '+') return false;
    size_t index = text.charAt(0) == '-' ? 1U : 0U;
    if (index >= text.length()) return false;
    const size_t integerStart = index;
    while (index < text.length() && isdigit(static_cast<unsigned char>(text.charAt(index)))) {
        ++index;
    }
    if (index == integerStart) return false;
    if (index - integerStart > 1U && text.charAt(integerStart) == '0') return false;
    uint8_t decimals = 0;
    if (index < text.length() && text.charAt(index) == '.') {
        ++index;
        const size_t fractionStart = index;
        while (index < text.length() && isdigit(static_cast<unsigned char>(text.charAt(index)))) {
            ++index;
        }
        if (index == fractionStart || index - fractionStart > 6U) return false;
        decimals = static_cast<uint8_t>(index - fractionStart);
    }
    if (index != text.length()) return false;
    char* end = nullptr;
    value = strtod(text.c_str(), &end);
    if (end == text.c_str() || *end || !std::isfinite(value)) return false;
    if (fractionalDigits) *fractionalDigits = decimals;
    return true;
}

bool readFinitePanelNumber(JsonVariantConst value, double& out,
                           const String& path, String& error) {
    if (!value.is<double>()) {
        error = path + " must be numeric";
        return false;
    }
    out = value.as<double>();
    String encoded;
    serializeJson(value, encoded);
    if (!std::isfinite(out) || encoded.length() > 24) {
        error = path + " is not a bounded finite number";
        return false;
    }
    return true;
}

bool panelNumberAligned(double value, double origin, double step) {
    if (!std::isfinite(value) || !std::isfinite(origin) ||
        !std::isfinite(step) || step <= 0.0) return false;
    const double positions = (value - origin) / step;
    if (!std::isfinite(positions)) return false;
    const double nearest = std::round(positions);
    const double tolerance = 1e-7 * std::max(1.0, std::fabs(positions));
    return std::fabs(positions - nearest) <= tolerance;
}

bool panelNumberFitsPrecision(double value, uint8_t precision) {
    const double scale = std::pow(10.0, precision);
    if (!std::isfinite(value) || !std::isfinite(scale) ||
        !std::isfinite(value * scale)) return false;
    const double scaled = value * scale;
    return std::fabs(scaled - std::round(scaled)) <=
           1e-7 * std::max(1.0, std::fabs(scaled));
}

bool fixedModbusAddressTemplate(const String& value) {
    const int separator = value.indexOf(':');
    if (separator <= 0 || value.substring(separator + 1) != "$value") return false;
    const String address = value.substring(0, separator);
    if (address.length() > 5) return false;
    for (size_t i = 0; i < address.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(address.charAt(i)))) return false;
    }
    return address.toInt() >= 0 && address.toInt() <= 65535;
}

bool numericActionConstraints(const ExecAction& action,
                              JsonArrayConst remoteControls,
                              PanelNumericConstraints& constraints) {
    const auto type = static_cast<ExecActionType>(action.actionType);
    const bool directTemplate = action.actionValue.isEmpty() ||
                                action.actionValue == "$value";
    double minimum = -std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::max();
    bool integerOnly = false;
    bool bounded = true;
    switch (type) {
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
            if (!directTemplate) return false;
            minimum = 50.0;
            maximum = 60000.0;
            integerOnly = true;
            break;
        case ExecActionType::ACTION_SET_PWM:
            if (!directTemplate) return false;
            minimum = 0.0;
            maximum = 65535.0;
            integerOnly = true;
            break;
        case ExecActionType::ACTION_SET_DAC:
            if (!directTemplate) return false;
            minimum = 0.0;
            maximum = 255.0;
            integerOnly = true;
            break;
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
            if (!directTemplate && !fixedModbusAddressTemplate(action.actionValue)) return false;
            minimum = 0.0;
            maximum = 65535.0;
            integerOnly = true;
            break;
        case ExecActionType::ACTION_DISPLAY_NUMBER:
            if (!directTemplate) return false;
            bounded = false;
            break;
        case ExecActionType::ACTION_REMOTE_CHANNEL: {
            if (!directTemplate) return false;
            const String ref = action.targetPeriphId.startsWith("remote.")
                ? action.targetPeriphId : String("remote.") + action.targetPeriphId;
            bool writableNumber = false;
            for (JsonObjectConst control : remoteControls) {
                if (String(control["controlRef"] | "") == ref &&
                    String(control["type"] | "") == "number" &&
                    (control["writable"] | false)) {
                    writableNumber = true;
                    break;
                }
            }
            if (!writableNumber) return false;
            bounded = false;
            break;
        }
        default:
            return false;
    }
    if (bounded) {
        constraints.minimum = std::max(constraints.minimum, minimum);
        constraints.maximum = std::min(constraints.maximum, maximum);
        constraints.bounded = true;
    }
    constraints.integerOnly = constraints.integerOnly || integerOnly;
    return constraints.minimum <= constraints.maximum;
}

PanelRuleInputKind panelRuleInputKind(const PeriphExecRule& rule,
                                     JsonArrayConst remoteControls,
                                     PanelNumericConstraints* numeric = nullptr) {
    PanelRuleInputKind resolved = PanelRuleInputKind::TRIGGER;
    PanelNumericConstraints constraints;
    for (const auto& action : rule.actions) {
        if (!action.useReceivedValue) continue;
        PanelRuleInputKind actionKind = PanelRuleInputKind::UNSUPPORTED;
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
            const String ref = action.targetPeriphId.startsWith("remote.")
                ? action.targetPeriphId : String("remote.") + action.targetPeriphId;
            for (JsonObjectConst control : remoteControls) {
                if (String(control["controlRef"] | "") != ref) continue;
                const String type = control["type"] | "";
                if (type == "switch") {
                    actionKind = PanelRuleInputKind::BOOLEAN;
                } else if (type == "number" &&
                           numericActionConstraints(action, remoteControls, constraints)) {
                    actionKind = PanelRuleInputKind::NUMBER;
                }
                break;
            }
        } else if (numericActionConstraints(action, remoteControls, constraints)) {
            actionKind = PanelRuleInputKind::NUMBER;
        }
        if (actionKind == PanelRuleInputKind::UNSUPPORTED ||
            (resolved != PanelRuleInputKind::TRIGGER && resolved != actionKind)) {
            return PanelRuleInputKind::UNSUPPORTED;
        }
        resolved = actionKind;
    }
    if (numeric && resolved == PanelRuleInputKind::NUMBER) *numeric = constraints;
    return resolved;
}

PanelRuleInputKind resolvePanelRuleInputKind(const PeriphExecRule& rule,
                                             PanelNumericConstraints* numeric = nullptr) {
    JsonDocument choices;
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    bool receivesValue = false;
    for (const auto& action : rule.actions) receivesValue |= action.useReceivedValue;
    if (receivesValue) {
        FastBee::RemoteResourceManager::instance().appendChoices(
            choices["sources"].to<JsonArray>(), choices["controls"].to<JsonArray>());
    }
#endif
    return panelRuleInputKind(rule, choices["controls"].as<JsonArrayConst>(), numeric);
}

bool validatePanelControlInput(PanelRuleInputKind inputKind, const String& visual,
                               String& error) {
    if (inputKind == PanelRuleInputKind::UNSUPPORTED) {
        error = "Panel input is not supported; configure a fixed-value execution rule";
        return false;
    }
    if ((inputKind == PanelRuleInputKind::BOOLEAN && visual != "switch") ||
        (inputKind == PanelRuleInputKind::NUMBER && visual != "number") ||
        (inputKind == PanelRuleInputKind::TRIGGER && visual != "action")) {
        error = "Panel control type does not match the rule input; re-save the binding";
        return false;
    }
    return true;
}

bool knownSystemSource(const String& sourceRef) {
    return sourceRef == "system.uptime" || sourceRef == "system.freeHeap" ||
           sourceRef == "system.wifiRssi" || sourceRef == "system.network";
}

bool knownLocalSource(const String& sourceRef) {
    ObservationSnapshot observation;
    if (PeriphExecManager::getInstance().getObservation(sourceRef, observation)) {
        return observation.bindingValid;
    }
    const auto rules = PeriphExecManager::getInstance().getAllRules();
    for (const auto& rule : rules) {
        for (const auto& action : rule.actions) {
            if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ) &&
                (sourceRef == action.targetPeriphId ||
                 sourceRef.startsWith(action.targetPeriphId + "_"))) return true;
            // Modbus mappings register their exact references in the shared
            // observation cache. A poll rule does not authorize arbitrary
            // modbus.* names or references removed during reconfiguration.
        }
    }
    return false;
}

// Resolve only cached/configured metadata. Saving a chart never samples a bus.
bool panelNumericSourceUnit(const String& sourceRef, String& unit) {
    if (sourceRef == "system.uptime") { unit = "s"; return true; }
    if (sourceRef == "system.freeHeap") { unit = "B"; return true; }
    if (sourceRef == "system.wifiRssi") { unit = "dBm"; return true; }
    ObservationSnapshot observation;
    if (PeriphExecManager::getInstance().getObservation(sourceRef, observation) &&
        observation.bindingValid && observation.valueType == "number") {
        unit = observation.unit;
        return true;
    }
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    JsonDocument choices;
    JsonArray sources = choices["sources"].to<JsonArray>();
    FastBee::RemoteResourceManager::instance().appendChoices(
        sources, choices["controls"].to<JsonArray>());
    for (JsonObjectConst source : sources) {
        if (String(source["sourceRef"] | "") == sourceRef &&
            String(source["type"] | "") == "number") {
            unit = String(source["unit"] | "");
            return true;
        }
    }
#endif
    return false;
}

void appendPanelClock(JsonObject clock) {
    const time_t now = time(nullptr);
    struct tm local {};
    bool valid = now >= 1609459200;
    if (valid) {
#if defined(_WIN32)
        valid = localtime_s(&local, &now) == 0;
#else
        valid = localtime_r(&now, &local) != nullptr;
#endif
    }
    // A valid device wall clock may come from NTP or another existing time
    // source. Do not claim a fresh NTP exchange or substitute browser time.
    clock["valid"] = valid;
    clock["quality"] = valid ? "good" : "unknown";
    if (!valid) return;
    char timeText[8];
    char dateText[16];
    strftime(timeText, sizeof(timeText), "%H:%M", &local);
    strftime(dateText, sizeof(dateText), "%Y-%m-%d", &local);
    clock["time"] = JsonString(timeText);
    clock["date"] = JsonString(dateText);
}

bool backupPanelBeforeTouchMigration(const String& sourcePath) {
    const String backup = String(ScreenPanelManager::CONFIG_FILE) + ".touch-v1.bak";
    auto validBackup = [&]() {
        File saved = LittleFS.open(backup, FILE_READ);
        if (!saved || saved.size() > ScreenPanelManager::MAX_CONFIG_BYTES) return false;
        JsonDocument doc;
        const bool valid = !deserializeJson(doc, saved) && doc["storageVersion"] == 1 && doc["config"].is<JsonObjectConst>();
        saved.close(); return valid;
    };
    // Never overwrite the original migration backup with a later revision.
    if (LittleFS.exists(backup)) return validBackup();
    const String temporary = backup + ".tmp";
    if (LittleFS.exists(temporary)) LittleFS.remove(temporary);
    File input = LittleFS.open(sourcePath, FILE_READ);
    File output = LittleFS.open(temporary, FILE_WRITE);
    if (!input || !output) { if (input) input.close(); if (output) output.close(); LittleFS.remove(temporary); return false; }
    const size_t expected = input.size(); size_t copied = 0; uint8_t buffer[128];
    while (input.available()) {
        const size_t count = input.read(buffer, sizeof(buffer));
        if (!count || output.write(buffer, count) != count) break;
        copied += count;
    }
    input.close(); output.flush(); output.close();
    if (copied != expected || !LittleFS.rename(temporary, backup)) { LittleFS.remove(temporary); return false; }
    return validBackup();
}

}  // namespace

ScreenPanelManager& ScreenPanelManager::instance() {
    static ScreenPanelManager manager;
    return manager;
}

void ScreenPanelManager::makeDefaultConfig(JsonDocument& out) {
    out.clear();
    out["schemaVersion"] = 1;
    out["displayRef"] = nullptr;
    JsonObject settings = out["settings"].to<JsonObject>();
    settings["brightnessPercent"] = 70;
    settings["idleSleepSeconds"] = 120;
    settings["touchBeepEnabled"] = true;
    settings["controlConfirmIdleSeconds"] = 60;
    settings["homePageId"] = "home";
    settings["themeId"] = "dark";
    settings["background"]["presetId"] = "solid";
    settings["allowSystemRestart"] = false;
    JsonArray bottomNav = settings["bottomNavPageIds"].to<JsonArray>();
    bottomNav.add("home");
    JsonObject page = out["pages"].to<JsonArray>().add<JsonObject>();
    page["id"] = "home";
    page["title"] = "Home";
    page["layoutId"] = "mixed-grid";
    page["layoutMode"] = "template";
    page["logicalWidth"] = 480;
    page["logicalHeight"] = 480;
    page["rotation"] = 0;
    page["cards"].to<JsonArray>();
}

bool ScreenPanelManager::initialize() {
    if (_initialized) return true;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    String loaded;
    uint32_t loadedRevision = 1;
    String loadedPath = CONFIG_FILE;
    bool needsMigration = false;
    bool persisted = loadConfigFile(loadedPath, loaded, loadedRevision, &needsMigration);
    if (!persisted) {
        loadedPath = String(CONFIG_FILE) + ".bak";
        persisted = loadConfigFile(loadedPath, loaded, loadedRevision, &needsMigration);
    }
    if (!persisted) {
        JsonDocument defaults;
        makeDefaultConfig(defaults);
        serializeJson(defaults, loaded);
        loadedRevision = 1;
    }

    JsonDocument parsed;
    if (deserializeJson(parsed, loaded)) return false;
    // Startup only: normal config/state GETs never rewrite the file. Upgrade
    // a copy, validate it, preserve one original backup, then use the existing
    // atomic envelope replacement and .bak recovery path.
    String migrationError;
    if (persisted && needsMigration && loadedRevision < UINT32_MAX) {
        JsonDocument candidate; candidate.set(parsed);
        if (ScreenPanelMigration::upgrade(candidate.as<JsonObject>())) {
            JsonDocument normalized;
            if (validateConfig(candidate.as<JsonVariantConst>(), normalized, migrationError) &&
                backupPanelBeforeTouchMigration(loadedPath) && writeEnvelope(normalized.as<JsonVariantConst>(), loadedRevision + 1)) {
                parsed = std::move(normalized); loaded = ""; serializeJson(parsed, loaded); ++loadedRevision;
            } else if (migrationError.isEmpty()) migrationError = "Screen layout migration deferred: backup or storage unavailable";
        }
    }
    _configJson = loaded;
    _revision = loadedRevision;
    _persisted = persisted;
    _displayRef = String(parsed["displayRef"] | "");
    _activePageId = parsed["settings"]["homePageId"] | "home";
    _applied = false;
    _restartRequired = false;
    _applicationError = migrationError;
    _initialized = true;
    return true;
}

bool ScreenPanelManager::loadConfigFile(const String& path, String& configJson,
                                        uint32_t& revision, bool* needsMigration) const {
    if (!LittleFS.exists(path)) return false;
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.size() == 0 || file.size() > MAX_CONFIG_BYTES) {
        if (file) file.close();
        return false;
    }
    JsonDocument envelope;
    const DeserializationError parseError = deserializeJson(envelope, file);
    file.close();
    if (parseError || !envelope.is<JsonObjectConst>() ||
        envelope["storageVersion"] != 1 || !envelope["revision"].is<uint32_t>() ||
        !envelope["config"].is<JsonObjectConst>()) return false;

    JsonDocument normalized;
    String validationError;
    if (!validateConfig(envelope["config"].as<JsonVariantConst>(), normalized,
                        validationError)) return false;
    revision = envelope["revision"].as<uint32_t>();
    if (revision == 0) return false;
    if (needsMigration) *needsMigration = envelope["touchUiMigration"] != 2;
    configJson = "";
    serializeJson(normalized, configJson);
    return !configJson.isEmpty();
}

bool ScreenPanelManager::copyConfig(JsonDocument& out, uint32_t& revision,
                                    bool& persisted) const {
    PanelLock lock(_mutex);
    if (!lock.locked() || !_initialized) return false;
    if (deserializeJson(out, _configJson)) return false;
    revision = _revision;
    persisted = _persisted;
    return true;
}

bool ScreenPanelManager::validateConfig(JsonVariantConst submitted,
                                        JsonDocument& normalized,
                                        String& error) {
    error = "";
    normalized.clear();
    if (!submitted.is<JsonObjectConst>()) {
        error = "config must be an object";
        return false;
    }
    JsonObjectConst root = submitted.as<JsonObjectConst>();
    const char* const rootKeys[] = {"schemaVersion", "displayRef", "settings", "pages"};
    if (!onlyKeys(root, rootKeys, "config", error)) return false;
    if (!root["schemaVersion"].is<int>() || root["schemaVersion"].as<int>() != 1) {
        error = "config.schemaVersion must be 1";
        return false;
    }
    normalized["schemaVersion"] = 1;

    if (!root["displayRef"].isNull()) {
        if (!boundedString(root["displayRef"], 40, true, "config.displayRef", error) ||
            !validIdentifier(root["displayRef"].as<String>())) {
            if (error.isEmpty()) error = "config.displayRef is invalid";
            return false;
        }
        normalized["displayRef"] = root["displayRef"];
    } else {
        normalized["displayRef"] = nullptr;
    }

    if (!root["settings"].is<JsonObjectConst>()) {
        error = "config.settings must be an object";
        return false;
    }
    JsonObjectConst settings = root["settings"].as<JsonObjectConst>();
    const char* const settingKeys[] = {"brightnessPercent", "idleSleepSeconds", "homePageId",
                                       "themeId", "background", "allowSystemRestart",
                                       "touchBeepEnabled", "controlConfirmIdleSeconds",
                                       "bottomNavPageIds"};
    if (!onlyKeys(settings, settingKeys, "config.settings", error)) return false;
    int brightness = 70;
    int sleepSeconds = 120;
    int confirmIdleSeconds = 60;
    if (!readBoundedInt(settings["brightnessPercent"], 0, 100, 70, brightness,
                        "config.settings.brightnessPercent", error) ||
        !readBoundedInt(settings["idleSleepSeconds"], 0, 86400, 120, sleepSeconds,
                        "config.settings.idleSleepSeconds", error) ||
        !readBoundedInt(settings["controlConfirmIdleSeconds"], 15, 600, 60, confirmIdleSeconds,
                        "config.settings.controlConfirmIdleSeconds", error)) return false;
    if (!settings["touchBeepEnabled"].isNull() && !settings["touchBeepEnabled"].is<bool>()) {
        error = "config.settings.touchBeepEnabled must be boolean";
        return false;
    }
    if (!boundedString(settings["homePageId"], 40, true,
                       "config.settings.homePageId", error) ||
        !validIdentifier(settings["homePageId"].as<String>())) {
        if (error.isEmpty()) error = "config.settings.homePageId is invalid";
        return false;
    }
    String theme = settings["themeId"] | "dark";
    if (!settings["themeId"].isNull() && !settings["themeId"].is<const char*>()) {
        error = "config.settings.themeId must be a string";
        return false;
    }
    if (!oneOf(theme, {"dark", "light", "high-contrast"})) {
        error = "config.settings.themeId is unsupported";
        return false;
    }
    if (!settings["allowSystemRestart"].isNull() &&
        !settings["allowSystemRestart"].is<bool>()) {
        error = "config.settings.allowSystemRestart must be boolean";
        return false;
    }
    String backgroundPreset = "solid";
    if (!settings["background"].isNull()) {
        if (!settings["background"].is<JsonObjectConst>()) {
            error = "config.settings.background must be an object";
            return false;
        }
        JsonObjectConst background = settings["background"].as<JsonObjectConst>();
        const char* const backgroundKeys[] = {"presetId"};
        if (!onlyKeys(background, backgroundKeys, "config.settings.background", error) ||
            !boundedString(background["presetId"], 16, true,
                           "config.settings.background.presetId", error)) return false;
        backgroundPreset = background["presetId"].as<String>();
        if (!oneOf(backgroundPreset, {"solid", "grid", "soft-gradient"})) {
            error = "config.settings.background.presetId is unsupported";
            return false;
        }
    }

    JsonObject normalizedSettings = normalized["settings"].to<JsonObject>();
    normalizedSettings["brightnessPercent"] = brightness;
    normalizedSettings["idleSleepSeconds"] = sleepSeconds;
    normalizedSettings["touchBeepEnabled"] = settings["touchBeepEnabled"] | true;
    normalizedSettings["controlConfirmIdleSeconds"] = confirmIdleSeconds;
    normalizedSettings["homePageId"] = settings["homePageId"];
    normalizedSettings["themeId"] = theme;
    normalizedSettings["background"]["presetId"] = backgroundPreset;
    normalizedSettings["allowSystemRestart"] = settings["allowSystemRestart"] | false;

    if (!root["pages"].is<JsonArrayConst>()) {
        error = "config.pages must be an array";
        return false;
    }
    JsonArrayConst pages = root["pages"].as<JsonArrayConst>();
    if (pages.size() == 0 || pages.size() > ScreenPanelSnapshot::MAX_PAGES) {
        error = "config.pages must contain 1 to 8 pages";
        return false;
    }

    JsonArray normalizedPages = normalized["pages"].to<JsonArray>();
    String pageIds[ScreenPanelSnapshot::MAX_PAGES];
    String cardIds[ScreenPanelSnapshot::MAX_CARDS];
    size_t pageCount = 0;
    size_t cardCount = 0;
    for (JsonObjectConst page : pages) {
        const String pagePath = String("config.pages[") + pageCount + "]";
        const char* const pageKeys[] = {"id", "title", "layoutId", "layoutMode",
                                        "logicalWidth", "logicalHeight", "rotation",
                                        "themeId", "background", "cards"};
        if (!onlyKeys(page, pageKeys, pagePath, error) ||
            !boundedString(page["id"], 40, true, pagePath + ".id", error) ||
            !boundedString(page["title"], 64, true, pagePath + ".title", error)) return false;
        const String pageId = page["id"].as<String>();
        if (!validIdentifier(pageId)) {
            error = pagePath + ".id is invalid";
            return false;
        }
        for (size_t i = 0; i < pageCount; ++i) {
            if (pageIds[i] == pageId) {
                error = pagePath + ".id is duplicated";
                return false;
            }
        }
        pageIds[pageCount++] = pageId;

        String layoutId = page["layoutId"] | "mixed-grid";
        String layoutMode = page["layoutMode"] | "template";
        if ((!page["layoutId"].isNull() && !page["layoutId"].is<const char*>()) ||
            (!page["layoutMode"].isNull() && !page["layoutMode"].is<const char*>()) ||
            !oneOf(layoutId, {"mixed-grid", "info-home", "tile-grid", "detail"}) ||
            !oneOf(layoutMode, {"template", "canvas"})) {
            error = pagePath + " has an unsupported layout";
            return false;
        }
        int logicalWidth = 480;
        int logicalHeight = 480;
        int rotation = 0;
        if (!readBoundedInt(page["logicalWidth"], 128, 2048, 480, logicalWidth,
                            pagePath + ".logicalWidth", error) ||
            !readBoundedInt(page["logicalHeight"], 128, 2048, 480, logicalHeight,
                            pagePath + ".logicalHeight", error) ||
            !readBoundedInt(page["rotation"], 0, 270, 0, rotation,
                            pagePath + ".rotation", error)) return false;
        if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
            error = pagePath + ".rotation must be 0, 90, 180 or 270";
            return false;
        }
        if (!page["cards"].is<JsonArrayConst>()) {
            error = pagePath + ".cards must be an array";
            return false;
        }

        JsonObject normalizedPage = normalizedPages.add<JsonObject>();
        normalizedPage["id"] = pageId;
        normalizedPage["title"] = page["title"];
        normalizedPage["layoutId"] = layoutId;
        normalizedPage["layoutMode"] = layoutMode;
        normalizedPage["logicalWidth"] = logicalWidth;
        normalizedPage["logicalHeight"] = logicalHeight;
        normalizedPage["rotation"] = rotation;
        if (!page["themeId"].isNull()) {
            if (!page["themeId"].is<const char*>() ||
                !oneOf(page["themeId"].as<String>(), {"dark", "light", "high-contrast"})) {
                error = pagePath + ".themeId is unsupported";
                return false;
            }
            normalizedPage["themeId"] = page["themeId"];
        }
        if (!page["background"].isNull()) {
            if (!page["background"].is<JsonObjectConst>()) {
                error = pagePath + ".background must be an object";
                return false;
            }
            JsonObjectConst background = page["background"].as<JsonObjectConst>();
            const char* const backgroundKeys[] = {"presetId"};
            if (!onlyKeys(background, backgroundKeys, pagePath + ".background", error) ||
                !boundedString(background["presetId"], 16, true,
                               pagePath + ".background.presetId", error) ||
                !oneOf(background["presetId"].as<String>(), {"solid", "grid", "soft-gradient"})) {
                if (error.isEmpty()) error = pagePath + ".background.presetId is unsupported";
                return false;
            }
            normalizedPage["background"]["presetId"] = background["presetId"];
        }

        JsonArray normalizedCards = normalizedPage["cards"].to<JsonArray>();
        ScreenPanelRect pageRects[ScreenPanelSnapshot::MAX_CARDS];
        size_t pageRectCount = 0;
        size_t localCardIndex = 0;
        for (JsonObjectConst card : page["cards"].as<JsonArrayConst>()) {
            if (cardCount >= ScreenPanelSnapshot::MAX_CARDS) {
                error = "config contains more than 24 cards";
                return false;
            }
            const String cardPath = pagePath + ".cards[" + localCardIndex + "]";
            const char* const cardKeys[] = {"id", "kind", "visualType", "displayMode", "title",
                                            "unit", "precision", "sourceRef", "sourceRefs",
                                            "controlRef", "systemActionId", "actionValue", "pageId", "rect",
                                            "iconId", "requireConfirmation", "historyMode",
                                            "rangeMin", "rangeMax", "step", "initialValue",
                                            "bindingFingerprint", "motorSpeed", "motorPulse", "motorSpeedStep", "motorPulseStep"};
            if (!onlyKeys(card, cardKeys, cardPath, error) ||
                !boundedString(card["id"], 40, true, cardPath + ".id", error) ||
                !boundedString(card["kind"], 16, true, cardPath + ".kind", error) ||
                !boundedString(card["title"], 64, true, cardPath + ".title", error)) return false;
            const String cardId = card["id"].as<String>();
            if (!validIdentifier(cardId)) {
                error = cardPath + ".id is invalid";
                return false;
            }
            for (size_t i = 0; i < cardCount; ++i) {
                if (cardIds[i] == cardId) {
                    error = cardPath + ".id is duplicated";
                    return false;
                }
            }
            cardIds[cardCount++] = cardId;

            ScreenCardKind kind;
            if (!parseKind(card["kind"].as<String>(), kind)) {
                error = cardPath + ".kind is unsupported";
                return false;
            }
            if (!card["visualType"].isNull() && !card["displayMode"].isNull()) {
                error = cardPath + " must not contain both visualType and displayMode";
                return false;
            }
            JsonVariantConst visualValue = card["visualType"].isNull()
                ? card["displayMode"] : card["visualType"];
            if (!boundedString(visualValue, 16, true, cardPath + ".visualType", error)) return false;
            ScreenVisualType visual;
            if (!parseVisual(visualValue.as<String>(), visual) || !visualMatches(kind, visual)) {
                error = cardPath + ".visualType is incompatible with kind";
                return false;
            }

            if (!card["rect"].is<JsonObjectConst>()) {
                error = cardPath + ".rect must be an object";
                return false;
            }
            JsonObjectConst rect = card["rect"].as<JsonObjectConst>();
            const char* const rectKeys[] = {"x", "y", "w", "h"};
            if (!onlyKeys(rect, rectKeys, cardPath + ".rect", error)) return false;
            int x = 0, y = 0, w = 0, h = 0;
            if (!readBoundedInt(rect["x"], 0, logicalWidth, 0, x, cardPath + ".rect.x", error) ||
                !readBoundedInt(rect["y"], 0, logicalHeight, 0, y, cardPath + ".rect.y", error) ||
                !readBoundedInt(rect["w"], 1, logicalWidth, 0, w, cardPath + ".rect.w", error) ||
                !readBoundedInt(rect["h"], 1, logicalHeight, 0, h, cardPath + ".rect.h", error)) return false;
            if (x + w > logicalWidth || y + h > logicalHeight) {
                error = cardPath + ".rect exceeds page bounds";
                return false;
            }
            const bool interactive = kind == ScreenCardKind::CONTROL ||
                                     kind == ScreenCardKind::SYSTEM ||
                                     kind == ScreenCardKind::PAGE_LINK;
            if (interactive && (w < 44 || h < 44)) {
                error = cardPath + ".rect is smaller than the minimum touch target";
                return false;
            }
            if (visual == ScreenVisualType::NUMBER_CONTROL && (w < 180 || h < 112)) {
                error = cardPath + ".rect is too small for a numeric control";
                return false;
            }
            if (visual == ScreenVisualType::MOTOR_CONTROL && (w < 280 || h < 300)) {
                error = cardPath + ".rect is too small for motor controls"; return false;
            }
            if (visual == ScreenVisualType::SLIDER && (w < 180 || h < 80)) {
                error = cardPath + ".rect is too small for a slider"; return false;
            }
            ScreenPanelRect parsedRect{static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                       static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
            for (size_t i = 0; i < pageRectCount; ++i) {
                if (rectsOverlap(parsedRect, pageRects[i])) {
                    error = cardPath + ".rect overlaps another card";
                    return false;
                }
            }
            pageRects[pageRectCount++] = parsedRect;

            if (!card["unit"].isNull() &&
                !boundedString(card["unit"], 16, false, cardPath + ".unit", error)) return false;
            if (!card["iconId"].isNull() &&
                (!boundedString(card["iconId"], 24, false, cardPath + ".iconId", error) ||
                 (!card["iconId"].as<String>().isEmpty() && !validIdentifier(card["iconId"].as<String>(), 24)))) {
                if (error.isEmpty()) error = cardPath + ".iconId is invalid";
                return false;
            }
            int precision = 1;
            if (!readBoundedInt(card["precision"], 0, 6, 1, precision,
                                cardPath + ".precision", error)) return false;
            if (!card["requireConfirmation"].isNull() &&
                !card["requireConfirmation"].is<bool>()) {
                error = cardPath + ".requireConfirmation must be boolean";
                return false;
            }
            const bool confirmation = card["requireConfirmation"] | false;

            // Weather/control templates may be saved before a service or rule
            // is selected. Numeric data/charts still require a real binding.
            const bool needsSource = kind == ScreenCardKind::DATA ||
                                     kind == ScreenCardKind::CHART;
            if (needsSource && (!boundedString(card["sourceRef"], 96, true,
                                               cardPath + ".sourceRef", error) ||
                                !validReference(card["sourceRef"].as<String>()))) {
                if (error.isEmpty()) error = cardPath + ".sourceRef is invalid";
                return false;
            }
            if (!card["sourceRef"].isNull() &&
                (!card["sourceRef"].is<const char*>() ||
                 (!(kind == ScreenCardKind::WEATHER && card["sourceRef"].as<String>().isEmpty()) &&
                  !validReference(card["sourceRef"].as<String>())))) {
                error = cardPath + ".sourceRef is invalid";
                return false;
            }
            if (!card["sourceRefs"].isNull()) {
                if (!card["sourceRefs"].is<JsonArrayConst>() || card["sourceRefs"].size() > 4) {
                    error = cardPath + ".sourceRefs must contain at most 4 references";
                    return false;
                }
                if (card["sourceRefs"].size() && visual != ScreenVisualType::BAR) {
                    error = cardPath + ".sourceRefs is only supported for bars";
                    return false;
                }
                String seenSources[4];
                size_t sourceIndex = 0;
                for (JsonVariantConst source : card["sourceRefs"].as<JsonArrayConst>()) {
                    if (!source.is<const char*>() || !validReference(source.as<String>())) {
                        error = cardPath + ".sourceRefs contains an invalid reference";
                        return false;
                    }
                    const String sourceRef = source.as<String>();
                    if (sourceIndex == 0 && sourceRef != String(card["sourceRef"] | "")) {
                        error = cardPath + ".sourceRefs must start with sourceRef";
                        return false;
                    }
                    for (size_t prior = 0; prior < sourceIndex; ++prior) {
                        if (seenSources[prior] == sourceRef) {
                            error = cardPath + ".sourceRefs must be unique";
                            return false;
                        }
                    }
                    seenSources[sourceIndex++] = sourceRef;
                }
                if (sourceIndex && (h < 36 + 40 * sourceIndex || w < 100)) {
                    error = cardPath + ".rect is too small for the bar sources";
                    return false;
                }
            }
            if (kind == ScreenCardKind::CONTROL &&
                (!boundedString(card["controlRef"], 96, false,
                                cardPath + ".controlRef", error) ||
                 (!String(card["controlRef"] | "").isEmpty() &&
                  !validReference(card["controlRef"].as<String>())))) {
                if (error.isEmpty()) error = cardPath + ".controlRef is invalid";
                return false;
            }
            if (kind == ScreenCardKind::PAGE_LINK &&
                (!boundedString(card["pageId"], 40, true, cardPath + ".pageId", error) ||
                 !validIdentifier(card["pageId"].as<String>()))) {
                if (error.isEmpty()) error = cardPath + ".pageId is invalid";
                return false;
            }
            if (kind == ScreenCardKind::SYSTEM) {
                if (!boundedString(card["systemActionId"], 16, true,
                                   cardPath + ".systemActionId", error)) {
                    return false;
                }
                const String systemActionId = card["systemActionId"].as<String>();
                if (!oneOf(systemActionId,
                           {"brightness", "sleep", "sleep-time", "touch-beep", "confirm-idle", "buzzer", "restart"})) {
                    error = cardPath + ".systemActionId is unsupported";
                    return false;
                }
                if ((visual == ScreenVisualType::SLIDER && systemActionId != "brightness") ||
                    (visual == ScreenVisualType::SELECT && systemActionId != "sleep-time" && systemActionId != "confirm-idle") ||
                    (visual == ScreenVisualType::SWITCH && systemActionId != "touch-beep") ||
                    (systemActionId == "touch-beep" && visual != ScreenVisualType::SWITCH) ||
                    (systemActionId == "confirm-idle" && visual != ScreenVisualType::SELECT)) {
                    error = cardPath + " has an incompatible system widget"; return false;
                }
                if (systemActionId == "brightness") {
                    int actionValue = 70;
                    if (!readBoundedInt(card["actionValue"], 0, 100, 70,
                                        actionValue,
                                        cardPath + ".actionValue", error)) {
                        return false;
                    }
                } else if (!card["actionValue"].isNull()) {
                    error = cardPath + ".actionValue is only supported for brightness";
                    return false;
                }
                if (systemActionId == "restart" &&
                    (!(settings["allowSystemRestart"] | false) || !confirmation)) {
                    error = cardPath + " restart requires permission and confirmation";
                    return false;
                }
            } else if (!card["systemActionId"].isNull() ||
                       !card["actionValue"].isNull()) {
                error = cardPath + " system action fields require kind=system";
                return false;
            }

            String historyMode = card["historyMode"] | "none";
            if (!card["historyMode"].isNull() && !card["historyMode"].is<const char*>()) {
                error = cardPath + ".historyMode must be a string";
                return false;
            }
            if (!oneOf(historyMode, {"none", "recent10"}) ||
                (historyMode == "recent10" && visual != ScreenVisualType::LINE)) {
                error = cardPath + ".historyMode is incompatible with visualType";
                return false;
            }

            const bool hasMin = !card["rangeMin"].isNull();
            const bool hasMax = !card["rangeMax"].isNull();
            if (hasMin != hasMax) {
                error = cardPath + " must define rangeMin and rangeMax together";
                return false;
            }
            double rangeMin = 0;
            double rangeMax = 0;
            if (hasMin) {
                if (!readFinitePanelNumber(card["rangeMin"], rangeMin,
                                           cardPath + ".rangeMin", error) ||
                    !readFinitePanelNumber(card["rangeMax"], rangeMax,
                                           cardPath + ".rangeMax", error)) return false;
                if (rangeMin >= rangeMax) {
                    error = cardPath + " range is invalid";
                    return false;
                }
            } else if (visual == ScreenVisualType::GAUGE) {
                error = cardPath + " gauge requires a range";
                return false;
            }

            double numericStep = 0;
            double initialValue = 0;
            if (visual == ScreenVisualType::NUMBER_CONTROL) {
                if (!hasMin || card["step"].isNull() || card["initialValue"].isNull()) {
                    error = cardPath + " numeric control requires range, step and initialValue";
                    return false;
                }
                if (!readFinitePanelNumber(card["step"], numericStep,
                                           cardPath + ".step", error) ||
                    !readFinitePanelNumber(card["initialValue"], initialValue,
                                           cardPath + ".initialValue", error)) return false;
                if (numericStep <= 0.0 || numericStep > rangeMax - rangeMin ||
                    initialValue < rangeMin || initialValue > rangeMax ||
                    !panelNumberFitsPrecision(rangeMin, precision) ||
                    !panelNumberFitsPrecision(rangeMax, precision) ||
                    !panelNumberFitsPrecision(numericStep, precision) ||
                    !panelNumberFitsPrecision(initialValue, precision) ||
                    !panelNumberAligned(initialValue, rangeMin, numericStep)) {
                    error = cardPath + " numeric range, step or initialValue is invalid";
                    return false;
                }
            } else if (!card["step"].isNull() || !card["initialValue"].isNull()) {
                error = cardPath + " step and initialValue require visualType=number";
                return false;
            }

            int motorSpeed = 50, motorPulse = 1600, motorSpeedStep = 1, motorPulseStep = 100;
            if (visual == ScreenVisualType::MOTOR_CONTROL) {
                if (!readBoundedInt(card["motorSpeed"], 0, 65535, 50, motorSpeed, cardPath + ".motorSpeed", error) ||
                    !readBoundedInt(card["motorPulse"], 0, 65535, 1600, motorPulse, cardPath + ".motorPulse", error) ||
                    !readBoundedInt(card["motorSpeedStep"], 1, 65535, 1, motorSpeedStep, cardPath + ".motorSpeedStep", error) ||
                    !readBoundedInt(card["motorPulseStep"], 1, 65535, 100, motorPulseStep, cardPath + ".motorPulseStep", error)) return false;
            } else if (!card["motorSpeed"].isNull() || !card["motorPulse"].isNull() || !card["motorSpeedStep"].isNull() || !card["motorPulseStep"].isNull()) {
                error = cardPath + " motor fields require motor visual"; return false;
            }
            JsonObject normalizedCard = normalizedCards.add<JsonObject>();
            if (visual == ScreenVisualType::MOTOR_CONTROL) {
                normalizedCard["motorSpeed"] = motorSpeed; normalizedCard["motorPulse"] = motorPulse;
                normalizedCard["motorSpeedStep"] = motorSpeedStep; normalizedCard["motorPulseStep"] = motorPulseStep;
            }
            normalizedCard["id"] = cardId;
            normalizedCard["kind"] = card["kind"];
            normalizedCard["visualType"] = visualValue;
            normalizedCard["title"] = card["title"];
            if (!card["unit"].isNull()) normalizedCard["unit"] = card["unit"];
            normalizedCard["precision"] = precision;
            if (!card["sourceRef"].isNull()) normalizedCard["sourceRef"] = card["sourceRef"];
            if (!card["sourceRefs"].isNull()) normalizedCard["sourceRefs"] = card["sourceRefs"];
            if (!card["controlRef"].isNull()) normalizedCard["controlRef"] = card["controlRef"];
            if (!card["bindingFingerprint"].isNull()) {
                const char* fingerprint = card["bindingFingerprint"] | "";
                if (strlen(fingerprint) != 8 || strspn(fingerprint, "0123456789abcdefABCDEF") != 8) {
                    error = cardPath + ".bindingFingerprint must be an 8-digit hexadecimal string"; return false;
                }
                normalizedCard["bindingFingerprint"] = card["bindingFingerprint"];
            }
            if (!card["systemActionId"].isNull()) normalizedCard["systemActionId"] = card["systemActionId"];
            if (kind == ScreenCardKind::SYSTEM &&
                String(card["systemActionId"] | "") == "brightness") {
                normalizedCard["actionValue"] = card["actionValue"] | 70;
            }
            if (!card["pageId"].isNull()) normalizedCard["pageId"] = card["pageId"];
            normalizedCard["rect"]["x"] = x;
            normalizedCard["rect"]["y"] = y;
            normalizedCard["rect"]["w"] = w;
            normalizedCard["rect"]["h"] = h;
            if (!card["iconId"].isNull()) normalizedCard["iconId"] = card["iconId"];
            normalizedCard["requireConfirmation"] = visual == ScreenVisualType::MOTOR_CONTROL || confirmation;
            normalizedCard["historyMode"] = historyMode;
            if (hasMin) {
                normalizedCard["rangeMin"] = rangeMin;
                normalizedCard["rangeMax"] = rangeMax;
            }
            if (visual == ScreenVisualType::NUMBER_CONTROL) {
                normalizedCard["step"] = numericStep;
                normalizedCard["initialValue"] = initialValue;
            }
            ++localCardIndex;
        }
    }

    const String homePageId = normalized["settings"]["homePageId"].as<String>();
    bool homeFound = false;
    for (size_t i = 0; i < pageCount; ++i) homeFound = homeFound || pageIds[i] == homePageId;
    if (!homeFound) {
        error = "config.settings.homePageId does not reference a page";
        return false;
    }
    JsonArray normalizedBottomNav = normalized["settings"]["bottomNavPageIds"].to<JsonArray>();
    JsonVariantConst submittedBottomNav = settings["bottomNavPageIds"];
    if (submittedBottomNav.isNull() ||
        (submittedBottomNav.is<JsonArrayConst>() && submittedBottomNav.size() == 0)) {
        for (size_t i = 0; i < pageCount &&
             i < ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES; ++i) {
            normalizedBottomNav.add(pageIds[i]);
        }
    } else {
        if (!submittedBottomNav.is<JsonArrayConst>() ||
            submittedBottomNav.size() > ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES) {
            error = "config.settings.bottomNavPageIds must contain at most 5 pages";
            return false;
        }
        String seenNav[ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES];
        size_t navCount = 0;
        for (JsonVariantConst nav : submittedBottomNav.as<JsonArrayConst>()) {
            if (!nav.is<const char*>() || !validIdentifier(nav.as<String>())) {
                error = "config.settings.bottomNavPageIds contains an invalid page id";
                return false;
            }
            const String navId = nav.as<String>();
            bool found = false;
            for (size_t i = 0; i < pageCount; ++i) found = found || pageIds[i] == navId;
            if (!found) {
                error = "config.settings.bottomNavPageIds references an unknown page";
                return false;
            }
            for (size_t i = 0; i < navCount; ++i) {
                if (seenNav[i] == navId) {
                    error = "config.settings.bottomNavPageIds must be unique";
                    return false;
                }
            }
            seenNav[navCount++] = navId;
            normalizedBottomNav.add(navId);
        }
    }
    for (JsonObjectConst page : normalized["pages"].as<JsonArrayConst>()) {
        for (JsonObjectConst card : page["cards"].as<JsonArrayConst>()) {
            if (String(card["kind"] | "") != "page-link") continue;
            bool found = false;
            const String target = card["pageId"].as<String>();
            for (size_t i = 0; i < pageCount; ++i) found = found || pageIds[i] == target;
            if (!found) {
                error = String("page-link '") + (card["id"] | "") + "' has an invalid pageId";
                return false;
            }
        }
    }
    if (normalized.overflowed() || measureJson(normalized) > MAX_CONFIG_BYTES) {
        error = "Normalized panel configuration is too large";
        return false;
    }
    return true;
}

String ScreenPanelManager::ruleFingerprint(const PeriphExecRule& rule) {
    uint32_t hash = 2166136261UL;
    auto add = [&hash](const String& value) {
        for (size_t i = 0; i < value.length(); ++i) {
            hash ^= static_cast<uint8_t>(value.charAt(i));
            hash *= 16777619UL;
        }
        hash ^= 0xFF;
        hash *= 16777619UL;
    };
    add(rule.id);
    add(rule.allowPanelControl ? "1" : "0");
    for (const auto& action : rule.actions) {
        add(String(action.actionType));
        add(action.targetPeriphId);
        add(action.actionValue);
        add(action.useReceivedValue ? "1" : "0");
        add(String(action.syncDelayMs));
    }
    char buffer[9];
    snprintf(buffer, sizeof(buffer), "%08lx", static_cast<unsigned long>(hash));
    return String(buffer);
}

bool ScreenPanelManager::validateAndBindReferences(JsonDocument& normalized,
                                                   String& error) const {
    const char* displayRef = normalized["displayRef"] | "";
    if (displayRef[0]) {
        PeripheralConfig display;
        if (!PeripheralManager::getInstance().getPeripheralSnapshot(displayRef, display) ||
            display.type != PeripheralType::SCREEN_DISPLAY) {
            error = "displayRef does not reference a configured display";
            return false;
        }
    }

    for (JsonObject page : normalized["pages"].as<JsonArray>()) {
        for (JsonObject card : page["cards"].as<JsonArray>()) {
            const String kind = card["kind"] | "";
            auto validateSource = [&](const String& sourceRef) -> bool {
                if (knownSystemSource(sourceRef) || knownLocalSource(sourceRef)) return true;
#if FASTBEE_ENABLE_REMOTE_RESOURCES
                JsonDocument value;
                if (FastBee::RemoteResourceManager::instance().getValue(
                        sourceRef, value.to<JsonObject>())) return true;
#endif
                error = String("Unknown sourceRef '") + sourceRef + "'";
                return false;
            };
            if (!card["sourceRef"].isNull() &&
                !(kind == "weather" && String(card["sourceRef"] | "").isEmpty()) &&
                !validateSource(card["sourceRef"].as<String>())) return false;
            String seriesUnit;
            size_t seriesIndex = 0;
            for (JsonVariant source : card["sourceRefs"].as<JsonArray>()) {
                if (!validateSource(source.as<String>())) return false;
                String unit;
                if (!panelNumericSourceUnit(source.as<String>(), unit) ||
                    (seriesIndex && unit != seriesUnit)) {
                    error = "Bar sources must be numeric and use the same unit";
                    return false;
                }
                seriesUnit = unit;
                ++seriesIndex;
            }
            if (kind != "control") continue;

            String controlRef = card["controlRef"] | "";
            if (controlRef.isEmpty()) continue;  // Inert unbound template.
            if (String(card["visualType"] | "") == "motor") {
                if (!MotorControlService::instance().validReference(controlRef)) {
                    error = "Motor binding is unavailable or changed"; return false;
                }
                continue;
            }
            if (!controlRef.startsWith("rule.")) {
                error = "controlRef must reference an explicitly panel-enabled execution rule";
                return false;
            }
            const String ruleId = controlRef.substring(5);
            PeriphExecRule rule;
            if (!PeriphExecManager::getInstance().getRuleSnapshot(ruleId, rule)) {
                error = String("Unknown control rule '") + ruleId + "'";
                return false;
            }
            if (!rule.enabled || !panelSafeRule(rule)) {
                error = String("Control rule '") + ruleId + "' is not panel-safe";
                return false;
            }
            PanelNumericConstraints numeric;
            const PanelRuleInputKind inputKind = resolvePanelRuleInputKind(rule, &numeric);
            if (!validatePanelControlInput(inputKind,
                                           String(card["visualType"] | ""), error)) return false;
            if (inputKind == PanelRuleInputKind::NUMBER) {
                const uint8_t precision = card["precision"] | 0;
                const double minimum = card["rangeMin"].as<double>();
                const double maximum = card["rangeMax"].as<double>();
                if ((numeric.integerOnly && precision != 0) ||
                    (numeric.bounded &&
                     (minimum < numeric.minimum || maximum > numeric.maximum))) {
                    error = "Numeric control range or precision is incompatible with its rule";
                    return false;
                }
            }
            card["bindingFingerprint"] = ruleFingerprint(rule);
        }
    }
    return true;
}

ScreenPanelManager::SaveResult ScreenPanelManager::saveConfig(
    JsonVariantConst submitted, uint32_t expectedRevision, String& error) {
    if (!_initialized && !initialize()) {
        error = "Screen panel manager is unavailable";
        return SaveResult::STORAGE_ERROR;
    }

    JsonDocument normalized;
    if (!validateConfig(submitted, normalized, error) ||
        !validateAndBindReferences(normalized, error)) {
        return SaveResult::INVALID_CONFIG;
    }

    String serialized;
    serializeJson(normalized, serialized);
    const String nextDisplayRef = String(normalized["displayRef"] | "");
    bool displayChanged = false;
    {
        PanelLock lock(_mutex);
        if (!lock.locked()) {
            error = "Screen panel configuration is busy";
            return SaveResult::BUSY;
        }
        if (expectedRevision != _revision) {
            error = "Screen panel configuration revision conflict";
            return SaveResult::REVISION_CONFLICT;
        }
        uint32_t nextRevision = _revision + 1;
        if (nextRevision == 0) nextRevision = 1;
        // Keep the revision check, durable write and in-memory publication in
        // one critical section so concurrent editors cannot both commit the
        // same expected revision.
        if (!writeEnvelope(normalized.as<JsonVariantConst>(), nextRevision)) {
            error = "Failed to persist screen panel configuration";
            return SaveResult::STORAGE_ERROR;
        }
        _configJson = serialized;
        _revision = nextRevision;
        _persisted = true;
        displayChanged = _displayRef != nextDisplayRef;
        _displayRef = nextDisplayRef;
        _applied = false;
        _restartRequired = false;
        _applicationError = "";
        bool activeFound = false;
        for (JsonObjectConst page : normalized["pages"].as<JsonArrayConst>()) {
            activeFound = activeFound || _activePageId == String(page["id"] | "");
        }
        if (!activeFound) _activePageId = normalized["settings"]["homePageId"].as<String>();
    }
#if FASTBEE_ENABLE_SCREEN_UI
    auto& runtime = FastBee::Screen::ScreenRuntime::instance();
    if (displayChanged) {
        String stopError;
        const bool stopped = runtime.stop(&stopError);
        reportApplication(false, true,
                          stopped ? String("SCREEN_DISPLAY_RESTART_REQUIRED")
                                  : stopError);
    } else if (runtime.running()) {
        runtime.requestRefresh();
        reportApplication(true, false);
    }
#endif
    error = "";
    return SaveResult::SAVED;
}

bool ScreenPanelManager::queueSetting(const String& key, uint32_t value, String& error) {
    const bool valid = (key == "brightnessPercent" && value <= 100) ||
        (key == "idleSleepSeconds" && value <= 86400) ||
        (key == "touchBeepEnabled" && value <= 1) ||
        (key == "controlConfirmIdleSeconds" && value >= 15 && value <= 600);
    if (!valid) { error = "Invalid screen setting"; return false; }
#if defined(ARDUINO_ARCH_ESP32)
    struct PendingSettings {
        SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
        TaskHandle_t task = nullptr;
        uint32_t revision = 0, changedAt = 0;
        int brightness = -1, sleep = -1, beep = -1, confirmIdle = -1;
    };
    static PendingSettings pending;
    if (!pending.mutex || xSemaphoreTake(pending.mutex, 0) != pdTRUE) { error = "Screen settings busy"; return false; }
    if (pending.revision != revision()) { pending.brightness = -1; pending.sleep = -1; pending.beep = -1; pending.confirmIdle = -1; }
    pending.revision = revision(); pending.changedAt = millis();
    if (key == "brightnessPercent") pending.brightness = value;
    else if (key == "idleSleepSeconds") pending.sleep = value;
    else if (key == "touchBeepEnabled") pending.beep = value;
    else pending.confirmIdle = value;
    if (!pending.task) {
        auto save = [](void* arg) {
            auto& job = *static_cast<PendingSettings*>(arg);
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(100));
                if (xSemaphoreTake(job.mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
                if (millis() - job.changedAt < 2000U) { xSemaphoreGive(job.mutex); continue; }
                const int brightness = job.brightness, sleep = job.sleep, beep = job.beep, confirmIdle = job.confirmIdle;
                const uint32_t expected = job.revision;
                job.brightness = -1; job.sleep = -1; job.beep = -1; job.confirmIdle = -1;
                xSemaphoreGive(job.mutex);
                JsonDocument config; uint32_t actual; bool persisted; String saveError;
                auto& manager = ScreenPanelManager::instance();
                bool saved = manager.copyConfig(config, actual, persisted) && actual == expected;
                if (saved) {
                    if (brightness >= 0) config["settings"]["brightnessPercent"] = brightness;
                    if (sleep >= 0) config["settings"]["idleSleepSeconds"] = sleep;
                    if (beep >= 0) config["settings"]["touchBeepEnabled"] = beep != 0;
                    if (confirmIdle >= 0) config["settings"]["controlConfirmIdleSeconds"] = confirmIdle;
                    saved = manager.saveConfig(config.as<JsonVariantConst>(), expected, saveError) == SaveResult::SAVED;
                }
                if (!saved) manager.reportApplication(false, false, saveError.isEmpty() ? "SCREEN_SETTING_REVISION_CONFLICT" : saveError);
                xSemaphoreTake(job.mutex, portMAX_DELAY);
                if (job.brightness < 0 && job.sleep < 0 && job.beep < 0 && job.confirmIdle < 0) {
                    job.task = nullptr; xSemaphoreGive(job.mutex); vTaskDelete(nullptr); return;
                }
                // Only advance edits made while our own save was in flight.
                if (saved && job.revision == expected) job.revision = expected + 1;
                xSemaphoreGive(job.mutex);
            }
        };
        if (xTaskCreate(save, "screen_settings", 6144, &pending, 1, &pending.task) != pdPASS) {
            pending.task = nullptr; pending.brightness = -1; pending.sleep = -1; pending.beep = -1; pending.confirmIdle = -1;
            xSemaphoreGive(pending.mutex); error = "Screen settings worker unavailable"; return false;
        }
    }
    xSemaphoreGive(pending.mutex); error = ""; return true;
#else
    (void)key; (void)value; error = "Screen settings worker unavailable"; return false;
#endif
}

bool ScreenPanelManager::writeEnvelope(JsonVariantConst config,
                                       uint32_t revision) const {
    JsonDocument envelope;
    envelope["storageVersion"] = 1;
    envelope["touchUiMigration"] = 2;
    envelope["revision"] = revision;
    envelope["config"].set(config);
    if (envelope.overflowed() || measureJson(envelope) > MAX_CONFIG_BYTES) return false;

    const String path = CONFIG_FILE;
    const String tmp = path + ".tmp";
    const String backup = path + ".bak";
    if (!LittleFS.exists("/config") && !LittleFS.mkdir("/config")) return false;
    if (LittleFS.exists(tmp)) LittleFS.remove(tmp);
    File file = LittleFS.open(tmp, FILE_WRITE);
    if (!file) return false;
    const size_t expected = measureJson(envelope);
    const size_t written = serializeJson(envelope, file);
    file.flush();
    file.close();
    if (written != expected) {
        LittleFS.remove(tmp);
        return false;
    }
    File verifyFile = LittleFS.open(tmp, FILE_READ);
    JsonDocument verify;
    const bool valid = verifyFile && !deserializeJson(verify, verifyFile) &&
                       verify["storageVersion"] == 1;
    if (verifyFile) verifyFile.close();
    if (!valid) {
        LittleFS.remove(tmp);
        return false;
    }

    const bool hadPrimary = LittleFS.exists(path);
    if (LittleFS.exists(backup) && !LittleFS.remove(backup)) {
        LittleFS.remove(tmp);
        return false;
    }
    if (hadPrimary && !LittleFS.rename(path, backup)) {
        LittleFS.remove(tmp);
        return false;
    }
    if (LittleFS.rename(tmp, path)) return true;
    if (hadPrimary) LittleFS.rename(backup, path);
    LittleFS.remove(tmp);
    return false;
}

void ScreenPanelManager::appendCapabilities(JsonObject out) const {
    FastBee::Screen::ScreenSystemActionCapabilities actionCapabilities{};
#if FASTBEE_ENABLE_SCREEN_UI
    actionCapabilities =
        FastBee::Screen::ScreenRuntime::instance().systemActionCapabilities();
#endif
    out["schemaVersion"] = 1;
    out["maxPages"] = ScreenPanelSnapshot::MAX_PAGES;
    out["maxCards"] = ScreenPanelSnapshot::MAX_CARDS;
    out["maxHistorySamples"] = ScreenPanelSnapshot::MAX_HISTORY;
    out["maxSourcesPerCard"] = 4;
    out["templates"] = true;
    out["canvas"] = true;
    out["motorControls"] = FASTBEE_ENABLE_MODBUS != 0;
    out["touchSettings"] = true;
    out["charts"] = true;
    out["systemRestart"] = actionCapabilities.restart;
    out["brightness"] = actionCapabilities.brightness;
    out["brightnessContinuous"] = actionCapabilities.brightnessContinuous;
    out["minVisibleBrightnessPercent"] = actionCapabilities.minVisibleBrightnessPercent;
    out["sleep"] = actionCapabilities.sleep;
    out["touchWake"] = actionCapabilities.touchWake;
    out["buzzer"] = actionCapabilities.buzzer;
    out["touchBeep"] = actionCapabilities.buzzer;
    out["controlConfirmIdle"] = FASTBEE_ENABLE_SCREEN_UI != 0;
    out["maxBottomNavPages"] = ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES;
    JsonObject disabledReasons = out["disabledReasons"].to<JsonObject>();
    if (!actionCapabilities.brightness) disabledReasons["brightness"] = "当前硬件不支持亮度调节";
    if (actionCapabilities.brightness && !actionCapabilities.brightnessContinuous) disabledReasons["brightnessContinuous"] = "仅支持背光开关";
    if (!actionCapabilities.sleep) disabledReasons["sleep"] = "当前硬件不支持自动息屏";
    if (!actionCapabilities.touchWake) disabledReasons["touchWake"] = "当前硬件不支持触摸唤醒";
    if (!actionCapabilities.buzzer) disabledReasons["buzzer"] = "当前硬件不支持提示音";
    if (!(FASTBEE_ENABLE_MODBUS != 0)) disabledReasons["motorControls"] = "当前固件不支持电机控制";
    JsonArray systemActions = out["systemActions"].to<JsonArray>();
    if (actionCapabilities.brightness) systemActions.add("brightness");
    if (actionCapabilities.sleep) { systemActions.add("sleep"); systemActions.add("sleep-time"); }
    if (actionCapabilities.buzzer) systemActions.add("touch-beep");
    if (FASTBEE_ENABLE_SCREEN_UI) systemActions.add("confirm-idle");
    if (actionCapabilities.buzzer) systemActions.add("buzzer");
    if (actionCapabilities.restart) systemActions.add("restart");
    out["requiresUniqueCardIds"] = true;
    out["allowsOverlap"] = false;
}

void ScreenPanelManager::appendChoices(JsonArray out) const {
    FastBee::Screen::ScreenSystemActionCapabilities actionCapabilities{};
#if FASTBEE_ENABLE_SCREEN_UI
    actionCapabilities =
        FastBee::Screen::ScreenRuntime::instance().systemActionCapabilities();
#endif
    JsonDocument remoteChoices;
    std::vector<String> seenSources;
    std::vector<String> seenControls;
    seenSources.reserve(32);
    seenControls.reserve(16);

    auto seen = [](const std::vector<String>& values, const String& value) {
        for (const auto& candidate : values) {
            if (candidate == value) return true;
        }
        return false;
    };
    auto addSource = [&](const String& sourceRef, const String& label,
                         const String& type, const String& unit,
                         const char* group, bool available,
                         const String& reason = String()) {
        if (sourceRef.isEmpty() || seen(seenSources, sourceRef)) return;
        seenSources.push_back(sourceRef);
        JsonObject item = out.add<JsonObject>();
        item["id"] = sourceRef;
        item["sourceRef"] = sourceRef;
        item["label"] = label.isEmpty() ? sourceRef : label;
        item["kind"] = type == "weather" ? "weather" : "data";
        item["type"] = type;
        item["unit"] = unit;
        item["group"] = group;
        item["available"] = available;
        if (!reason.isEmpty()) item["reason"] = reason;
    };

    const auto observations = PeriphExecManager::getInstance().getObservationSnapshots();
    for (const auto& observation : observations) {
        if (!observation.bindingValid) continue;
        addSource(observation.sourceRef, observation.label,
                  observation.valueType, observation.unit, "local",
                  observation.quality != ObservationQuality::OFFLINE &&
                      observation.quality != ObservationQuality::ERROR,
                  observation.error);
    }

#if FASTBEE_ENABLE_REMOTE_RESOURCES
    JsonArray remoteSources = remoteChoices["sources"].to<JsonArray>();
    JsonArray ignoredControls = remoteChoices["controls"].to<JsonArray>();
    FastBee::RemoteResourceManager::instance().appendChoices(remoteSources,
                                                             ignoredControls);
    for (JsonObjectConst source : remoteSources) {
        addSource(String(source["sourceRef"] | ""),
                  String(source["label"] | ""),
                  String(source["type"] | "number"),
                  String(source["unit"] | ""),
                  source["group"] | "remote", true);
    }
#endif

    const auto rules = PeriphExecManager::getInstance().getAllRules();
    for (const auto& rule : rules) {
        if (!rule.enabled || !panelSafeRule(rule)) continue;
        const String controlRef = String("rule.") + rule.id;
        if (seen(seenControls, controlRef)) continue;
        seenControls.push_back(controlRef);
        PanelNumericConstraints numeric;
        const PanelRuleInputKind inputKind = panelRuleInputKind(
            rule, remoteChoices["controls"].as<JsonArrayConst>(), &numeric);
        JsonObject item = out.add<JsonObject>();
        item["id"] = controlRef;
        item["controlRef"] = controlRef;
        item["label"] = rule.name.isEmpty() ? rule.id : rule.name;
        item["kind"] = "control";
        item["type"] = inputKind == PanelRuleInputKind::BOOLEAN ? "switch" :
                       inputKind == PanelRuleInputKind::NUMBER ? "number" : "action";
        item["group"] = "rule";
        item["available"] = inputKind != PanelRuleInputKind::UNSUPPORTED;
        if (inputKind == PanelRuleInputKind::NUMBER) {
            item["precision"] = numeric.integerOnly ? 0 : 2;
            item["step"] = numeric.integerOnly ? 1 : 0.1;
            if (numeric.bounded) {
                item["rangeMin"] = numeric.minimum;
                item["rangeMax"] = numeric.maximum;
                item["initialValue"] = std::max(numeric.minimum,
                    std::min(numeric.maximum, 0.0));
            } else {
                item["rangeMin"] = 0;
                item["rangeMax"] = 100;
                item["initialValue"] = 0;
            }
        }
        if (inputKind == PanelRuleInputKind::UNSUPPORTED) {
            item["reason"] = "面板暂不支持该规则的输入值，请配置固定值执行动作";
        }
    }

    MotorControlService::instance().appendChoices(out);

    addSource("system.uptime", "System uptime", "number", "s", "system", true);
    addSource("system.freeHeap", "Free heap", "number", "B", "system", true);
    addSource("system.wifiRssi", "Wi-Fi RSSI", "number", "dBm", "system", true);
    addSource("system.network", "Network status", "status", "", "system", true);

    auto addSystemAction = [&](const char* id, const char* label,
                               bool confirmation, bool available,
                               int actionValue = -1, const char* visual = "action") {
        JsonObject item = out.add<JsonObject>();
        item["id"] = String("system.") + id;
        item["label"] = label;
        item["kind"] = "system";
        item["type"] = visual;
        item["visualType"] = visual;
        item["systemActionId"] = id;
        if (actionValue >= 0) item["actionValue"] = actionValue;
        item["requiresConfirmation"] = confirmation;
        item["available"] = available;
        if (!available) {
            item["reason"] =
                "Active display/touch backend does not support this action";
        }
    };

    addSystemAction("brightness", "Set screen brightness", false,
                    actionCapabilities.brightness, 70, "slider");
    addSystemAction("sleep-time", "自动息屏", false, actionCapabilities.sleep, -1, "select");
    addSystemAction("touch-beep", "触摸提示音", false, actionCapabilities.buzzer, -1, "switch");
    addSystemAction("confirm-idle", "操作确认间隔", false, FASTBEE_ENABLE_SCREEN_UI != 0, -1, "select");
    addSystemAction("sleep", "Turn screen off", false,
                    actionCapabilities.sleep);

    addSystemAction("restart", "Restart device", true,
                    actionCapabilities.restart);
}

void ScreenPanelManager::appendApplication(JsonObject out) const {
    {
        PanelLock lock(_mutex);
        if (!lock.locked()) return;
        out["persisted"] = _persisted;
        out["applied"] = _applied;
        out["restartRequired"] = _restartRequired;
        out["revision"] = _revision;
        out["status"] = !_persisted ? "draft" : _applied ? "applied" :
                        _applicationError.isEmpty() ? "saved" : "apply_failed";
        if (!_applicationError.isEmpty()) out["error"] = _applicationError;
    }
#if FASTBEE_ENABLE_SCREEN_UI
    auto& runtime = FastBee::Screen::ScreenRuntime::instance();
    JsonObject state = out["runtime"].to<JsonObject>();
    state["running"] = runtime.running();
    state["sleeping"] = runtime.sleeping();
    state["allocatedBytes"] = runtime.allocatedBytes();
    String runtimeError;
    runtime.getLastError(runtimeError);
    if (!runtimeError.isEmpty()) state["error"] = runtimeError;
    state["internalFreeBytes"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    state["internalLargestBytes"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    state["psramFreeBytes"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

void ScreenPanelManager::reportApplication(bool applied, bool restartRequired,
                                           const String& error) {
    PanelLock lock(_mutex);
    if (!lock.locked()) return;
    _applied = applied;
    _restartRequired = restartRequired;
    _applicationError = error;
}

void ScreenPanelManager::appendState(JsonObject out) const {
    std::unique_ptr<ScreenPanelSnapshot> snapshot(
        new (std::nothrow) ScreenPanelSnapshot());
    if (!snapshot || !getSnapshot(*snapshot)) {
        out["available"] = false;
        return;
    }
    out["available"] = true;
    out["revision"] = snapshot->revision;
    out["activePageIndex"] = snapshot->activePageIndex;
    out["brightnessPercent"] = snapshot->brightnessPercent;
    out["idleSleepSeconds"] = snapshot->idleSleepSeconds;
    out["touchBeepEnabled"] = snapshot->touchBeepEnabled;
    out["controlConfirmIdleSeconds"] = snapshot->controlConfirmIdleSeconds;
    appendPanelClock(out["clock"].to<JsonObject>());
    JsonObject items = out["items"].to<JsonObject>();
    JsonArray cards = out["cards"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot->cardCount; ++i) {
        const auto& card = snapshot->cards[i];
        JsonObject item = cards.add<JsonObject>();
        // const char[N] is treated as a static literal by ArduinoJson 7.4.
        // This snapshot dies before the response is serialized, so copy text.
        item["id"] = JsonString(card.id);
        item["sourceRef"] = JsonString(card.sourceRef);
        item["controlRef"] = JsonString(card.controlRef);
        item["value"] = JsonString(card.value);
        item["unit"] = JsonString(card.unit);
        item["quality"] = qualityName(card.quality);
        item["error"] = JsonString(card.error);
        item["updatedMs"] = card.updatedMs;
        item["pending"] = card.pending;
        item["operationId"] = card.operationId;
        item["motorSpeed"] = card.motorSpeed; item["motorPulse"] = card.motorPulse;
        item["motorPrecision"] = card.motorPrecision;
        item["motorKnown"] = card.motorKnown; item["systemActionId"] = JsonString(card.systemActionId);
        item["historyCount"] = card.historyCount;
        item["historyStale"] = card.historyStale;
        item["historyError"] = JsonString(card.historyError);
        item["seriesCount"] = card.seriesCount;

        const char* stateRef = card.controlRef[0] ? card.controlRef : card.sourceRef;
        if (!stateRef[0]) continue;
        JsonObject state = items[stateRef].to<JsonObject>();
        state["cardId"] = JsonString(card.id);
        state["motorSpeed"] = card.motorSpeed; state["motorPulse"] = card.motorPulse;
        state["motorPrecision"] = card.motorPrecision;
        state["motorKnown"] = card.motorKnown;
        state["value"] = JsonString(card.value);
        state["unit"] = JsonString(card.unit);
        state["quality"] = qualityName(card.quality);
        state["error"] = JsonString(card.error);
        state["updatedMs"] = card.updatedMs;
        state["pending"] = card.pending;
        state["operationId"] = card.operationId;
        state["historyStale"] = card.historyStale;
        if (card.historyError[0]) state["historyError"] = JsonString(card.historyError);
        if (card.weatherCondition[0]) {
            state["condition"] = JsonString(card.weatherCondition);
            state["city"] = JsonString(card.weatherCity);
            if (card.hasWeatherTemperature) {
                state["temperature"] = card.weatherTemperature;
            }
        }
        if (card.seriesCount) {
            JsonArray series = state["series"].to<JsonArray>();
            for (uint8_t seriesIndex = 0; seriesIndex < card.seriesCount;
                 ++seriesIndex) {
                const auto& source = card.series[seriesIndex];
                JsonObject seriesItem = series.add<JsonObject>();
                seriesItem["sourceRef"] = JsonString(source.sourceRef);
                seriesItem["label"] = JsonString(source.label);
                seriesItem["unit"] = JsonString(source.unit);
                seriesItem["value"] = JsonString(source.value);
                seriesItem["quality"] = qualityName(source.quality);
                seriesItem["updatedMs"] = source.updatedMs;
            }
            // Per-card series remain distinct when two charts share a primary
            // source but compare different additional sources.
            item["series"] = series;
        }
    }
}

namespace {

#if defined(ARDUINO_ARCH_ESP32) && FASTBEE_USE_PSRAM
class PanelSnapshotJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t bytes) override {
        return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* memory) override { heap_caps_free(memory); }
    void* reallocate(void* memory, size_t bytes) override {
        return heap_caps_realloc(memory, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};
#endif

JsonDocument makePanelSnapshotDocument(bool persistent) {
#if defined(ARDUINO_ARCH_ESP32) && FASTBEE_USE_PSRAM
    static PanelSnapshotJsonAllocator allocator;
    if (persistent) return JsonDocument(&allocator);
#else
    (void)persistent;
#endif
    return JsonDocument();
}

template <typename T>
void updatePanelSnapshotField(T& destination, const T& value) {
    // The destination normally lives in PSRAM. Comparing before copying keeps
    // unchanged cards from generating continuous external-memory writebacks.
    if (memcmp(&destination, &value, sizeof(T)) != 0) {
        memcpy(&destination, &value, sizeof(T));
    }
}

void updatePanelDynamicFields(ScreenPanelCardSnapshot& out,
                              const ScreenPanelCardSnapshot& fresh) {
    updatePanelSnapshotField(out.motorSpeed, fresh.motorSpeed);
    updatePanelSnapshotField(out.motorPrecision, fresh.motorPrecision);
    updatePanelSnapshotField(out.motorPulse, fresh.motorPulse);
    updatePanelSnapshotField(out.motorKnown, fresh.motorKnown);
    updatePanelSnapshotField(out.unit, fresh.unit);
    updatePanelSnapshotField(out.value, fresh.value);
    updatePanelSnapshotField(out.error, fresh.error);
    updatePanelSnapshotField(out.historyError, fresh.historyError);
    updatePanelSnapshotField(out.weatherCondition, fresh.weatherCondition);
    updatePanelSnapshotField(out.weatherCity, fresh.weatherCity);
    updatePanelSnapshotField(out.quality, fresh.quality);
    updatePanelSnapshotField(out.weatherTemperature, fresh.weatherTemperature);
    updatePanelSnapshotField(out.hasWeatherTemperature, fresh.hasWeatherTemperature);
    updatePanelSnapshotField(out.pending, fresh.pending);
    updatePanelSnapshotField(out.historyStale, fresh.historyStale);
    updatePanelSnapshotField(out.updatedMs, fresh.updatedMs);
    updatePanelSnapshotField(out.operationId, fresh.operationId);
    updatePanelSnapshotField(out.historyCount, fresh.historyCount);
    updatePanelSnapshotField(out.seriesCount, fresh.seriesCount);
    updatePanelSnapshotField(out.history, fresh.history);
    updatePanelSnapshotField(out.series, fresh.series);
}

}  // namespace

ScreenPanelSnapshotCache::ScreenPanelSnapshotCache(bool persistent)
    : _layout(makePanelSnapshotDocument(persistent)) {}

bool ScreenPanelManager::getSnapshot(ScreenPanelSnapshot& out) const {
    // Keep the Web endpoint's existing transient allocation/fallback policy.
    ScreenPanelSnapshotCache cache(false);
    return getSnapshotImpl(out, cache, false);
}

bool ScreenPanelManager::getActiveSnapshot(ScreenPanelSnapshot& out,
                                          ScreenPanelSnapshotCache& cache) const {
    return getSnapshotImpl(out, cache, true);
}

bool ScreenPanelManager::getSnapshotImpl(ScreenPanelSnapshot& out,
                                        ScreenPanelSnapshotCache& cache,
                                        bool activeOnly) const {
    String configJson;
    String activePage;
    uint32_t revision = 1;
    std::array<uint32_t, ScreenPanelSnapshot::MAX_CARDS> operationIds{};
    std::array<String, ScreenPanelSnapshot::MAX_CARDS> operationCards{};
    bool restartSaveFailed = false;
    bool rebuild = false;
    {
        PanelLock lock(_mutex);
        if (!lock.locked() || !_initialized) return false;
        rebuild = cache._snapshot != &out || cache._revision != _revision;
        if (rebuild) configJson = _configJson;
        activePage = _activePageId;
        revision = _revision;
        operationIds = _operationIds;
        operationCards = _operationCardIds;
        restartSaveFailed = _restartSaveFailed;
    }

    if (rebuild) {
        cache.invalidate();
        if (deserializeJson(cache._layout, configJson)) return false;
        // This 50KB initialization belongs to layout changes, never to the
        // screen's periodic data refresh. Removed pages/cards are cleared too.
        memset(&out, 0, sizeof(out));
        out.revision = revision;
        out.brightnessPercent = cache._layout["settings"]["brightnessPercent"] | 70;
        out.idleSleepSeconds = cache._layout["settings"]["idleSleepSeconds"] | 120;
        out.touchBeepEnabled = cache._layout["settings"]["touchBeepEnabled"] | true;
        out.controlConfirmIdleSeconds = cache._layout["settings"]["controlConfirmIdleSeconds"] | 60;
        copyText(out.homePageId, String(cache._layout["settings"]["homePageId"] | "home"));
        copyText(out.themeId, String(cache._layout["settings"]["themeId"] | "dark"));
        copyText(out.backgroundPresetId,
                 String(cache._layout["settings"]["background"]["presetId"] | "solid"));
        out.bottomNavCount = 0;
        for (JsonVariantConst nav :
             cache._layout["settings"]["bottomNavPageIds"].as<JsonArrayConst>()) {
            if (out.bottomNavCount >= ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES) break;
            copyText(out.bottomNavPageIds[out.bottomNavCount++], String(nav | ""));
        }
        if (out.bottomNavCount == 0) {
            for (JsonObjectConst page : cache._layout["pages"].as<JsonArrayConst>()) {
                if (out.bottomNavCount >= ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES) break;
                copyText(out.bottomNavPageIds[out.bottomNavCount++], String(page["id"] | ""));
            }
        }
        cache._terminalOperations = {};
    }
    const JsonObjectConst config = cache._layout.as<JsonObjectConst>();
    out.activePageIndex = 0;
    uint8_t activeIndex = 0;
    for (JsonObjectConst page : config["pages"].as<JsonArrayConst>()) {
        if (activeIndex >= ScreenPanelSnapshot::MAX_PAGES) break;
        if (activePage == (page["id"] | "")) {
            out.activePageIndex = activeIndex;
            break;
        }
        ++activeIndex;
    }

    // Action IDs are retained after completion. A persistent screen cache
    // keeps terminal outcomes, so idle screens no longer copy the result list.
    bool needsRecentOperations = false;
    uint8_t operationPageIndex = 0;
    for (JsonObjectConst page : config["pages"].as<JsonArrayConst>()) {
        if (!activeOnly || operationPageIndex == out.activePageIndex) {
            for (JsonObjectConst card : page["cards"].as<JsonArrayConst>()) {
                const char* cardId = card["id"] | "";
                for (size_t slot = 0; slot < operationIds.size(); ++slot) {
                    const uint32_t id = operationIds[slot];
                    if (id && !(id & 0x80000000UL) &&
                        operationCards[slot] == cardId &&
                        cache._terminalOperations[slot].id != id) {
                        needsRecentOperations = true;
                        break;
                    }
                }
                if (needsRecentOperations) break;
            }
        }
        if (needsRecentOperations) break;
        ++operationPageIndex;
    }
    std::vector<AsyncExecResult> recentOperations;
    if (needsRecentOperations) {
        recentOperations = PeriphExecManager::getInstance().getRecentResults();
        for (size_t slot = 0; slot < operationIds.size(); ++slot) {
            if (!operationIds[slot]) continue;
            for (const auto& result : recentOperations) {
                if (result.operationId != operationIds[slot] ||
                    !panelAsyncExecStatusIsTerminal(result.status)) continue;
                auto& terminal = cache._terminalOperations[slot];
                terminal.id = result.operationId;
                terminal.failed = result.status != AsyncExecStatus::COMPLETED;
                terminal.error = static_cast<uint8_t>(result.errorCode);
                break;
            }
        }
    }
    cache._refreshedCardCount = 0;
    const auto observations = panelObservationSnapshots(config["pages"].as<JsonArrayConst>(),
        out.activePageIndex, activeOnly);
    auto findObservation = [&](const String& source) -> const ObservationSnapshot* {
        for (const auto& observation : observations) {
            if (observation.sourceRef == source) return &observation;
        }
        return nullptr;
    };
    uint8_t pageIndex = 0;
    uint8_t cardIndex = 0;
    for (JsonObjectConst page : config["pages"].as<JsonArrayConst>()) {
        if (pageIndex >= ScreenPanelSnapshot::MAX_PAGES) break;
        auto& pageOut = out.pages[pageIndex];
        if (rebuild) {
            copyText(pageOut.id, String(page["id"] | ""));
            copyText(pageOut.title, String(page["title"] | ""));
            copyText(pageOut.layoutId, String(page["layoutId"] | "mixed-grid"));
            copyText(pageOut.themeId,
                     page["themeId"].is<const char*>()
                         ? page["themeId"].as<String>() : String(out.themeId));
            copyText(pageOut.backgroundPresetId,
                     page["background"]["presetId"].is<const char*>()
                         ? page["background"]["presetId"].as<String>()
                         : String(out.backgroundPresetId));
            pageOut.logicalWidth = page["logicalWidth"] | 480;
            pageOut.logicalHeight = page["logicalHeight"] | 480;
            pageOut.rotation = page["rotation"] | 0;
            pageOut.firstCard = cardIndex;
        }

        for (JsonObjectConst card : page["cards"].as<JsonArrayConst>()) {
            if (cardIndex >= ScreenPanelSnapshot::MAX_CARDS) break;
            auto& cardOut = out.cards[cardIndex];
            if (rebuild) {
                copyText(cardOut.id, String(card["id"] | ""));
                copyText(cardOut.pageId, String(page["id"] | ""));
                copyText(cardOut.targetPageId, String(card["pageId"] | ""));
                copyText(cardOut.title, String(card["title"] | ""));
                copyText(cardOut.unit, String(card["unit"] | ""));
                copyText(cardOut.sourceRef, String(card["sourceRef"] | ""));
                copyText(cardOut.controlRef, String(card["controlRef"] | ""));
                copyText(cardOut.systemActionId, String(card["systemActionId"] | ""));
                cardOut.motorSpeedInput = card["motorSpeed"] | 50;
                cardOut.motorPulseInput = card["motorPulse"] | 1600;
                cardOut.motorSpeedStep = card["motorSpeedStep"] | 1;
                cardOut.motorPulseStep = card["motorPulseStep"] | 100;
                copyText(cardOut.iconId, String(card["iconId"] | ""));
                parseKind(String(card["kind"] | "data"), cardOut.kind);
                parseVisual(String(card["visualType"] | "value"), cardOut.visualType);
                cardOut.precision = card["precision"] | 1;
                cardOut.rect.x = card["rect"]["x"] | 0;
                cardOut.rect.y = card["rect"]["y"] | 0;
                cardOut.rect.w = card["rect"]["w"] | 0;
                cardOut.rect.h = card["rect"]["h"] | 0;
                if (cardOut.visualType == ScreenVisualType::SLIDER) cardOut.numericValue = out.brightnessPercent;
                if (cardOut.visualType == ScreenVisualType::SELECT) cardOut.numericValue =
                    String(cardOut.systemActionId) == "confirm-idle" ? out.controlConfirmIdleSeconds : out.idleSleepSeconds;
                if (String(cardOut.systemActionId) == "touch-beep") cardOut.numericValue = out.touchBeepEnabled ? 1 : 0;
                cardOut.requiresConfirmation = card["requireConfirmation"] | false;
                cardOut.interactive = (cardOut.kind == ScreenCardKind::CONTROL && cardOut.controlRef[0]) ||
                                      cardOut.kind == ScreenCardKind::SYSTEM ||
                                      cardOut.kind == ScreenCardKind::PAGE_LINK;
                if (!card["rangeMin"].isNull()) {
                    cardOut.hasRange = true;
                    cardOut.rangeMin = card["rangeMin"].as<float>();
                    cardOut.rangeMax = card["rangeMax"].as<float>();
                }
                if (cardOut.visualType == ScreenVisualType::NUMBER_CONTROL) {
                    cardOut.numericStep = card["step"].as<float>();
                    cardOut.numericValue = card["initialValue"].as<float>();
                }
            }

            if (!activeOnly || pageIndex == out.activePageIndex) {
                // A single card-sized temporary stays on the worker's internal
                // stack. Zeroing it resets stale/error/history state without a
                // bulk PSRAM clear; only changed dynamic fields are written back.
                ScreenPanelCardSnapshot fresh{};
                auto& cardOut = fresh;
                cardOut.kind = out.cards[cardIndex].kind;
                copyText(cardOut.unit, String(card["unit"] | ""));
                if ((cardOut.kind == ScreenCardKind::WEATHER &&
                     !(card["sourceRef"] | "")[0]) ||
                    (cardOut.kind == ScreenCardKind::CONTROL &&
                     !(card["controlRef"] | "")[0])) {
                    copyText(cardOut.error, String("binding_unconfigured"));
                }
                const String sourceRef = card["sourceRef"] | "";
                bool populated = fillPanelSystemSource(sourceRef, cardOut);
    #if FASTBEE_ENABLE_REMOTE_RESOURCES
                if (sourceRef.startsWith("remote.") || sourceRef.startsWith("weather.")) {
                    JsonDocument value;
                    JsonObject valueObject = value.to<JsonObject>();
                    if (FastBee::RemoteResourceManager::instance().getValue(sourceRef, valueObject)) {
                        cardOut.quality = qualityFromText(String(valueObject["quality"] | "unknown"));
                        cardOut.pending = valueObject["pending"] | false;
                        cardOut.updatedMs = valueObject["updatedMs"] | 0;
                        copyText(cardOut.error, String(valueObject["error"] | ""));
                        if (!valueObject["value"].isNull()) {
                            copyText(cardOut.value, jsonScalar(valueObject["value"]));
                        }
                        if (!valueObject["unit"].isNull()) {
                            copyText(cardOut.unit, String(valueObject["unit"] | ""));
                        }
                        JsonObjectConst weather = valueObject["weather"].as<JsonObjectConst>();
                        if (!weather.isNull()) {
                            copyText(cardOut.weatherCondition, String(weather["condition"] | "unknown"));
                            copyText(cardOut.weatherCity, String(weather["city"] | ""));
                            if (weather["temperature"].is<double>()) {
                                cardOut.weatherTemperature = weather["temperature"].as<float>();
                                cardOut.hasWeatherTemperature = true;
                                copyText(cardOut.value, String(cardOut.weatherTemperature, 1));
                            }
                        }
                        populated = true;
                    }
                }
    #endif
                if (!populated && !sourceRef.isEmpty()) {
                    if (const auto* sampled = findObservation(sourceRef)) {
                        const auto& observation = *sampled;
                        copyText(cardOut.value, observation.value);
                        if (String(cardOut.unit).isEmpty()) copyText(cardOut.unit, observation.unit);
                        copyText(cardOut.error, observation.error);
                        cardOut.quality = qualityFromObservation(observation.quality);
                        cardOut.updatedMs = observation.sampleTimestamp;
                        populated = true;
                    }
                }
                if (cardOut.kind == ScreenCardKind::CLOCK) cardOut.quality = ScreenValueQuality::GOOD;
                if (cardOut.kind == ScreenCardKind::SYSTEM && sourceRef.isEmpty()) {
                    cardOut.quality = ScreenValueQuality::GOOD;
                    const String actionId = card["systemActionId"] | "";
                    if (actionId == "touch-beep") copyText(cardOut.value, out.touchBeepEnabled ? "true" : "false");
                    else if (actionId == "confirm-idle") copyText(cardOut.value, String(out.controlConfirmIdleSeconds));
                    else if (actionId == "sleep-time") copyText(cardOut.value, String(out.idleSleepSeconds));
                }

                if (out.cards[cardIndex].visualType == ScreenVisualType::MOTOR_CONTROL) {
                    MotorControlState motor;
                    if (MotorControlService::instance().state(String(card["controlRef"] | ""), motor)) {
                        cardOut.motorKnown = motor.known; cardOut.motorSpeed = motor.speed; cardOut.motorPulse = motor.pulse;
                        cardOut.motorPrecision = motor.precision;
                        cardOut.pending = motor.pending; cardOut.updatedMs = motor.updatedMs;
                        cardOut.quality = motor.known ? ScreenValueQuality::GOOD : ScreenValueQuality::UNKNOWN;
                        copyText(cardOut.error, String(motor.error)); copyText(cardOut.value, String(motor.lastAction));
                    }
                }
                for (JsonVariantConst seriesSource :
                     card["sourceRefs"].as<JsonArrayConst>()) {
                    if (cardOut.seriesCount >= cardOut.series.size()) break;
                    const String seriesRef = seriesSource.as<String>();
                    auto& seriesOut = cardOut.series[cardOut.seriesCount++];
                    copyText(seriesOut.sourceRef, seriesRef);
                    copyText(seriesOut.label, seriesRef);

                    bool seriesPopulated = fillPanelSystemSource(seriesRef, seriesOut);
    #if FASTBEE_ENABLE_REMOTE_RESOURCES
                    if (seriesRef.startsWith("remote.") ||
                        seriesRef.startsWith("weather.")) {
                        JsonDocument value;
                        JsonObject valueObject = value.to<JsonObject>();
                        if (FastBee::RemoteResourceManager::instance().getValue(
                                seriesRef, valueObject)) {
                            copyText(seriesOut.label,
                                     String(valueObject["label"] | seriesRef.c_str()));
                            copyText(seriesOut.value, jsonScalar(valueObject["value"]));
                            copyText(seriesOut.unit, String(valueObject["unit"] | ""));
                            seriesOut.quality = qualityFromText(
                                String(valueObject["quality"] | "unknown"));
                            seriesOut.updatedMs = valueObject["updatedMs"] | 0;
                            seriesPopulated = true;
                        }
                    }
    #endif
                    if (!seriesPopulated) {
                        if (const auto* sampled = findObservation(seriesRef)) {
                            const auto& observation = *sampled;
                            copyText(seriesOut.label, observation.label);
                            copyText(seriesOut.value, observation.value);
                            copyText(seriesOut.unit, observation.unit);
                            seriesOut.quality = qualityFromObservation(observation.quality);
                            seriesOut.updatedMs = observation.sampleTimestamp;
                        }
                    }
                }

    #if FASTBEE_ENABLE_POINT_HISTORY
                if (!sourceRef.isEmpty() && String(card["historyMode"] | "none") == "recent10") {
                    FastBee::PointHistorySeriesSnapshot history;
                    if (FastBee::PointHistory::instance().getSeries(sourceRef, history)) {
                        cardOut.historyCount = history.count;
                        cardOut.historyStale = history.stale;
                        copyText(cardOut.historyError, history.staleReason);
                        for (uint8_t i = 0; i < history.count; ++i) {
                            cardOut.history[i].value = history.samples[i].value;
                            cardOut.history[i].sequence = history.samples[i].sequence;
                            cardOut.history[i].uptimeMs = history.samples[i].uptimeMs;
                            cardOut.history[i].epochMs = history.samples[i].epochMs;
                            cardOut.history[i].restored = history.samples[i].restored;
                        }
                    }
                }
    #endif

                for (size_t opIndex = 0; opIndex < operationCards.size(); ++opIndex) {
                    if (operationCards[opIndex] != (card["id"] | "") || operationIds[opIndex] == 0) continue;
                    cardOut.operationId = operationIds[opIndex];
                    cardOut.pending = true;
                    if ((cardOut.operationId & 0x80000000UL) != 0) {
                        cardOut.pending = SystemRebooter::isScheduled();
                        if (!cardOut.pending) {
                            copyText(cardOut.error,
                                     restartSaveFailed
                                         ? String("history_save_failed")
                                         : String("Restart was cancelled"));
                        }
                        break;
                    }
                    const auto& terminal = cache._terminalOperations[opIndex];
                    if (terminal.id == cardOut.operationId) {
                        cardOut.pending = false;
                        if (terminal.failed) {
                            snprintf(cardOut.error, sizeof(cardOut.error), "%s",
                                     panelAsyncExecErrorName(static_cast<AsyncExecError>(terminal.error)));
                        }
                        break;
                    }
                    for (const auto& operation : recentOperations) {
                        if (operation.operationId != cardOut.operationId) continue;
                        cardOut.pending = !panelAsyncExecStatusIsTerminal(operation.status);
                        if (!cardOut.pending && operation.status != AsyncExecStatus::COMPLETED) {
                            snprintf(cardOut.error, sizeof(cardOut.error), "%s",
                                     panelAsyncExecErrorName(operation.errorCode));
                        }
                        break;
                    }
                    break;
                }
                updatePanelDynamicFields(out.cards[cardIndex], fresh);
                ++cache._refreshedCardCount;
            }
            ++cardIndex;
            if (rebuild) ++pageOut.cardCount;
        }
        ++pageIndex;
    }
    out.pageCount = pageIndex;
    out.cardCount = cardIndex;
    if (rebuild) {
        cache._snapshot = &out;
        cache._revision = revision;
        ++cache._rebuildCount;
    }
    return true;
}

bool ScreenPanelManager::dispatchAction(const char* cardId, ScreenActionKind kind,
                                        bool target, uint32_t& operationId,
                                        String& error) {
    const String value = kind == ScreenActionKind::SET_BOOLEAN
        ? String(target ? "on" : "off") :
        (kind == ScreenActionKind::SYSTEM_RESTART ||
         kind == ScreenActionKind::SYSTEM_ACTION)
            ? String(target ? "true" : "false") : String();
    return dispatchAction(cardId, kind, value, operationId, error);
}

bool ScreenPanelManager::dispatchAction(const char* cardId, ScreenActionKind kind,
                                        const String& value, uint32_t& operationId,
                                        String& error, const char* expectedControlRef) {
    operationId = 0;
    error = "";
    if (!cardId || !validIdentifier(String(cardId))) {
        error = "Invalid card ID";
        return false;
    }
    JsonDocument config;
    uint32_t revision = 0;
    bool persisted = false;
    if (!copyConfig(config, revision, persisted) || !persisted) {
        error = "Screen panel configuration is not saved";
        return false;
    }

    JsonObjectConst selected;
    for (JsonObjectConst page : config["pages"].as<JsonArrayConst>()) {
        for (JsonObjectConst card : page["cards"].as<JsonArrayConst>()) {
            if (String(card["id"] | "") == cardId) {
                selected = card;
                break;
            }
        }
        if (selected) break;
    }
    if (!selected) {
        error = "Card is not present in the saved configuration";
        return false;
    }

    if (kind == ScreenActionKind::MOTOR_COMMAND) {
        if (String(selected["kind"] | "") != "control" || String(selected["visualType"] | "") != "motor") {
            error = "Card is not a motor control"; return false;
        }
        // Bind the command to what the user actually saw/confirmed. Comparing
        // against this copied config closes the UI refresh/rebinding window.
        if (expectedControlRef && (!expectedControlRef[0] ||
            strcmp(expectedControlRef, selected["controlRef"] | "") != 0)) {
            error = "Motor binding changed; refresh and confirm again"; return false;
        }
        String action = value; int parameter = 0;
        const int colon = action.indexOf(':');
        if (colon >= 0) {
            String number = action.substring(colon + 1); double parsed = 0;
            if (!canonicalPanelNumber(number, parsed) || parsed < 0 || parsed > 65535 || std::floor(parsed) != parsed) {
                error = "Invalid motor parameter"; return false;
            }
            parameter = static_cast<int>(parsed); action = action.substring(0, colon);
        }
        static uint32_t requestSequence = 0;
        const String requestId = "panel-" + String(millis()) + "-" + String(++requestSequence);
        return MotorControlService::instance().submit(String(selected["controlRef"] | ""), action, parameter, requestId, operationId, error);
    }
    const String cardKind = selected["kind"] | "";
    const bool target = value == "true" || value == "on" || value == "1";
    if (cardKind == "system") {
        const String systemActionId = selected["systemActionId"] | "";
        if (systemActionId != "restart") {
            if (kind != ScreenActionKind::SYSTEM_ACTION) {
                error = "Action kind does not match the saved system card";
                return false;
            }
#if FASTBEE_ENABLE_SCREEN_UI
            if (systemActionId == "touch-beep" || systemActionId == "confirm-idle" || systemActionId == "sleep-time") {
                const auto capabilities = FastBee::Screen::ScreenRuntime::instance().systemActionCapabilities();
                if ((systemActionId == "touch-beep" && !capabilities.buzzer) ||
                    (systemActionId == "sleep-time" && !capabilities.sleep)) {
                    error = "Active display/touch backend does not support this setting"; return false;
                }
                uint32_t settingValue = 0;
                if (systemActionId == "touch-beep") {
                    if (!oneOf(value, {"true", "false", "on", "off", "1", "0"})) {
                        error = "Touch beep setting requires a boolean value"; return false;
                    }
                    settingValue = target ? 1 : 0;
                } else {
                    double number = 0;
                    if (!canonicalPanelNumber(value, number) || number < 0 || number > 86400 || std::floor(number) != number) {
                        error = "Screen setting requires an integer value"; return false;
                    }
                    settingValue = static_cast<uint32_t>(number);
                }
                return queueSetting(systemActionId == "touch-beep" ? "touchBeepEnabled" :
                    systemActionId == "confirm-idle" ? "controlConfirmIdleSeconds" : "idleSleepSeconds", settingValue, error);
            }
            FastBee::Screen::ScreenSystemAction runtimeAction =
                FastBee::Screen::ScreenSystemAction::NONE;
            uint8_t actionValue = 0;
            if (systemActionId == "brightness") {
                runtimeAction = FastBee::Screen::ScreenSystemAction::SET_BRIGHTNESS;
                actionValue = selected["actionValue"] | 70;
            } else if (systemActionId == "sleep") {
                runtimeAction = FastBee::Screen::ScreenSystemAction::SLEEP;
            } else if (systemActionId == "buzzer") {
                runtimeAction = FastBee::Screen::ScreenSystemAction::BUZZER;
            } else {
                error = "Saved system action is unsupported";
                return false;
            }
            if (!FastBee::Screen::ScreenRuntime::instance().requestSystemAction(
                    runtimeAction, actionValue, error)) {
                return false;
            }
            operationId = 0x80000000UL | (millis() & 0x7fffffffUL);
            if (operationId == 0x80000000UL) ++operationId;
            PanelLock lock(_mutex);
            if (lock.locked()) {
                size_t slot = _operationCardIds.size();
                for (size_t i = 0; i < _operationCardIds.size(); ++i) {
                    if (_operationCardIds[i] == cardId ||
                        _operationCardIds[i].isEmpty()) {
                        slot = i;
                        break;
                    }
                }
                if (slot == _operationCardIds.size()) {
                    slot = operationId % _operationCardIds.size();
                }
                _operationCardIds[slot] = cardId;
                _operationIds[slot] = operationId;
            }
            return true;
#else
            error = "Screen UI is not compiled";
            return false;
#endif
        }
        if ((kind != ScreenActionKind::SYSTEM_ACTION &&
             kind != ScreenActionKind::SYSTEM_RESTART) ||
            !(config["settings"]["allowSystemRestart"] | false) ||
            !(selected["requireConfirmation"] | false)) {
            error = "System restart is not permitted for this card";
            return false;
        }
        if (SystemRebooter::isScheduled()) {
            error = "A restart is already scheduled";
            return false;
        }
#if FASTBEE_ENABLE_OTA
        auto* framework = FastBeeFramework::getInstance();
        auto* ota = framework ? framework->getOTAManager() : nullptr;
        if (ota && ota->isOTAInProgress()) {
            error = "OTA is in progress; restart is unavailable";
            return false;
        }
#endif
        {
            PanelLock lock(_mutex);
            if (!lock.locked()) {
                error = "Screen panel operation state is busy";
                return false;
            }
            if (target && !_restartSaveFailed) {
                error = "Forced restart requires a previous history save failure";
                return false;
            }
            _restartWithoutHistorySave = target;
            _restartSaveFailed = false;
        }
        operationId = 0x80000000UL | (millis() & 0x7fffffffUL);
        if (operationId == 0x80000000UL) ++operationId;
        {
            PanelLock lock(_mutex);
            if (!lock.locked()) {
                error = "Screen panel operation state is busy";
                operationId = 0;
                return false;
            }
            size_t slot = _operationCardIds.size();
            for (size_t i = 0; i < _operationCardIds.size(); ++i) {
                if (_operationCardIds[i] == cardId ||
                    _operationCardIds[i].isEmpty()) {
                    slot = i;
                    break;
                }
            }
            if (slot == _operationCardIds.size()) {
                slot = operationId % _operationCardIds.size();
            }
            _operationCardIds[slot] = cardId;
            _operationIds[slot] = operationId;
        }
        SystemRebooter::scheduleReboot(
            target ? "Panel restart without history" : "Screen panel restart", 2000,
                                      RestartReason::USER_COMMAND);
        return true;
    }

    if (cardKind != "control") {
        error = "Card is not executable";
        return false;
    }
    const String visualType = selected["visualType"] | "";
    if ((kind == ScreenActionKind::SET_BOOLEAN && visualType != "switch") ||
        (kind == ScreenActionKind::SET_NUMBER && visualType != "number") ||
        (kind == ScreenActionKind::TRIGGER && visualType != "action") ||
        kind == ScreenActionKind::SYSTEM_RESTART ||
        kind == ScreenActionKind::SYSTEM_ACTION) {
        error = "Action kind does not match the saved card";
        return false;
    }

    const String controlRef = selected["controlRef"] | "";
    if (controlRef.isEmpty()) {
        error = "binding_unconfigured";
        return false;
    }
    if (!controlRef.startsWith("rule.")) {
        error = "Control binding is invalid";
        return false;
    }
    const String ruleId = controlRef.substring(5);
    PeriphExecRule rule;
    if (!PeriphExecManager::getInstance().getRuleSnapshot(ruleId, rule) ||
        !rule.enabled || !panelSafeRule(rule)) {
        error = "Control rule is missing, disabled or no longer panel-safe";
        return false;
    }
    if (String(selected["bindingFingerprint"] | "") != ruleFingerprint(rule)) {
        error = "Control rule changed; re-save the panel binding";
        return false;
    }
    PanelNumericConstraints numeric;
    const PanelRuleInputKind inputKind = resolvePanelRuleInputKind(rule, &numeric);
    if (!validatePanelControlInput(inputKind, visualType, error)) {
        return false;
    }

    String receivedValue;
    if (kind == ScreenActionKind::SET_BOOLEAN) {
        if (value != "on" && value != "off") {
            error = "Boolean control value must be on or off";
            return false;
        }
        receivedValue = value;
    } else if (kind == ScreenActionKind::SET_NUMBER) {
        double parsed = 0;
        uint8_t decimals = 0;
        const uint8_t precision = selected["precision"] | 0;
        const double minimum = selected["rangeMin"].as<double>();
        const double maximum = selected["rangeMax"].as<double>();
        const double step = selected["step"].as<double>();
        if (!canonicalPanelNumber(value, parsed, &decimals) || decimals > precision ||
            parsed < minimum || parsed > maximum ||
            !panelNumberAligned(parsed, minimum, step) ||
            (numeric.integerOnly && std::floor(parsed) != parsed) ||
            (numeric.bounded &&
             (parsed < numeric.minimum || parsed > numeric.maximum))) {
            error = "Numeric control value is invalid, out of range or off step";
            return false;
        }
        receivedValue = value;
    } else if (kind == ScreenActionKind::TRIGGER && !value.isEmpty()) {
        error = "Trigger controls do not accept a runtime value";
        return false;
    }

    String rejection;
    AsyncExecResult admitted;
    if (!PeriphExecManager::getInstance().runOnce(ruleId, receivedValue,
                                                   &rejection, &admitted)) {
        error = rejection.isEmpty() ? String("Control execution was rejected") : rejection;
        return false;
    }
    operationId = admitted.operationId;
    {
        PanelLock lock(_mutex);
        if (lock.locked()) {
            size_t slot = _operationCardIds.size();
            for (size_t i = 0; i < _operationCardIds.size(); ++i) {
                if (_operationCardIds[i] == cardId || _operationCardIds[i].isEmpty()) {
                    slot = i;
                    break;
                }
            }
            if (slot == _operationCardIds.size()) slot = operationId % _operationCardIds.size();
            _operationCardIds[slot] = cardId;
            _operationIds[slot] = operationId;
        }
    }
    return true;
}

void ScreenPanelManager::notifyRestartSaveFailure() {
    PanelLock lock(_mutex);
    if (!lock.locked()) return;
    _restartSaveFailed = true;
    _restartWithoutHistorySave = false;
}

bool ScreenPanelManager::consumeRestartWithoutHistorySave() {
    PanelLock lock(_mutex);
    if (!lock.locked() || !_restartWithoutHistorySave) return false;
    _restartWithoutHistorySave = false;
    return true;
}

bool ScreenPanelManager::setActivePage(const String& pageId) {
    if (!validIdentifier(pageId)) return false;
    JsonDocument config;
    uint32_t revision = 0;
    bool persisted = false;
    if (!copyConfig(config, revision, persisted)) return false;
    bool found = false;
    for (JsonObjectConst page : config["pages"].as<JsonArrayConst>()) {
        if (pageId == String(page["id"] | "")) {
            found = true;
            break;
        }
    }
    if (!found) return false;
    PanelLock lock(_mutex);
    if (!lock.locked()) return false;
    _activePageId = pageId;
    return true;
}

bool getScreenPanelSnapshot(ScreenPanelSnapshot& out) {
    auto& manager = ScreenPanelManager::instance();
    return manager.initialize() && manager.getSnapshot(out);
}

bool dispatchScreenPanelAction(const char* cardId, ScreenActionKind kind,
                               bool target, uint32_t& operationId,
                               String& error) {
    auto& manager = ScreenPanelManager::instance();
    return manager.initialize() &&
           manager.dispatchAction(cardId, kind, target, operationId, error);
}

bool dispatchScreenPanelAction(const char* cardId, ScreenActionKind kind,
                               const String& value, uint32_t& operationId,
                               String& error) {
    auto& manager = ScreenPanelManager::instance();
    return manager.initialize() &&
           manager.dispatchAction(cardId, kind, value, operationId, error);
}

#endif  // FASTBEE_ENABLE_SCREEN_PANEL
