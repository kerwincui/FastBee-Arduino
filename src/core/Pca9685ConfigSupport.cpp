#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterJson.h"
#include <cmath>
#include <cstring>

namespace FastBee {
bool parsePca9685Command(const String& json, Pca9685Command& output, String& error) {
    output = {}; error = "PCA_ACTION_INVALID";
    if (json.isEmpty() || json.length() > 256 || strlen(json.c_str()) != json.length()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json.c_str(), json.length()) || doc.overflowed() || !doc.is<JsonObject>() || !doc["action"].is<const char*>()) return false;
    const JsonString action = doc["action"].as<JsonString>();
    if (!action.size() || action.size() >= sizeof(output.action) || strlen(action.c_str()) != action.size()) return false;
    const H1Parameter::Command* descriptor = H1Parameter::command(action.c_str());
    if (!descriptor) return false;
    for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
        const char* key = pair.key().c_str();
        if (strlen(key) != pair.key().size()) return false;
        if (strcmp(key, "action") && (!descriptor->value || (strcmp(key, "channel") && strcmp(key, "value")))) return false;
    }
    if (descriptor->value) {
        if (!acceptsH1JsonField(H1Parameter::channel, doc["channel"]) ||
            !acceptsH1JsonField(*descriptor->value, doc["value"])) return false;
        output.channel = doc["channel"].as<uint32_t>(); output.value = doc["value"].as<double>();
    }
    strcpy(output.action, action.c_str()); error = ""; return true;
}

bool validatePca9685Config(const PeripheralConfig& config, String& error) {
    const auto& p = config.params.pca9685;
    const auto number = [](const char* key, double value) {
        const auto* field = H1Parameter::pcaField(key);
        return field && H1Parameter::accepts(*field, value);
    };
    const auto reference = [](const char* key, const char* value, size_t capacity) {
        const auto* field = H1Parameter::pcaField(key);
        return field && memchr(value, 0, capacity) &&
            H1Parameter::acceptsReference(*field, value, strlen(value));
    };
    PCA9685FrequencyConfig frequency;
    uint16_t minTicks = 0, maxTicks = 0;
    if (config.type != PeripheralType::PCA9685_PWM || config.pinCount != 0 ||
        !reference("busPeripheralId", p.busPeripheralId, sizeof(p.busPeripheralId)) ||
        !reference("oePeripheralId", p.oePeripheralId, sizeof(p.oePeripheralId)) ||
        !strcmp(p.busPeripheralId, p.oePeripheralId) ||
        config.id == p.busPeripheralId || config.id == p.oePeripheralId ||
        !number("address", p.address) || p.address == H1Parameter::pcaExcludedAddress ||
        !number("channelMask", p.channelMask) ||
        !number("pwmHz", p.pwmHz) || !number("clockHz", p.clockHz) ||
        !makePCA9685FrequencyConfig(p.pwmHz, p.clockHz, frequency) ||
        !number("minAngle", p.minAngle) || !number("maxAngle", p.maxAngle) || p.minAngle >= p.maxAngle ||
        !number("minPulseUs", p.minPulseUs) || !number("maxPulseUs", p.maxPulseUs) || p.minPulseUs >= p.maxPulseUs ||
        !makePCA9685PulseTicks(p.minPulseUs, frequency.actualHz, minTicks) ||
        !makePCA9685PulseTicks(p.maxPulseUs, frequency.actualHz, maxTicks) || minTicks >= maxTicks) {
        error = "PCA_CONFIG_INVALID"; return false;
    }
    error = ""; return true;
}

bool parsePca9685Config(JsonObjectConst params, PeripheralConfig& config, String& error) {
    error = "PCA_CONFIG_INVALID";
    if (params.isNull() || config.type != PeripheralType::PCA9685_PWM) return false;
    // Confirmation and initialization are never configuration or rule fields.
    for (JsonPairConst pair : params) {
        const char* key = pair.key().c_str();
        if (strlen(key) != pair.key().size()) return false;
        if (!H1Parameter::pcaField(key)) { error = "PCA_UNKNOWN_FIELD"; return false; }
    }
    for (const auto& field : H1Parameter::pcaFields)
        if (!acceptsH1JsonField(field, params[field.key])) return false;
    PCA9685FrequencyConfig rawFrequency;
    uint16_t rawMinTicks = 0, rawMaxTicks = 0;
    if (!makePCA9685FrequencyConfig(params["pwmHz"].as<double>(), params["clockHz"].as<double>(), rawFrequency) ||
        params["minAngle"].as<double>() >= params["maxAngle"].as<double>() ||
        !makePCA9685PulseTicks(params["minPulseUs"].as<double>(), rawFrequency.actualHz, rawMinTicks) ||
        !makePCA9685PulseTicks(params["maxPulseUs"].as<double>(), rawFrequency.actualHz, rawMaxTicks) || rawMinTicks >= rawMaxTicks) return false;
    auto previous = config.params.pca9685;
    auto& p = config.params.pca9685;
    std::strcpy(p.busPeripheralId, params["busPeripheralId"].as<const char*>());
    std::strcpy(p.oePeripheralId, params["oePeripheralId"].as<const char*>());
    p.address = params["address"].as<uint8_t>(); p.channelMask = params["channelMask"].as<uint16_t>();
    p.pwmHz = params["pwmHz"].as<float>(); p.clockHz = params["clockHz"].as<float>();
    p.minAngle = params["minAngle"].as<float>(); p.maxAngle = params["maxAngle"].as<float>();
    p.minPulseUs = params["minPulseUs"].as<float>(); p.maxPulseUs = params["maxPulseUs"].as<float>();
    p.openDrain = params["openDrain"].as<bool>();
    if (validatePca9685Config(config, error)) return true;
    p = previous; return false;
}

void serializePca9685Config(JsonObject params, const PeripheralConfig& config) {
    const auto& p = config.params.pca9685;
    params["busPeripheralId"] = JsonString(p.busPeripheralId, false); params["oePeripheralId"] = JsonString(p.oePeripheralId, false);
    params["address"] = p.address; params["channelMask"] = p.channelMask;
    params["pwmHz"] = p.pwmHz; params["clockHz"] = p.clockHz;
    params["minAngle"] = p.minAngle; params["maxAngle"] = p.maxAngle;
    params["minPulseUs"] = p.minPulseUs; params["maxPulseUs"] = p.maxPulseUs;
    params["openDrain"] = p.openDrain;
}
}
