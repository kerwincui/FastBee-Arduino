#pragma once
#include <freertos/FreeRTOS.h>
#include <cassert>
using SemaphoreHandle_t = void*;
inline bool rtcMutexBusy = false;
inline int rtcLockDepth = 0;
inline TickType_t rtcLockTimeout = 0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return xSemaphoreCreateMutex(); }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return xSemaphoreCreateMutex(); }
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout) {
    rtcLockTimeout = timeout;
    if(!handle || rtcMutexBusy) return pdFALSE;
    ++rtcLockDepth; return pdTRUE;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t) {
    assert(rtcLockDepth > 0); --rtcLockDepth; return pdTRUE;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { return xSemaphoreTakeRecursive(h,t); }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { return xSemaphoreGiveRecursive(h); }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
