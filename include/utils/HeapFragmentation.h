#ifndef HEAP_FRAGMENTATION_H
#define HEAP_FRAGMENTATION_H

#include <stdint.h>

static inline uint8_t calculateHeapFragmentationPercent(uint32_t freeHeap,
                                                         uint32_t largestFreeBlock) {
    if (freeHeap == 0 || largestFreeBlock >= freeHeap) {
        return 0;
    }

    uint32_t usedPercent = (largestFreeBlock * 100U) / freeHeap;
    if (usedPercent >= 100U) {
        return 0;
    }

    uint32_t fragmentation = 100U - usedPercent;
    return static_cast<uint8_t>(fragmentation > 100U ? 100U : fragmentation);
}

#endif
