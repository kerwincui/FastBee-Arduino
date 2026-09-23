#pragma once
#include <mutex>
#include "../../../../mocks/freertos/FreeRTOS.h"
using SemaphoreHandle_t = std::recursive_mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_mutex; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t value, TickType_t) { value->lock(); return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t value) { value->unlock(); return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t value) { delete value; }
