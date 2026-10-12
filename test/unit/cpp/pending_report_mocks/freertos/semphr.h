#pragma once
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <cassert>
#include <mutex>
using SemaphoreHandle_t = void*;
inline std::atomic<bool> pendingForceBusy{false}, pendingCreateFail{false};
inline thread_local int pendingLockDepth = 0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    return pendingCreateFail.load() ? nullptr : static_cast<void*>(new std::mutex);
}
inline void vSemaphoreDelete(SemaphoreHandle_t h) { delete static_cast<std::mutex*>(h); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) {
    assert(wait==0);
    if(!h || pendingForceBusy.load()) return pdFALSE;
    if(!static_cast<std::mutex*>(h)->try_lock()) return pdFALSE;
    ++pendingLockDepth; return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    assert(pendingLockDepth==1);--pendingLockDepth;
    static_cast<std::mutex*>(h)->unlock();return pdTRUE;
}
