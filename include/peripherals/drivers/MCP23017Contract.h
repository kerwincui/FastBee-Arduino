#ifndef FASTBEE_MCP23017_CONTRACT_H
#define FASTBEE_MCP23017_CONTRACT_H

#include <cstdint>

namespace FastBee {

// Pure encoding only. Microchip DS20001952D, tables 3-1/3-5 and section 3.5.1.
// No device probing, register writes, bank detection, or registered capability.
enum class MCP23017Register : uint8_t {
    Direction, Polarity, InterruptEnable, DefaultValue, InterruptControl,
    Configuration, Pullup, InterruptFlag, InterruptCapture, Port, Latch
};

inline bool mcp23017RegisterAddress(MCP23017Register reg, uint32_t port,
                                    uint32_t bank, uint8_t& address) {
    address = 0;
    const uint32_t index = static_cast<uint8_t>(reg);
    if (index > 10 || port > 1 || bank > 1) return false;
    address = static_cast<uint8_t>(bank ? index + 0x10U * port : index * 2U + port);
    return true;
}

struct MCP23017PortConfig {
    uint8_t direction[2] = {};
    uint8_t polarity[2] = {};
    uint8_t pullup[2] = {};
    uint8_t latch[2] = {};
    bool valid = false;
};

inline bool makeMCP23017PortConfig(uint32_t outputMask, uint32_t inputMask,
                                  uint32_t startupHigh, uint32_t pullupMask,
                                  uint32_t invertInputMask, MCP23017PortConfig& config) {
    config = {};
    constexpr uint32_t outputOnly = 0x8080; // GPA7, GPB7; not MCP23S17 policy.
    if ((outputMask | inputMask | startupHigh | pullupMask | invertInputMask) > 0xffffU ||
        (outputMask & outputOnly) != outputOnly || (inputMask & outputMask) ||
        (startupHigh & ~outputMask) || (pullupMask & ~inputMask) ||
        (invertInputMask & ~inputMask)) return false;
    // Explicit ownership of both output-only pins is required. Never silently
    // convert a configured input or an unclaimed pin into a driven output.
    for (uint8_t port = 0; port < 2; ++port) {
        const uint8_t shift = port * 8U;
        config.direction[port] = static_cast<uint8_t>((~outputMask) >> shift);
        config.polarity[port] = static_cast<uint8_t>(invertInputMask >> shift);
        config.pullup[port] = static_cast<uint8_t>(pullupMask >> shift);
        config.latch[port] = static_cast<uint8_t>(startupHigh >> shift);
    }
    config.valid = true;
    return true;
}

inline bool updateMCP23017Latch(uint32_t knownLatch, uint32_t outputMask,
                               uint32_t selectedMask, uint32_t highMask, uint16_t& result) {
    result = 0;
    if ((knownLatch | outputMask | selectedMask | highMask) > 0xffffU ||
        (outputMask & 0x8080U) != 0x8080U || (selectedMask & ~outputMask) ||
        (highMask & ~selectedMask)) return false;
    result = static_cast<uint16_t>((knownLatch & ~selectedMask) | highMask);
    return true;
}

} // namespace FastBee
#endif
