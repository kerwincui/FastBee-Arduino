#pragma once
#include <stddef.h>
#include <stdint.h>

namespace FastBee {
struct RfOutputFrame {
    uint32_t code = 0;
    uint8_t bits = 0, repeat = 0;
    uint16_t pulseUs = 0;
};
// No signs, separators, whitespace, truncation or platform-sized strtoul.
// Bare hexadecimal with A..F remains supported. Binary preserves its width.
inline bool parseRfOutputCode(const char* text, size_t length, uint32_t& code, uint8_t& inferredBits) {
    code = 0; inferredBits = 0;
    if (!text || !length || length > 34) return false;
    unsigned base = 10; size_t start = 0;
    if (length >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2; start = 2;
        if (length == 2 || length - 2 > 32) return false;
        inferredBits = static_cast<uint8_t>(length - 2);
    } else if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16; start = 2;
    } else {
        for (size_t i = 0; i < length; ++i)
            if ((text[i] >= 'a' && text[i] <= 'f') || (text[i] >= 'A' && text[i] <= 'F')) base = 16;
    }
    if (start == length) return false;
    uint32_t value = 0;
    for (size_t i = start; i < length; ++i) {
        const char c = text[i];
        const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') :
            c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) :
            c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : 255U;
        if (digit >= base || value > (UINT32_MAX - digit) / base) return false;
        value = value * base + digit;
    }
    code = value; return true;
}
inline bool parseRfOutputOption(const char* text, size_t length, uint32_t maximum, uint32_t& value) {
    value = 0;
    if (!text || !length || length > 10) return false;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const unsigned digit = static_cast<unsigned>(text[i] - '0');
        if (digit > maximum || value > (maximum - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    return true;
}
inline bool resolveRfOutputFrame(const char* text, size_t length,
    uint32_t bits, uint32_t pulseUs, uint32_t repeat,
    uint32_t defaultBits, uint32_t defaultPulseUs, uint32_t defaultRepeat, RfOutputFrame& out) {
    out = {};
    uint8_t inferred = 0; uint32_t code = 0;
    if (!parseRfOutputCode(text, length, code, inferred)) return false;
    bits = bits ? bits : inferred ? inferred : defaultBits;
    pulseUs = pulseUs ? pulseUs : defaultPulseUs;
    repeat = repeat ? repeat : defaultRepeat;
    if (bits < 1 || bits > 32 || pulseUs < 100 || pulseUs > 2000 || repeat < 1 || repeat > 20 ||
        (bits < 32 && code >= (uint32_t(1) << bits))) return false;
    out.code = code; out.bits = static_cast<uint8_t>(bits);
    out.pulseUs = static_cast<uint16_t>(pulseUs); out.repeat = static_cast<uint8_t>(repeat);
    return true;
}
} // namespace FastBee
