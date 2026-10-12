#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <mutex>
#include <chrono>
struct AdsTestMutex { std::recursive_timed_mutex mutex; bool bus = false; };
using SemaphoreHandle_t = AdsTestMutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new AdsTestMutex; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    auto* mutex = new AdsTestMutex; mutex->bus = true; return mutex;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
    if (!mutex->bus && adsTestDenyCacheLock) return pdFALSE;
    const bool ok = timeout ? mutex->mutex.try_lock_for(std::chrono::milliseconds(timeout)) : mutex->mutex.try_lock();
    if (ok && mutex->bus) ++sensorTestBusLockDepth;
    if (ok && !mutex->bus) ++adsTestCacheLockDepth;
    return ok ? pdTRUE : pdFALSE;
}
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t t) { return xSemaphoreTake(m, t); }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
    if (mutex->bus) --sensorTestBusLockDepth;
    else --adsTestCacheLockDepth;
    mutex->mutex.unlock(); return pdTRUE;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m) { return xSemaphoreGive(m); }
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
