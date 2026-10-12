#pragma once
#define millis fastbee_test_realtime_millis
#define delay fastbee_test_realtime_delay
#include "../../../mocks/Arduino.h"
#undef millis
#undef delay
#include <cmath>
using std::isnan;
using std::isinf;
inline uint32_t sensorTestNow = 1000;
inline int sensorTestBusLockDepth = 0;
inline int adsTestCacheLockDepth = 0;
inline bool adsTestDenyCacheLock = false;
inline bool sensorTestWaitedWithBusLock = false;
inline void (*adsTestDelayHook)() = nullptr;
inline unsigned long millis() { return sensorTestNow; }
inline void delay(unsigned long milliseconds) {
    if (sensorTestBusLockDepth) sensorTestWaitedWithBusLock = true;
    sensorTestNow += static_cast<uint32_t>(milliseconds);
    if (adsTestDelayHook) adsTestDelayHook();
}
constexpr int SDA = 21;
constexpr int SCL = 22;
