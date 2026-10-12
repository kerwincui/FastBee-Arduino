#pragma once
// Retain the shared telemetry tripwires; allocation support is TFT-local only.
#include "../../../mocks/esp_heap_caps.h"
#include <cassert>
#include <cstdlib>

inline bool tftSpiHeapFail = false;
inline unsigned tftSpiHeapAttempts = 0, tftSpiHeapAllocations = 0, tftSpiHeapFrees = 0;
inline void* tftSpiHeapStorage = nullptr;
inline void* heap_caps_malloc(size_t bytes, uint32_t caps) {
    assert(bytes > 0 && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++tftSpiHeapAttempts;
    if (tftSpiHeapFail) return nullptr;
    assert(!tftSpiHeapStorage);
    tftSpiHeapStorage = ::malloc(bytes);
    if (tftSpiHeapStorage) ++tftSpiHeapAllocations;
    return tftSpiHeapStorage;
}
inline void heap_caps_free(void* memory) {
    assert(memory && memory == tftSpiHeapStorage);
    ++tftSpiHeapFrees;
    ::free(memory);
    tftSpiHeapStorage = nullptr;
}
