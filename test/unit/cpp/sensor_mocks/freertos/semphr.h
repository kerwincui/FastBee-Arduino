#pragma once
#include <mutex>
#include <atomic>
#include "../../../../mocks/freertos/FreeRTOS.h"
using SemaphoreHandle_t = std::recursive_mutex*;
inline std::atomic<unsigned> sensorTestMutexCreates{0};
inline SemaphoreHandle_t xSemaphoreCreateMutex() { ++sensorTestMutexCreates; return new std::recursive_mutex; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t value, TickType_t wait) {
    if (!wait) return value->try_lock() ? pdTRUE : pdFALSE;
    value->lock(); return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t value) { value->unlock(); return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t value) { delete value; }
