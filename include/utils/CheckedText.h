#pragma once
#include <cstddef>
#include <climits>
#include <cstring>
#include <utility>

namespace FastBee {
// Arduino String-compatible operations. Inputs and output must not alias.
// Callers only consume output on true; no silent partial copy/concatenation.
template<class Text>
bool appendCheckedText(Text& out, const char* bytes, size_t size, size_t limit) {
    const size_t before = out.length();
    if (before > limit || size > limit - before || size > UINT_MAX ||
        (size && (!bytes || std::memchr(bytes, '\0', size)))) return false;
    if (!size) return true;
    return out.concat(bytes, static_cast<unsigned int>(size)) &&
        out.length() == before + size &&
        std::memcmp(out.c_str() + before, bytes, size) == 0;
}

template<class Text>
bool copyCheckedText(Text& out, const char* bytes, size_t size, size_t limit) {
    out = "";
    return size <= limit && size <= UINT_MAX && out.reserve(static_cast<unsigned int>(size)) &&
        appendCheckedText(out, bytes, size, limit);
}

template<class Text>
bool replaceCheckedText(const Text& input, const char* needle, const char* replacement,
                        size_t replacementSize, Text& out, size_t limit) {
    out = "";
    const size_t size = input.length(), needleSize = std::strlen(needle);
    if (!needleSize || size > limit || size > UINT_MAX ||
        std::memchr(input.c_str(), '\0', size) || replacementSize > limit ||
        (replacementSize && (!replacement || std::memchr(replacement, '\0', replacementSize)))) return false;
    size_t expected = size;
    const char* cursor = input.c_str();
    while (const char* found = std::strstr(cursor, needle)) {
        expected -= needleSize;
        if (replacementSize > limit - expected) return false;
        expected += replacementSize;
        cursor = found + needleSize;
    }
    if (expected > UINT_MAX || !out.reserve(static_cast<unsigned int>(expected))) return false;
    cursor = input.c_str();
    while (const char* found = std::strstr(cursor, needle)) {
        if (!appendCheckedText(out, cursor, static_cast<size_t>(found - cursor), limit) ||
            !appendCheckedText(out, replacement, replacementSize, limit)) return false;
        cursor = found + needleSize;
    }
    return appendCheckedText(out, cursor, size - static_cast<size_t>(cursor - input.c_str()), limit) &&
        out.length() == expected;
}
} // namespace FastBee
