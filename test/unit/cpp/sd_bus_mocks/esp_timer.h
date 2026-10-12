#pragma once
#include <atomic>
#include <cstdint>

// SD-only deterministic clock seam; never falls through to device/host time.
inline std::atomic<int64_t> sdMonotonicUs{1000000};
inline int64_t esp_timer_get_time() { return sdMonotonicUs.load(); }
