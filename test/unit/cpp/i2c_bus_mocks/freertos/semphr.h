#pragma once
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
using SemaphoreHandle_t = void*;
inline std::atomic<unsigned> semaphoreCreates{0}, semaphoreDeletes{0};
inline std::atomic<TickType_t> semaphoreLastWait{0};
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    auto* result = new std::recursive_timed_mutex;
    ++semaphoreCreates;
    // Force both first callers to allocate before either may publish its mutex.
    while (semaphoreCreates.load() < 2) std::this_thread::yield();
    return result;
}
inline void vSemaphoreDelete(SemaphoreHandle_t handle) {
    ++semaphoreDeletes;
    delete static_cast<std::recursive_timed_mutex*>(handle);
}
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout) {
    semaphoreLastWait = timeout;
    return static_cast<std::recursive_timed_mutex*>(handle)->try_lock_for(
        std::chrono::milliseconds(timeout)) ? pdTRUE : pdFALSE;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
    static_cast<std::recursive_timed_mutex*>(handle)->unlock();
    return pdTRUE;
}
