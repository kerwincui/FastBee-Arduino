#pragma once
#include <cstddef>
#include <cstdlib>
#include "esp_memory_utils.h"
// Keep the existing motor allocator's capabilities and fault-injection path.
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
static void* heap_caps_malloc(size_t bytes, unsigned caps);
inline void heap_caps_free(void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    auto& regions = MotorMemoryMock::regions;
    for (auto it = regions.begin(); it != regions.end(); ++it) {
        if (it->start == address) { regions.erase(it); break; }
    }
    // Arduino.h's existing free fault/accounting wrapper remains in use.
    free(pointer);
}
