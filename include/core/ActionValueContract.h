#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace FastBee {
// Same decimal conversion as the existing action preflight, with explicit
// overflow and embedded-NUL rejection. Output changes only after success.
// This pure helper neither allocates nor authorizes a hardware operation.
inline bool parseStrictActionLong(const char* text, size_t length, long& value) {
    if (!text || !length || std::strlen(text) != length) return false;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (errno == ERANGE || end == text || end != text + length) return false;
    value = parsed;
    return true;
}

inline bool parseDacActionValue(const char* text, size_t length, uint8_t& value) {
    long parsed = 0;
    if (!parseStrictActionLong(text, length, parsed) || parsed < 0 || parsed > 255)
        return false;
    value = static_cast<uint8_t>(parsed);
    return true;
}
} // namespace FastBee
