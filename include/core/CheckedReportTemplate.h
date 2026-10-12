#pragma once
#include "utils/CheckedText.h"
#include <ArduinoJson.h>
#include "utils/SharedJsonSerializer.h"

namespace FastBee {
template<class Text>
struct CheckedTemplateWriter {
    Text& out;
    size_t limit;
    bool good = true;
    size_t write(uint8_t byte) { return write(&byte, 1); }
    size_t write(const uint8_t* bytes, size_t size) {
        if (!good || !appendCheckedText(out, reinterpret_cast<const char*>(bytes), size, limit)) {
            good = false;
            return 0;
        }
        return size;
    }
};

template<class Text>
bool checkedTemplateValue(JsonVariantConst value, Text& text, size_t limit) {
    if (value.is<JsonString>()) {
        const JsonString str = value.as<JsonString>();
        return copyCheckedText(text, str.c_str(), str.size(), limit);
    }
    text = "";
    const size_t expected = measureJson(value);
    if (expected > limit || !text.reserve(static_cast<unsigned int>(expected))) return false;
    CheckedTemplateWriter<Text> writer{text, limit};
    return serializeJsonShared(value, writer) == expected && writer.good && text.length() == expected;
}

// Same first-32, ordered replacement semantics as the legacy report template.
// No vector of unchecked key/value String copies. Failure never returns raw
// data as a substitute for an intended transform.
template<class Text>
bool applyCheckedReportTemplate(const Text& script, const Text& input, Text& output, size_t limit) {
    output = "";
    if (input.length() > limit || std::memchr(input.c_str(), '\0', input.length())) return false;
    if (!script.length() || !input.length())
        return copyCheckedText(output, input.c_str(), input.length(), limit);
    JsonDocument doc;
    const auto error = deserializeJson(doc, input.c_str(), input.length());
    if (error == DeserializationError::NoMemory || doc.overflowed()) return false;
    // Non-JSON text remains legal; it historically bypasses JSON templates.
    if (error || (!doc.is<JsonObject>() && !doc.is<JsonArray>()))
        return copyCheckedText(output, input.c_str(), input.length(), limit);
    Text result;
    size_t count = 0;
    const auto replace = [&](const Text& key, JsonVariantConst value) {
        if (!count && !copyCheckedText(result, script.c_str(), script.length(), limit)) return false;
        Text token, replacement, next;
        if (!appendCheckedText(token, "${", 2, limit) ||
            !appendCheckedText(token, key.c_str(), key.length(), limit) ||
            !appendCheckedText(token, "}", 1, limit) ||
            !checkedTemplateValue(value, replacement, limit) ||
            !replaceCheckedText(result, token.c_str(), replacement.c_str(), replacement.length(), next, limit))
            return false;
        result = std::move(next);
        ++count;
        return true;
    };
    if (doc.is<JsonArray>()) {
        for (JsonObjectConst item : doc.as<JsonArrayConst>()) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;
            if (count >= 32) break;
            Text key;
            if (!checkedTemplateValue(item["id"], key, limit) || !replace(key, item["value"])) return false;
        }
    } else {
        for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
            if (count >= 32) break;
            Text key;
            if (!copyCheckedText(key, pair.key().c_str(), pair.key().size(), limit) ||
                !replace(key, pair.value())) return false;
        }
    }
    if (!count) return copyCheckedText(output, input.c_str(), input.length(), limit);
    output = std::move(result);
    return true;
}
} // namespace FastBee
