#pragma once
#include <cassert>
#include <cstdlib>
#include "esp_memory_utils.h"
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
inline void* heap_caps_malloc(size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) && size == 4);
    ++AtomicSdkMock::allocations;
    if (AtomicSdkMock::failAllocation) return nullptr;
    void* result = (std::malloc)(size);
    assert(result);
    if (!AtomicSdkMock::wrongRegion)
        AtomicSdkMock::internalAllocations[reinterpret_cast<uintptr_t>(result)] = size;
    return result;
}
inline void heap_caps_free(void* pointer) {
    ++AtomicSdkMock::frees;
    AtomicSdkMock::internalAllocations.erase(reinterpret_cast<uintptr_t>(pointer));
    (std::free)(pointer);
}
