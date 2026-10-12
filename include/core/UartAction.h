#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include "core/PeripheralExecution.h"
#include "core/ResourceProfile.h"

namespace FastBee {
// UART actions are bounded byte strings, not a protocol acknowledgement.
constexpr size_t kUartActionMaxBytes = 256;

inline const char* buildUartActionPayload(const ExecAction& action,
                                        const String& effectiveValue,
                                        const String& receivedValue, String& payload) {
    payload = "";
    String pattern = action.actionValue;
    if (pattern.length() != action.actionValue.length()) return "UART_PAYLOAD_MEMORY";
    if (pattern.length() > ResourceProfile::MAX_ACTION_VALUE_LEN) return "UART_TEMPLATE_TOO_LARGE";
    if (pattern.length() != std::strlen(pattern.c_str())) return "UART_PAYLOAD_TYPE_INVALID";
    if (pattern.startsWith("{")) {
        // Parse BEFORE replacing values. A received quote, backslash or JSON
        // fragment is payload data and cannot rewrite the command/target.
        JsonDocument doc;
        if (deserializeJson(doc, pattern.c_str(), pattern.length()) || !doc.is<JsonObject>()) return "UART_PAYLOAD_JSON_INVALID";
        const auto text = [](JsonVariantConst value) {
            if (!value.is<const char*>()) return false;
            const JsonString s = value.as<JsonString>();
            return s.size() == std::strlen(s.c_str());
        };
        if (!doc["periphId"].isNull() && (!text(doc["periphId"]) ||
            action.targetPeriphId != doc["periphId"].as<const char*>()))
            return "UART_TARGET_MISMATCH";
        if (!doc["action"].isNull()) {
            if (!text(doc["action"])) return "UART_COMMAND_INVALID";
            String command = doc["action"].as<const char*>();
            command.toLowerCase();
            if (command != "send" && command != "text" && command != "write")
                return "UART_COMMAND_INVALID";
        }
        unsigned fields = 0;
        for (const char* key : {"value", "text", "message"}) {
            if (doc[key].isNull()) continue;
            if (!text(doc[key])) return "UART_PAYLOAD_TYPE_INVALID";
            pattern = doc[key].as<const char*>();
            ++fields;
        }
        for (JsonPairConst field : doc.as<JsonObjectConst>()) {
            const char* key = field.key().c_str();
            if (std::strcmp(key, "periphId") && std::strcmp(key, "action") &&
                std::strcmp(key, "value") && std::strcmp(key, "text") && std::strcmp(key, "message"))
                return "UART_PAYLOAD_FIELD_INVALID";
        }
        if (fields != 1) return "UART_PAYLOAD_FIELD_INVALID";
        if (pattern.isEmpty()) return "UART_PAYLOAD_EMPTY";
    }
    // Preserve literal text including whitespace. Empty/direct received-value
    // actions use the effective value; explicit placeholders use the trigger.
    const String& substitute = !receivedValue.isEmpty() ? receivedValue : effectiveValue;
    const bool direct = pattern.isEmpty();
    if (direct) {
        if (effectiveValue.length() > kUartActionMaxBytes) return "UART_PAYLOAD_TOO_LARGE";
        pattern = effectiveValue;
        if (pattern.length() != effectiveValue.length()) return "UART_PAYLOAD_MEMORY";
    }
    if (pattern.isEmpty()) return "UART_PAYLOAD_EMPTY";
    String staged;
    if (!staged.reserve(kUartActionMaxBytes)) return "UART_PAYLOAD_MEMORY";
    for (size_t offset = 0; offset < pattern.length();) {
        const bool token = !direct && offset + 6 <= pattern.length() &&
            std::memcmp(pattern.c_str() + offset, "$value", 6) == 0;
        if (token) {
            if (substitute.isEmpty() || substitute == action.actionValue)
                return "UART_VALUE_UNRESOLVED";
            if (substitute.length() > kUartActionMaxBytes - staged.length())
                return "UART_PAYLOAD_TOO_LARGE";
            if (!staged.concat(substitute)) return "UART_PAYLOAD_MEMORY";
            offset += 6; // Never recursively expand bytes supplied by a trigger.
        } else {
            if (staged.length() == kUartActionMaxBytes) return "UART_PAYLOAD_TOO_LARGE";
            if (!staged.concat(pattern.charAt(offset++))) return "UART_PAYLOAD_MEMORY";
        }
    }
    if (staged.isEmpty()) return "UART_PAYLOAD_EMPTY";
    payload = staged;
    if (payload.length() != staged.length()) { payload = ""; return "UART_PAYLOAD_MEMORY"; }
    return nullptr;
}

// Validate fixed bytes / envelope at save and preflight. A one-byte placeholder
// is only a lower bound: every execution must recheck the actual expanded size.
inline const char* validateUartAction(const ExecAction& action) {
    String ignored;
    return buildUartActionPayload(action, action.useReceivedValue ? String("x") : action.actionValue,
                                 String("x"), ignored);
}
} // namespace FastBee
