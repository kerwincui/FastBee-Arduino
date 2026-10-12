#pragma once
#include <ArduinoJson.h>
#include <cstdint>

// A bounded register protocol, not a model-specific driver. Missing profiles
// retain legacy FC03/FC06 behavior; explicit invalid profiles fail closed.
struct MotorProtocol {
    bool configured = false, valid = true, finitePulseMove = true;
    bool commandPosition = false;
    bool startupReference = false;
    int32_t startupPosition = 0;
    uint8_t readFunction = 3, writeFunction = 6;
    uint16_t speedReadRegister = 0, pulseReadRegister = 0;
    uint16_t forwardValue = 1, reverseValue = 1, stopValue = 1;

    uint16_t readRegister(const uint16_t* regs, bool pulse) const {
        return configured ? (pulse ? pulseReadRegister : speedReadRegister) : regs[pulse ? 4 : 3];
    }
    bool legacyMapping(const uint16_t* regs) const {
        return valid && finitePulseMove && !commandPosition && readFunction == 3 && writeFunction == 6 &&
            forwardValue == 1 && reverseValue == 1 && stopValue == 1 &&
            readRegister(regs, false) == regs[3] && readRegister(regs, true) == regs[4];
    }
};

inline const char* loadMotorProtocol(JsonObjectConst device, MotorProtocol& out) {
    out = MotorProtocol{};
    if (device["motorProtocol"].isUnbound()) return nullptr;
    out.configured = true;
    out.valid = false;
    JsonObjectConst p = device["motorProtocol"].as<JsonObjectConst>();
    if (p.isNull() || !p["version"].is<unsigned>() || p["version"].as<unsigned>() != 1)
        return "motorProtocol.version must be 1";
    if (!p["readFunction"].is<unsigned>() ||
        (p["readFunction"].as<unsigned>() != 3 && p["readFunction"].as<unsigned>() != 4))
        return "motorProtocol.readFunction must be 3 or 4";
    if (!p["writeFunction"].is<unsigned>() ||
        (p["writeFunction"].as<unsigned>() != 6 && p["writeFunction"].as<unsigned>() != 16))
        return "motorProtocol.writeFunction must be 6 or 16";
    const char* words[] = {"speedReadRegister", "pulseReadRegister", "forwardValue", "reverseValue", "stopValue"};
    for (const char* key : words) {
        if (!p[key].is<uint16_t>()) return "motorProtocol addresses and values must be uint16 integers";
    }
    if (!p["finitePulseMove"].is<bool>()) return "motorProtocol.finitePulseMove must be boolean";
    if (!p["positionTracking"].isUnbound()) {
        if (!p["positionTracking"].is<const char*>()) return "motorProtocol.positionTracking must be a string";
        const char* tracking = p["positionTracking"];
        if (strcmp(tracking, "conservative") && strcmp(tracking, "commanded"))
            return "motorProtocol.positionTracking must be conservative or commanded";
        out.commandPosition = !strcmp(tracking, "commanded");
    }
    if (out.commandPosition && !p["finitePulseMove"].as<bool>())
        return "Command position tracking requires finite pulse motion";
    if (!p["startupPosition"].isUnbound()) {
        if (!out.commandPosition || !p["startupPosition"].is<int32_t>())
            return "motorProtocol.startupPosition requires commanded tracking and an int32 position";
        const int32_t minimum = device["motorMinPosition"] | 0;
        const int32_t maximum = device["motorMaxPosition"] | 0;
        out.startupPosition = p["startupPosition"];
        if (maximum <= minimum || out.startupPosition < minimum || out.startupPosition > maximum)
            return "motorProtocol.startupPosition must be inside the configured limits";
        out.startupReference = true;
    }
    JsonArrayConst regs = device["motorRegs"].as<JsonArrayConst>();
    if (regs.size() != 5) return "motorRegs must contain five register addresses";
    for (JsonVariantConst reg : regs) if (!reg.is<uint16_t>()) return "motorRegs must contain uint16 integers";
    const uint16_t values[] = {p["forwardValue"].as<uint16_t>(), p["reverseValue"].as<uint16_t>(), p["stopValue"].as<uint16_t>()};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = i + 1; j < 3; ++j)
            if (regs[i] == regs[j] && values[i] == values[j]) return "Motor actions must have distinct register/value pairs";
        if (regs[i] == regs[3] || regs[i] == regs[4]) return "Motor parameter writes must not alias action registers";
    }
    if (regs[3] == regs[4]) return "Motor speed and pulse write registers must differ";
    out.readFunction = p["readFunction"];
    out.writeFunction = p["writeFunction"];
    out.speedReadRegister = p["speedReadRegister"];
    out.pulseReadRegister = p["pulseReadRegister"];
    out.forwardValue = values[0]; out.reverseValue = values[1]; out.stopValue = values[2];
    out.finitePulseMove = p["finitePulseMove"];
    out.valid = true;
    return nullptr;
}

inline void saveMotorProtocol(JsonObject device, const MotorProtocol& p) {
    if (!p.configured) return;
    JsonObject out = device["motorProtocol"].to<JsonObject>();
    // Retain an invalid marker instead of silently saving legacy defaults.
    out["version"] = p.valid ? 1 : 0;
    out["readFunction"] = p.readFunction; out["writeFunction"] = p.writeFunction;
    out["speedReadRegister"] = p.speedReadRegister; out["pulseReadRegister"] = p.pulseReadRegister;
    out["forwardValue"] = p.forwardValue; out["reverseValue"] = p.reverseValue; out["stopValue"] = p.stopValue;
    out["finitePulseMove"] = p.finitePulseMove;
    if (p.commandPosition) out["positionTracking"] = "commanded";
    if (p.startupReference) out["startupPosition"] = p.startupPosition;
}
