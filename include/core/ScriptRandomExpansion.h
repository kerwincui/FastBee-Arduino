#pragma once
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace FastBee {
namespace ScriptRandomDetail {
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline void trim(const char*& begin, const char*& end) {
    while (begin < end && space(*begin)) ++begin;
    while (begin < end && space(end[-1])) --end;
}
inline bool integer(const char* begin, const char* end, int32_t& value) {
    trim(begin, end);
    bool negative = false;
    if (begin < end && (*begin == '-' || *begin == '+')) negative = *begin++ == '-';
    if (begin == end) return false;
    uint64_t magnitude = 0;
    const uint64_t limit = negative ? UINT64_C(2147483648) : UINT64_C(2147483647);
    for (; begin < end; ++begin) {
        if (*begin < '0' || *begin > '9') return false;
        const unsigned digit = static_cast<unsigned>(*begin - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    value = static_cast<int32_t>(negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude));
    return true;
}
inline bool decimal(const char* begin, const char* end, double& value) {
    trim(begin, end);
    const size_t length = static_cast<size_t>(end - begin);
    if (!length || length >= 64) return false;
    const char* cursor = begin;
    if (*cursor == '+' || *cursor == '-') ++cursor;
    bool digits = false;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') { digits = true; ++cursor; }
    if (cursor < end && *cursor == '.') {
        ++cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') { digits = true; ++cursor; }
    }
    if (!digits) return false;
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
        ++cursor;
        if (cursor < end && (*cursor == '+' || *cursor == '-')) ++cursor;
        const char* exponent = cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') ++cursor;
        if (cursor == exponent) return false;
    }
    if (cursor != end) return false;
    char token[64];
    std::memcpy(token, begin, length); token[length] = '\0';
    char* parsedEnd = nullptr;
    errno = 0;
    value = std::strtod(token, &parsedEnd);
    return parsedEnd == token + length && errno != ERANGE && std::isfinite(value);
}
} // namespace ScriptRandomDetail

// Single forward pass, bounded by input/output sizes. Append may fail; callers
// must discard ALL partial output then. RNG is consumed once per expression,
// left to right. No retry on invalid syntax, truncation or allocation failure.
template<class Random, class Append>
bool expandScriptRandom(const char* input, size_t length, size_t maximum,
                        Random random, Append append) {
    if (!input || length > maximum || std::memchr(input, '\0', length)) return false;
    size_t position = 0, produced = 0;
    auto emit = [&](const char* data, size_t count) {
        if (count > maximum - produced || !append(data, count)) return false;
        produced += count; return true;
    };
    while (position < length) {
        const size_t remaining = length - position;
        const bool floating = remaining >= 8 && std::memcmp(input + position, "RANDOMF(", 8) == 0;
        const bool integral = remaining >= 7 && std::memcmp(input + position, "RANDOM(", 7) == 0;
        if (!floating && !integral) {
            if (!emit(input + position, 1)) return false;
            ++position; continue;
        }
        const size_t start = position + (floating ? 8 : 7);
        size_t close = start;
        while (close < length && input[close] != ')') ++close;
        if (close == length) return false;
        const char* fields[3] = {input + start, nullptr, nullptr};
        const char* ends[3] = {};
        size_t count = 1;
        for (size_t i = start; i < close; ++i) if (input[i] == ',') {
            if (count >= (floating ? 3u : 2u)) return false;
            ends[count - 1] = input + i;
            fields[count++] = input + i + 1;
        }
        if (count != (floating ? 3u : 2u)) return false;
        ends[count - 1] = input + close;
        char formatted[64];
        int written = 0;
        if (floating) {
            double low = 0, high = 0;
            int32_t decimals = 0;
            if (!ScriptRandomDetail::decimal(fields[0], ends[0], low) ||
                !ScriptRandomDetail::decimal(fields[1], ends[1], high) ||
                !ScriptRandomDetail::integer(fields[2], ends[2], decimals)) return false;
            if (low > high) { const double swap = low; low = high; high = swap; }
            if (decimals < 0) decimals = 0;
            if (decimals > 6) decimals = 6;
            const double fraction = static_cast<double>(random() % 1000000UL) / 1000000.0;
            // Weighted endpoints avoid overflowing high-low for opposite signs.
            const double value = low == high ? low : (1.0 - fraction) * low + fraction * high;
            if (!std::isfinite(value)) return false;
            written = std::snprintf(formatted, sizeof(formatted), "%.*f", static_cast<int>(decimals), value);
        } else {
            int32_t low = 0, high = 0;
            if (!ScriptRandomDetail::integer(fields[0], ends[0], low) ||
                !ScriptRandomDetail::integer(fields[1], ends[1], high)) return false;
            if (low > high) { const int32_t swap = low; low = high; high = swap; }
            // Full signed-32-bit range has 2^32 values, not zero.
            const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(high) - low) + 1;
            const int64_t value = static_cast<int64_t>(low) +
                static_cast<int64_t>(static_cast<uint64_t>(random()) % width);
            written = std::snprintf(formatted, sizeof(formatted), "%lld", static_cast<long long>(value));
        }
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(formatted) ||
            !emit(formatted, static_cast<size_t>(written))) return false;
        position = close + 1;
    }
    return true;
}
} // namespace FastBee
