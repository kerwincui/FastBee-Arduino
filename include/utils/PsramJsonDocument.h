#pragma once

#include "core/FeatureFlags.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

namespace FastBee {

class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }

    void* reallocate(void* ptr, size_t newSize) override {
        return heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

inline bool psramAvailableForJson(size_t reserveBytes = 8192) {
#if FASTBEE_USE_PSRAM
    return ESP.getPsramSize() > 0 && ESP.getFreePsram() > reserveBytes;
#else
    (void)reserveBytes;
    return false;
#endif
}

inline JsonDocument makeJsonDocument(size_t psramReserveBytes = 8192) {
#if FASTBEE_USE_PSRAM
    static PsramJsonAllocator allocator;
    if (psramAvailableForJson(psramReserveBytes)) {
        return JsonDocument(&allocator);
    }
#else
    (void)psramReserveBytes;
#endif
    return JsonDocument();
}

} // namespace FastBee
