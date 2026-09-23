#pragma once
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <chrono>
#include <mutex>
using SemaphoreHandle_t = void*;
inline std::atomic<TickType_t> lastSemaphoreTimeout{0};
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new std::recursive_timed_mutex; }
inline void vSemaphoreDelete(SemaphoreHandle_t handle) {
    delete static_cast<std::recursive_timed_mutex*>(handle);
}
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout) {
    lastSemaphoreTimeout = timeout;
    return static_cast<std::recursive_timed_mutex*>(handle)->try_lock_for(
        std::chrono::milliseconds(timeout)) ? pdTRUE : pdFALSE;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
    static_cast<std::recursive_timed_mutex*>(handle)->unlock();
    return pdTRUE;
}
