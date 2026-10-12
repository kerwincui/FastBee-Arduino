#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include <cstdint>

namespace FastBee {
inline bool mqttRequiresTls(JsonVariantConst mqtt) {
    if (mqtt["downgraded_by_memory"] | false) return true;
    return std::strcmp(mqtt["scheme"] | "", "mqtts") == 0 ||
        (!mqtt["scheme"].is<const char*>() && (mqtt["port"] | 1883U) == 8883U);
}
// Historical automatic downgrade is evidence of secure intent, never consent
// for plaintext. Missing/invalid restoration metadata is rejected, not guessed.
inline uint16_t mqttLegacySecurePort(JsonVariantConst mqtt) {
    if (std::strcmp(mqtt["downgraded_original_scheme"] | "", "mqtts") != 0 ||
        !mqtt["downgraded_original_port"].is<uint32_t>()) return 0;
    const uint32_t original = mqtt["downgraded_original_port"].as<uint32_t>();
    return original > 0 && original <= UINT16_MAX ? static_cast<uint16_t>(original) : 0;
}
} // namespace FastBee
