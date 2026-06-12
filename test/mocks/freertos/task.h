#ifndef TASK_MOCK_H
#define TASK_MOCK_H

#include "FreeRTOS.h"

typedef void* TaskHandle_t;

inline BaseType_t xTaskCreatePinnedToCore(
    void (*pvTaskCode)(void*),
    const char* const pcName,
    const uint32_t usStackDepth,
    void* const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t* const pxCreatedTask,
    const BaseType_t xCoreID)
{
    (void)pvTaskCode; (void)pcName; (void)usStackDepth;
    (void)pvParameters; (void)uxPriority; (void)pxCreatedTask; (void)xCoreID;
    return pdPASS;
}

inline BaseType_t xTaskCreate(
    void (*pvTaskCode)(void*),
    const char* const pcName,
    const uint32_t usStackDepth,
    void* const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t* const pxCreatedTask)
{
    (void)pvTaskCode; (void)pcName; (void)usStackDepth;
    (void)pvParameters; (void)uxPriority; (void)pxCreatedTask;
    return pdPASS;
}

inline void vTaskDelete(TaskHandle_t xTask) { (void)xTask; }

#endif // TASK_MOCK_H
