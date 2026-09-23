#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

constexpr uint32_t MALLOC_CAP_INTERNAL = 1;
constexpr uint32_t MALLOC_CAP_8BIT = 2;
constexpr uint32_t MALLOC_CAP_SPIRAM = 4;
inline size_t heap_caps_get_free_size(uint32_t) {
    throw std::logic_error("Native schema test touched device heap telemetry");
}
inline size_t heap_caps_get_largest_free_block(uint32_t) {
    throw std::logic_error("Native schema test touched device heap telemetry");
}
