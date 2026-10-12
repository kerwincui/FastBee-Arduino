#pragma once
#include <cmath>
#include <cstddef>
#include <cstring>
#include "peripherals/drivers/PCA9685Contract.h"

namespace FastBee { namespace H1Parameter {
enum class Kind { Reference, Integer, Number, Boolean };
struct Field {
    const char* key;
    Kind kind;
    const char* unit;
    double defaultValue;
    double minimum;
    double maximum;
    bool hasMinimum;
    bool hasMaximum;
    bool exclusiveMinimum;
    bool required;
    size_t maxBytes;
};
constexpr Field numeric(const char* key, Kind kind, const char* unit, double value,
                        double min, double max, bool hasMin = true,
                        bool hasMax = true, bool exclusiveMin = false, bool required = true) {
    return {key, kind, unit, value, min, max, hasMin, hasMax, exclusiveMin, required, 0};
}
constexpr Field reference(const char* key, bool required) {
    return {key, Kind::Reference, "", 0, 0, 0, false, false, false, required, 40};
}
inline constexpr Field ioAddress = numeric("address", Kind::Integer, "", 0x20, 0x20, 0x27, true, true, false, false);
inline constexpr Field ioInterval = numeric("sampleInterval", Kind::Integer, "ms", 20, 20, 1000, true, true, false, false);
inline constexpr Field ioBus = reference("busPeripheralId", false);
inline constexpr Field mcpReset = reference("resetPeripheralId", true);
static constexpr unsigned pcaExcludedAddress = 0x70;
inline constexpr Field pcaFields[] = {
    reference("busPeripheralId", true), reference("oePeripheralId", true),
    numeric("address", Kind::Integer, "", 64, 64, 119),
    numeric("channelMask", Kind::Integer, "bitmask", 1, 1, 65535),
    numeric("pwmHz", Kind::Number, "Hz", 50, 0, 0, true, false, true),
    numeric("clockHz", Kind::Number, "Hz", 25000000, 0, PCA9685_MAX_CLOCK_HZ, true, true, true),
    numeric("minAngle", Kind::Number, "deg", 0, -360, 360),
    numeric("maxAngle", Kind::Number, "deg", 180, -360, 360),
    numeric("minPulseUs", Kind::Number, "us", 500, 0, 0, true, false, true),
    numeric("maxPulseUs", Kind::Number, "us", 2500, 0, 0, true, false, true),
    {"openDrain", Kind::Boolean, "", 0, 0, 0, false, false, false, true, 0}
};
inline constexpr Field channel = numeric("channel", Kind::Integer, "", 0, 0, 15);
inline constexpr Field duty = numeric("value", Kind::Number, "%", 0, 0, 100);
// Literal syntax domains: configured session/frequency checks remain stricter.
// In particular pulse=0 is parseable but rejected by the hardware conversion.
inline constexpr Field pulse = numeric("value", Kind::Number, "us", 0, 0, 0, true, false);
inline constexpr Field angle = numeric("value", Kind::Number, "deg", 0, 0, 0, false, false);
struct Command { const char* id; const Field* value; };
inline constexpr Command commands[] = {
    {"duty", &duty}, {"pulse", &pulse}, {"angle", &angle},
    {"enable", nullptr}, {"disable", nullptr}, {"stop", nullptr}
};
inline const Field* pcaField(const char* key) {
    if (key) for (const Field& field : pcaFields) if (!strcmp(key, field.key)) return &field;
    return nullptr;
}
inline const Command* command(const char* id) {
    if (id) for (const Command& item : commands) if (!strcmp(id, item.id)) return &item;
    return nullptr;
}
inline bool accepts(const Field& field, double value) {
    return (field.kind == Kind::Integer || field.kind == Kind::Number) && std::isfinite(value) &&
        (field.kind != Kind::Integer || std::floor(value) == value) &&
        (!field.hasMinimum || (field.exclusiveMinimum ? value > field.minimum : value >= field.minimum)) &&
        (!field.hasMaximum || value <= field.maximum);
}
inline bool acceptsReference(const Field& field, const char* value, size_t bytes) {
    return field.kind == Kind::Reference && value && (!field.required || bytes != 0) &&
        bytes <= field.maxBytes && std::strlen(value) == bytes;
}
inline bool acceptsIoInterval(double value, bool persisted) {
    // Existing persisted zero means the default interval. New requests cannot write zero.
    return (persisted && value == 0) || accepts(ioInterval, value);
}
} }
