#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace FastBee {
// Registers are already decoded from the RTU big-endian bytes. wordOrder only
// chooses the order of the two registers of a 32-bit value; it never swaps bytes.
inline bool decodeModbusRegisterValue(const uint16_t* words, size_t count,
                                     size_t offset, uint8_t type, uint8_t wordOrder,
                                     double scale, double& value) {
    if (!words || type > 4 || wordOrder > 1 || (type < 2 && wordOrder) ||
        !std::isfinite(scale) || std::fabs(scale) > std::numeric_limits<float>::max() ||
        offset >= count || (type >= 2 && count - offset < 2)) return false;
    double raw = words[offset];
    if (type == 1 && words[offset] >= 0x8000U) raw -= 65536.0;
    if (type >= 2) {
        const uint32_t bits = (uint32_t(words[offset + wordOrder]) << 16) |
                              words[offset + 1 - wordOrder];
        if (type == 2) raw = bits;
        else if (type == 3) raw = bits >= 0x80000000UL ? double(bits) - 4294967296.0 : double(bits);
        else {
            static_assert(sizeof(float) == sizeof(bits) && std::numeric_limits<float>::is_iec559,
                          "Modbus float32 requires IEEE 754 binary32");
            float decoded;
            std::memcpy(&decoded, &bits, sizeof(decoded));
            raw = decoded;
        }
    }
    const double candidate = raw * scale;
    if (!std::isfinite(raw) || !std::isfinite(candidate)) return false;
    value = candidate; // Failure never fabricates a valid zero.
    return true;
}

template<class Mapping>
inline bool decodeModbusMapping(const uint16_t* words, size_t count, const Mapping& mapping, double& value) {
    return mapping.decimalPlaces <= 6 && decodeModbusRegisterValue(words, count, mapping.regOffset,
        mapping.dataType, mapping.wordOrder, mapping.scaleFactor, value);
}

// Exact fixed decimal conversion, without newlib printf/dtoa or heap allocation.
// Mapping values are bounded by float32 * float32 (< 2^256); ten limbs also
// cover the six decimal places. Round halfway values to even, as printf does.
inline bool formatModbusFixed(double value, uint8_t decimals, char* output, size_t capacity) {
    if (!output || !capacity || decimals > 6 || !std::isfinite(value)) return false;
    const bool negative = std::signbit(value);
    int exponent = 0;
    const double fraction = std::frexp(std::fabs(value), &exponent);
    const uint64_t significand = static_cast<uint64_t>(std::ldexp(fraction, 53));
    uint32_t limbs[10] = {static_cast<uint32_t>(significand), static_cast<uint32_t>(significand >> 32)};
    uint32_t power = 1;
    for (uint8_t digit = 0; digit < decimals; ++digit) power *= 5;
    uint64_t carry = 0;
    for (auto& limb : limbs) {
        const uint64_t product = uint64_t(limb) * power + carry;
        limb = static_cast<uint32_t>(product);
        carry = product >> 32;
    }
    exponent += int(decimals) - 53;
    if (exponent > 320) return false;
    while (exponent > 0) {
        carry = 0;
        for (auto& limb : limbs) {
            const uint64_t shifted = (uint64_t(limb) << 1) | carry;
            limb = static_cast<uint32_t>(shifted);
            carry = shifted >> 32;
        }
        if (carry) return false;
        --exponent;
    }
    bool sticky = false;
    bool halfway = false;
    // Tiny values round to zero; at most 320 bits can be significant.
    if (exponent < -320) {
        for (auto& limb : limbs) limb = 0;
    } else {
        while (exponent < 0) {
            sticky = sticky || halfway;
            halfway = (limbs[0] & 1U) != 0;
            for (size_t index = 0; index < 10; ++index)
                limbs[index] = (limbs[index] >> 1) |
                    (index + 1 < 10 ? limbs[index + 1] << 31 : 0U);
            ++exponent;
        }
        if (halfway && (sticky || (limbs[0] & 1U))) {
            carry = 1;
            for (auto& limb : limbs) {
                const uint64_t sum = uint64_t(limb) + carry;
                limb = static_cast<uint32_t>(sum);
                carry = sum >> 32;
            }
            if (carry) return false;
        }
    }
    char reversed[97];
    size_t digits = 0;
    bool remaining;
    do {
        uint64_t remainder = 0;
        remaining = false;
        for (size_t index = 10; index-- > 0;) {
            const uint64_t dividend = (remainder << 32) | limbs[index];
            limbs[index] = static_cast<uint32_t>(dividend / 10);
            remainder = dividend % 10;
            remaining = remaining || limbs[index] != 0;
        }
        if (digits == sizeof(reversed)) return false;
        reversed[digits++] = char('0' + remainder);
    } while (remaining);
    const size_t paddedDigits = std::max<size_t>(digits, size_t(decimals) + 1);
    const size_t length = paddedDigits + (decimals ? 1 : 0) + (negative ? 1 : 0);
    if (length >= capacity) return false;
    size_t cursor = 0;
    if (negative) output[cursor++] = '-';
    for (size_t position = paddedDigits; position-- > 0;) {
        output[cursor++] = position < digits ? reversed[position] : '0';
        if (decimals && position == decimals) output[cursor++] = '.';
    }
    output[cursor] = '\0';
    return true;
}

template<class Mapping>
inline bool formatModbusMapping(const uint16_t* words, size_t count, const Mapping& mapping,
                               char* output, size_t capacity) {
    double value;
    if (!output || !capacity || !decodeModbusMapping(words, count, mapping, value)) return false;
    return formatModbusFixed(value, mapping.decimalPlaces, output, capacity);
}
}
