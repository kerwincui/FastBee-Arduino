#pragma once
#include <ArduinoJson.h>
#include "core/RfOutputContract.h"
#include "core/NativeOutputDispatch.h"
#include <cstring>

namespace FastBee {
struct RfOutputAction {
    char code[35]{};
    uint32_t bits = 0, pulseUs = 0, repeat = 0;
};
inline bool rfJsonOption(JsonVariantConst input, uint32_t maximum, uint32_t& value) {
    if (input.is<const char*>()) {
        const JsonString text = input.as<JsonString>();
        return parseRfOutputOption(text.c_str(), text.size(), maximum, value);
    }
    // ArduinoJson's integer predicate rejects stored floats, bool and signed
    // negatives; never narrow before checking the full value.
    if (!input.is<uint32_t>()) return false;
    value = input.as<uint32_t>(); return value <= maximum;
}
inline bool parseRfOutputAction(const String& id, const String& raw, const String& effective,
                                const String& received, RfOutputAction& output) {
    output = {};
    const String& replacement = received.isEmpty() ? effective : received;
    const char* code = nullptr; size_t length = 0;
    JsonDocument doc;
    if (raw.length() && raw.charAt(0) == '{') {
        if (raw.length() > 384 || raw.length() != std::strlen(raw.c_str()) ||
            deserializeJson(doc, raw.c_str(), raw.length()) || doc.overflowed() || !doc.is<JsonObject>()) return false;
        const auto object = doc.as<JsonObjectConst>();
        for (JsonPairConst field : object) {
            const JsonString key = field.key();
            if (key.size() != std::strlen(key.c_str()) ||
                (std::strcmp(key.c_str(), "periphId") && std::strcmp(key.c_str(), "action") &&
                 std::strcmp(key.c_str(), "code") && std::strcmp(key.c_str(), "value") &&
                 std::strcmp(key.c_str(), "bits") && std::strcmp(key.c_str(), "bitLength") &&
                 std::strcmp(key.c_str(), "pulseWidth") && std::strcmp(key.c_str(), "repeat"))) return false;
        }
        if (object.containsKey("periphId")) {
            if (!object["periphId"].is<const char*>()) return false;
            const JsonString target = object["periphId"].as<JsonString>();
            if (target.size() != id.length() || std::memcmp(target.c_str(), id.c_str(), id.length())) return false;
        }
        if (object.containsKey("action")) {
            if (!object["action"].is<const char*>()) return false;
            const JsonString action = object["action"].as<JsonString>();
            if (action.size() != std::strlen(action.c_str()) ||
                (std::strcmp(action.c_str(), "send") && std::strcmp(action.c_str(), "tx") &&
                 std::strcmp(action.c_str(), "write") && std::strcmp(action.c_str(), "code"))) return false;
        }
        const bool hasCode = object.containsKey("code"), hasValue = object.containsKey("value");
        if (hasCode == hasValue || (object.containsKey("bits") && object.containsKey("bitLength"))) return false;
        const JsonVariantConst payload = object[hasCode ? "code" : "value"];
        if (!payload.is<const char*>()) return false;
        const JsonString value = payload.as<JsonString>(); code = value.c_str(); length = value.size();
        if (object.containsKey("bits") && !rfJsonOption(object["bits"], 32, output.bits)) return false;
        if (object.containsKey("bitLength") && !rfJsonOption(object["bitLength"], 32, output.bits)) return false;
        if (object.containsKey("pulseWidth") && !rfJsonOption(object["pulseWidth"], 2000, output.pulseUs)) return false;
        if (object.containsKey("repeat") && !rfJsonOption(object["repeat"], 20, output.repeat)) return false;
    } else {
        const String& value = raw.isEmpty() ? replacement : raw;
        code = value.c_str(); length = value.length();
    }
    if (length == 6 && std::memcmp(code, "$value", 6) == 0) {
        code = replacement.c_str(); length = replacement.length();
    }
    uint32_t parsed = 0; uint8_t inferred = 0;
    if (!parseRfOutputCode(code, length, parsed, inferred) ||
        (output.pulseUs && output.pulseUs < 100)) return false;
    std::memcpy(output.code, code, length); output.code[length] = '\0'; return true;
}
inline bool executeRfOutputAction(PeripheralManager& manager, const String& id, const String& raw,
    const String& effective, const String& received, const OrdinaryControlContext* context,
    OrdinaryControlResult* result) {
    RfOutputAction action;
    if (!parseRfOutputAction(id, raw, effective, received, action))
        return rejectOrdinaryDigitalAction(id, "RF_PARAMETERS_INVALID", result);
    return manager.sendRfCode(id, String(action.code), action.bits, action.pulseUs, action.repeat, context, result);
}
} // namespace FastBee
