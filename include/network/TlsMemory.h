#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace FastBee { namespace TlsMemory {
constexpr size_t PSRAM_MIN_ALLOC = 256U;
constexpr size_t PSRAM_RESERVE = 64U * 1024U;
// All clients share the same allocator. Both ESP-IDF's original allocator and
// this allocator release through heap_caps_free, including live older sessions.
inline void* calloc(size_t count, size_t size) {
    if (size && count > static_cast<size_t>(-1) / size) return nullptr;
    const size_t bytes=count*size;
#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
    if (bytes>=PSRAM_MIN_ALLOC && psramFound()) {
        // Large TLS records/certificates must not silently fall back to scarce
        // internal memory after admission. Reserve enough PSRAM for UI work.
        const size_t available=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (available<=PSRAM_RESERVE || bytes>=available-PSRAM_RESERVE) return nullptr;
        return heap_caps_calloc(count,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    }
#endif
    (void)bytes;
    return heap_caps_calloc(count,size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
}
inline void free(void* pointer) { heap_caps_free(pointer); }
inline bool installed=false;
inline portMUX_TYPE installMux=portMUX_INITIALIZER_UNLOCKED;
inline bool ensureAllocator() {
#if defined(MBEDTLS_PLATFORM_MEMORY)
    portENTER_CRITICAL(&installMux);
    if (!installed) installed=mbedtls_platform_set_calloc_free(calloc,free)==0;
    const bool ready=installed;
    portEXIT_CRITICAL(&installMux);
    return ready;
#else
    return false;
#endif
}
inline bool usesPsram() {
#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
    return ensureAllocator() && psramFound() &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=PSRAM_RESERVE+128U*1024U &&
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=32768U;
#else
    return false;
#endif
}
}} // namespace FastBee::TlsMemory
