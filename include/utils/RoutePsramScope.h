#pragma once

#include "core/FeatureFlags.h"
#include <esp_heap_caps.h>

namespace FastBee {

// Route objects/callbacks are small, long-lived, and never used from an ISR.
// Keep their allocations out of internal DRAM during boot registration. Task
// stacks, DMA and other explicit INTERNAL allocations retain their normal caps.
class RoutePsramScope {
public:
    RoutePsramScope() {
#if FASTBEE_USE_PSRAM
        _enabled = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= 64U * 1024U;
        // Route list nodes and short URI/callback allocations are often only
        // 12-32 bytes. A 32-byte cutoff leaves hundreds of these long-lived
        // records in DRAM, consuming the headroom needed by concurrent TCP
        // requests. Include every size while registering routes; explicit
        // INTERNAL/DMA allocations are unaffected and runtime policy is
        // restored when initialize() returns.
        if (_enabled) heap_caps_malloc_extmem_enable(0);
#endif
    }
    ~RoutePsramScope() {
#if FASTBEE_USE_PSRAM
        // Restore the normal allocation policy established in main.cpp.
        if (_enabled) heap_caps_malloc_extmem_enable(256);
#endif
    }
    RoutePsramScope(const RoutePsramScope&) = delete;
    RoutePsramScope& operator=(const RoutePsramScope&) = delete;
private:
    bool _enabled = false;
};

} // namespace FastBee
