#pragma once
#include <Arduino.h>
inline uint64_t sensorTestWrapBase = 0;
inline int64_t esp_timer_get_time() {
    return static_cast<int64_t>(sensorTestWrapBase + sensorTestNow) * 1000;
}
