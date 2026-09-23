#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace FastBee {
namespace ProtocolSecret {

constexpr const char* MASK = "********";

inline bool isMask(const String& value) {
    String normalized = value;
    normalized.trim();
    if (normalized.length() < 3) return false;

    for (size_t i = 0; i < normalized.length(); ++i) {
        if (normalized[i] != '*') return false;
    }
    return true;
}

inline bool shouldPersist(const String& submittedValue, bool supplied) {
    return supplied && !isMask(submittedValue);
}

inline void redactMqttField(JsonObject mqtt, const char* key) {
    if (mqtt.isNull() || mqtt[key].isNull()) return;

    const char* value = mqtt[key] | "";
    if (value && value[0] != '\0') {
        mqtt[key] = MASK;
    } else {
        mqtt.remove(key);
    }
}

inline void redactMqttConfig(JsonObject mqtt) {
    redactMqttField(mqtt, "password");
    redactMqttField(mqtt, "mqttSecret");
    redactMqttField(mqtt, "authCode");
}

inline void redactRemoteConfig(JsonObject config) {
    for (JsonObject source : config["weatherSources"].as<JsonArray>()) {
        redactMqttField(source, "apiKey");
        redactMqttField(source, "authHeader");
    }
}

} // namespace ProtocolSecret
} // namespace FastBee
