#pragma once
#include "../../../mocks/esp_heap_caps.h"
// This non-PSRAM fixture must use ArduinoJson's host allocator. These methods
// keep the real allocator declaration compilable and reject accidental use.
inline void* heap_caps_malloc(size_t, uint32_t) {
    throw std::logic_error("OTA persistence test touched device allocation");
}
inline void* heap_caps_realloc(void*, size_t, uint32_t) {
    throw std::logic_error("OTA persistence test touched device reallocation");
}
inline void heap_caps_free(void*) {
    throw std::logic_error("OTA persistence test touched device free");
}
