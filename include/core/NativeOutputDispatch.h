#pragma once
#include "core/PeripheralManager.h"
#include "core/ActionValueContract.h"
#include <stddef.h>
#include <string.h>

namespace FastBee {
// Native/PCF/MCP ordinary digital and native scalar outputs share one provenance,
// grant and retirement path, including native passive buzzers. Servo and protocol backends retain
// their separate admission until their own reviewed integrations are complete.
inline bool isOrdinaryDigitalTarget(const PeripheralConfig& config) {
    return config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
        !config.isModbusPeripheral() &&
        !config.params.gpio.passive;
}

inline void rememberOrdinaryControlResult(const OrdinaryControlResult& current,
                                         OrdinaryControlResult* aggregate) {
    // Preserve the first rejected output even if a later action succeeds.
    if (aggregate && current.evaluated &&
        (!aggregate->evaluated || aggregate->accepted)) *aggregate = current;
}

inline bool isOrdinaryScalarTarget(const PeripheralConfig& config) {
    return !config.isExpanderChannel() && !config.isModbusPeripheral() &&
        (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
         config.type == PeripheralType::GPIO_ANALOG_OUTPUT || config.type == PeripheralType::DAC ||
         (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive));
}

inline bool ordinaryScalarMaximum(const PeripheralConfig& config, uint32_t& maximum) {
    if (!isOrdinaryScalarTarget(config)) return false;
    if (config.type == PeripheralType::DAC) { maximum = 255; return true; }
    const bool passive = config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive;
    const uint8_t resolution = passive && !config.params.gpio.pwmResolution ? 8 : config.params.gpio.pwmResolution;
    const bool analog = !passive && config.params.gpio.aoMode;
    if (resolution < 1 || resolution > 16 || (analog &&
        (config.params.gpio.aoMinPct > 100 || config.params.gpio.aoMaxPct > 100 ||
         config.params.gpio.aoMinPct > config.params.gpio.aoMaxPct))) return false;
    maximum = analog ? 100U : (1U << resolution) - 1U;
    return true;
}

inline bool ordinaryScalarHighValue(const PeripheralConfig& config, uint32_t& high) {
    if (!ordinaryScalarMaximum(config, high)) return false;
    // Preserve passive HIGH as half duty, not a constant-high pad. Raw PWM
    // remains a count. No software value proves sound or load feedback.
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive) high /= 2U;
    return true;
}

inline bool parseOrdinaryScalarValue(const PeripheralConfig& config, const String& text,
                                     uint32_t& value) {
    uint32_t maximum = 0;
    if (!ordinaryScalarMaximum(config, maximum)) return false;
    if (config.type == PeripheralType::DAC) {
        uint8_t parsed = 0;
        if (!parseDacActionValue(text.c_str(), text.length(), parsed)) return false;
        value = parsed; return true;
    }
    long parsed = 0;
    if (!parseStrictActionLong(text.c_str(), text.length(), parsed) || parsed < 0 ||
        static_cast<unsigned long>(parsed) > maximum) return false;
    value = static_cast<uint32_t>(parsed); return true;
}

inline bool rejectOrdinaryDigitalAction(const String& id, const char* reason,
                                       OrdinaryControlResult* aggregate) {
    OrdinaryControlResult current;
    current.evaluated = true;
    if (id.length() <= 40) memcpy(current.targetId, id.c_str(), id.length() + 1);
    current.reason = reason;
    rememberOrdinaryControlResult(current, aggregate);
    return false;
}

inline bool dispatchOrdinaryDigitalAction(PeripheralManager& manager, const String& id,
    DigitalOutputCommand command, uint32_t intervalMs,
    const OrdinaryControlContext* context, OrdinaryControlResult* aggregate) {
    OrdinaryControlResult current;
    current.evaluated = true;
    if (id.length() <= 40) memcpy(current.targetId, id.c_str(), id.length() + 1);
    if (!context) current.reason = "CONTROL_CONTEXT_REQUIRED";
    else manager.submitOrdinaryDigital(id, command, intervalMs, *context, current);
    rememberOrdinaryControlResult(current, aggregate);
    return current.accepted;
}

inline bool parseOrdinaryBlinkInterval(const String& text, uint32_t& intervalMs) {
    if (text.isEmpty()) { intervalMs = 500; return true; }
    if (text.length() > 5) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text.charAt(i);
        if (c < '0' || c > '9') return false;
        value = value * 10U + static_cast<uint32_t>(c - '0');
    }
    if (!value || value > 65535U) return false;
    intervalMs = value;
    return true;
}

inline bool dispatchOrdinaryScalarAction(PeripheralManager& manager, const String& id,
    ScalarOutputCommand command, uint32_t value,
    const OrdinaryControlContext* context, OrdinaryControlResult* aggregate) {
    OrdinaryControlResult current;
    current.evaluated = true;
    if (id.length() <= 40 && id.length() == strlen(id.c_str()))
        memcpy(current.targetId, id.c_str(), id.length() + 1);
    if (!context) current.reason = "CONTROL_CONTEXT_REQUIRED";
    else manager.submitOrdinaryScalar(id, command, value, *context, current);
    rememberOrdinaryControlResult(current, aggregate);
    return current.accepted;
}

inline bool parseOrdinaryBreathePeriod(const String& text, uint32_t& periodMs) {
    if (text.isEmpty()) { periodMs = 2000; return true; }
    return parseOrdinaryBlinkInterval(text, periodMs);
}

inline bool parseOrdinaryScalarCommand(const PeripheralConfig& config, const String& command,
    const String& text, ScalarOutputCommand& output, uint32_t& value) {
    if (!isOrdinaryScalarTarget(config)) return false;
    const bool dac = config.type == PeripheralType::DAC;
    value = 0;
    if (command == "stop") { output = ScalarOutputCommand::Stop; return text.isEmpty(); }
    if (command == "toggle") {
        output = ScalarOutputCommand::Toggle;
        return !dac && text.isEmpty();
    }
    if (command == "blink") {
        output = ScalarOutputCommand::Blink;
        return (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
                (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive)) &&
            parseOrdinaryBlinkInterval(text, value);
    }
    if ((config.type == PeripheralType::GPIO_PWM_OUTPUT || config.type == PeripheralType::GPIO_ANALOG_OUTPUT) && command == "breathe") {
        output = ScalarOutputCommand::Breathe;
        return parseOrdinaryBreathePeriod(text, value);
    }
    output = dac ? ScalarOutputCommand::Dac : ScalarOutputCommand::Pwm;
    if (command == "high" || command == "on")
        return text.isEmpty() && ordinaryScalarHighValue(config, value);
    if (command == "low" || command == "off") return text.isEmpty();
    if ((!dac && command == "pwm") || (dac && command == "dac"))
        return parseOrdinaryScalarValue(config, text, value);
    // Numeric command is a scalar value, including 0 and 1, not a boolean.
    return text.isEmpty() && parseOrdinaryScalarValue(config, command, value);
}
} // namespace FastBee
