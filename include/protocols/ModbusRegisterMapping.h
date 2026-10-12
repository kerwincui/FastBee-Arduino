#pragma once
#include <ArduinoJson.h>
#include "core/SystemConstants.h"
#include "protocols/ModbusRegisterCodec.h"

struct RegisterMapping {
    uint8_t regOffset = 0;
    uint8_t dataType = 0; // 0 uint16, 1 int16, 2 uint32, 3 int32, 4 float32
    uint8_t wordOrder = 0; // 0 high register first (legacy), 1 low register first
    float scaleFactor = 1.0f;
    uint8_t decimalPlaces = 1;
    char sensorId[Protocols::MODBUS_SENSOR_ID_MAX_LEN] = {};
    char unit[8] = {};
};

namespace FastBee {
inline bool mappingByte(JsonObjectConst object, const char* key, uint8_t maximum,
                        uint8_t fallback, uint8_t& value) {
    if (!object.containsKey(key)) { value = fallback; return true; }
    if (!object[key].is<uint8_t>() || object[key].as<uint8_t>() > maximum) return false;
    value = object[key].as<uint8_t>(); return true;
}
template<size_t N>
inline bool mappingText(JsonObjectConst object, const char* key, char (&output)[N], bool identifier = false) {
    if (!object.containsKey(key)) { output[0] = 0; return true; }
    const auto text = object[key].as<JsonString>();
    if (!text || text.size() >= N || text.size() != std::strlen(text.c_str())) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text.c_str()[i]);
        if (byte < 32 || (identifier && (byte == '"' || byte == '\\'))) return false;
    }
    std::memcpy(output, text.c_str(), text.size() + 1); return true;
}
inline bool readModbusRegisterMapping(JsonObjectConst object, size_t quantity, RegisterMapping& output) {
    RegisterMapping parsed;
    if (object.isNull() || !mappingByte(object, "regOffset", 124, 0, parsed.regOffset) ||
        !mappingByte(object, "dataType", 4, 0, parsed.dataType) ||
        !mappingByte(object, "wordOrder", 1, 0, parsed.wordOrder) ||
        !mappingByte(object, "decimalPlaces", 6, 1, parsed.decimalPlaces) ||
        (parsed.dataType < 2 && parsed.wordOrder) ||
        size_t(parsed.regOffset) + (parsed.dataType >= 2 ? 2 : 1) > quantity ||
        !mappingText(object, "sensorId", parsed.sensorId, true) || !mappingText(object, "unit", parsed.unit)) return false;
    if (object.containsKey("scaleFactor")) {
        if (!object["scaleFactor"].is<double>()) return false;
        const double scale = object["scaleFactor"].as<double>();
        if (!std::isfinite(scale) || std::fabs(scale) > std::numeric_limits<float>::max()) return false;
        parsed.scaleFactor = static_cast<float>(scale);
        if (scale != 0 && parsed.scaleFactor == 0) return false;
    }
    output = parsed; return true;
}
inline bool validModbusTaskMappings(JsonObjectConst task) {
    if (!task.containsKey("mappings")) return true;
    const auto mappings = task["mappings"].as<JsonArrayConst>();
    if (mappings.isNull() || mappings.size() > Protocols::MODBUS_MAX_MAPPINGS_PER_TASK) return false;
    if (mappings.size() && task.containsKey("quantity") &&
        (!task["quantity"].is<uint16_t>() || task["quantity"].as<uint16_t>() < 1 ||
         task["quantity"].as<uint16_t>() > Protocols::MODBUS_MAX_REGISTERS_PER_READ)) return false;
    const size_t quantity = task["quantity"] | 10U;
    for (JsonVariantConst entry : mappings) {
        RegisterMapping parsed;
        if (!readModbusRegisterMapping(entry.as<JsonObjectConst>(), quantity, parsed)) return false;
    }
    return true;
}
}
