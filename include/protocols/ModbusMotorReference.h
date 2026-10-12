#pragma once
#include <Arduino.h>
#include <cstring>
namespace FastBee {
template<class Device>
String motorControlReference(const Device& dev) {
    uint32_t hash = 2166136261UL;
    auto add = [&](const uint8_t* bytes, size_t count) { while (count--) hash = (hash ^ *bytes++) * 16777619UL; };
    const char* end = static_cast<const char*>(std::memchr(dev.sensorId, '\0', sizeof(dev.sensorId)));
    if (!end) return String();
    add(reinterpret_cast<const uint8_t*>(dev.sensorId), static_cast<size_t>(end - dev.sensorId));
    add(reinterpret_cast<const uint8_t*>(dev.motorRegs), sizeof(dev.motorRegs));
    add(&dev.motorDecimals, sizeof(dev.motorDecimals));
    const auto& p = dev.motorProtocol;
    if (p.configured || !p.legacyMapping(dev.motorRegs)) {
        // Hash individual words, never struct padding. Explicit profiles also
        // replace legacy rule-value semantics, so they require a fresh binding.
        const uint16_t words[] = {static_cast<uint16_t>(p.configured), static_cast<uint16_t>(p.valid), p.readFunction, p.writeFunction,
            p.readRegister(dev.motorRegs, false), p.readRegister(dev.motorRegs, true),
            p.forwardValue, p.reverseValue, p.stopValue, static_cast<uint16_t>(p.finitePulseMove)};
        add(reinterpret_cast<const uint8_t*>(words), sizeof(words));
        if (p.commandPosition) { const uint8_t tracking = 1; add(&tracking, sizeof(tracking)); }
        if (p.startupReference) {
            const uint8_t startup = 1; add(&startup, sizeof(startup));
            add(reinterpret_cast<const uint8_t*>(&p.startupPosition), sizeof(p.startupPosition));
        }
    }
    char text[48]; snprintf(text, sizeof(text), "motor.%u.%08lx", dev.slaveAddress, static_cast<unsigned long>(hash));
    return String(text);
}
} // namespace FastBee
