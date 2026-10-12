#pragma once
#include <cstdint>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace FastBee {

// Pool capabilities overlap. These samples cannot be added together or used
// to attribute memory to a feature. The allocator visits heaps sequentially.
struct MemoryPoolSample {
    bool observed = false;
    uint32_t freeBytes = 0;
    uint32_t minimumFreeBytes = 0;
    uint32_t largestBlockBytes = 0;
};

struct MemoryPoolSnapshot {
    MemoryPoolSample internal8;
    MemoryPoolSample internalDma;
    MemoryPoolSample psram8;
};

inline MemoryPoolSnapshot readMemoryPoolSnapshot() {
    MemoryPoolSnapshot result;
#if defined(ESP_PLATFORM)
    const auto read = [](uint32_t caps) {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, caps);
        return MemoryPoolSample{true, static_cast<uint32_t>(info.total_free_bytes),
            static_cast<uint32_t>(info.minimum_free_bytes),
            static_cast<uint32_t>(info.largest_free_block)};
    };
    result.internal8 = read(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    result.internalDma = read(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    result.psram8 = read(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    return result;
}

}  // namespace FastBee
