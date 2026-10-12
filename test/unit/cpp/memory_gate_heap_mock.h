#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Distinct pools make both a PSRAM query and a bare INTERNAL query appear
// healthy when byte-addressable internal RAM is actually exhausted.
constexpr uint32_t MALLOC_CAP_8BIT = 1U << 2;
constexpr uint32_t MALLOC_CAP_INTERNAL = 1U << 11;
constexpr uint32_t MALLOC_CAP_DEFAULT = 1U << 12;
inline size_t internalFree = 32768;
inline size_t internalLargest = 32768;
inline size_t internal32Free = 65536;
inline size_t internal32Largest = 32768;
inline size_t psramFree = 8U * 1024U * 1024U;
inline size_t psramLargest = 4U * 1024U * 1024U;

inline size_t heap_caps_get_free_size(uint32_t caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return internalFree;
    if (caps == MALLOC_CAP_INTERNAL) return internalFree + internal32Free;
    return internalFree + psramFree;
}

inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return internalLargest;
    if (caps == MALLOC_CAP_INTERNAL) return std::max(internalLargest, internal32Largest);
    return std::max(internalLargest, psramLargest);
}
