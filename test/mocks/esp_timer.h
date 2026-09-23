#pragma once
#include <cstdint>
#include <stdexcept>

inline int64_t esp_timer_get_time() {
    throw std::logic_error("Native schema test touched device clock telemetry");
}
