#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {
constexpr uint32_t Ds18b20ConversionMs = 750;
constexpr uint32_t Ds18b20SampleMaxAgeMs = 1750;

inline bool validDs18b20Address(const uint8_t* address) {
    if (!address || address[0] != 0x28) return false;
    uint8_t crc = 0;
    for (unsigned i = 0; i < 7; ++i) {
        uint8_t byte = address[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool mix = (crc ^ byte) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8c;
            byte >>= 1;
        }
    }
    return crc == address[7];
}

inline bool parseDs18b20Address(const char* text, size_t length, uint8_t* address) {
    if (!text || !address || length != 16) return false;
    for (unsigned i = 0; i < 16; ++i) {
        const char c = text[i];
        const int nibble = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (nibble < 0) return false;
        if ((i & 1) == 0) address[i / 2] = static_cast<uint8_t>(nibble << 4);
        else address[i / 2] |= static_cast<uint8_t>(nibble);
    }
    return validDs18b20Address(address);
}
} // namespace FastBee
