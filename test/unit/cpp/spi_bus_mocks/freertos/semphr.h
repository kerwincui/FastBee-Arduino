#pragma once
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
using SemaphoreHandle_t = void*;
inline std::atomic<int> spiMutexCreates{0};
inline std::atomic<int> spiMutexDeletes{0};
inline std::atomic<bool> spiMutexFailAllocation{false};
inline std::atomic<bool> spiMutexFailTake{false};
inline std::atomic<bool> spiMutexRace{false};
inline std::atomic<int> spiMutexTakes{0};
inline std::atomic<TickType_t> spiMutexLastWait{0};
inline std::atomic<TickType_t> spiMutexLastRequest{0};
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    if (spiMutexFailAllocation.load()) return nullptr;
    auto* mutex = new std::recursive_timed_mutex();
    ++spiMutexCreates;
    while (spiMutexRace.load() && spiMutexCreates.load() < 2) std::this_thread::yield();
    return mutex;
}
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) {
    ++spiMutexDeletes;
    delete static_cast<std::recursive_timed_mutex*>(mutex);
}
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
    ++spiMutexTakes;
    spiMutexLastRequest = ticks;
    if (spiMutexFailTake.load()) return pdFALSE;
    const bool acquired = static_cast<std::recursive_timed_mutex*>(mutex)->try_lock_for(std::chrono::milliseconds(ticks));
    if (acquired) spiMutexLastWait = ticks;
    return acquired ? pdTRUE : pdFALSE;
}
inline int xSemaphoreGive(SemaphoreHandle_t mutex) {
    static_cast<std::recursive_timed_mutex*>(mutex)->unlock();
    return pdTRUE;
}
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return xSemaphoreCreateMutex(); }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t ticks) { return xSemaphoreTake(mutex, ticks); }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) { return xSemaphoreGive(mutex); }
