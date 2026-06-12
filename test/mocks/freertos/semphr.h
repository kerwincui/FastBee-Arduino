#ifndef SEMAPHORE_MOCK_H
#define SEMAPHORE_MOCK_H

#include "FreeRTOS.h"

typedef void* SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return nullptr; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return nullptr; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore,
                                  TickType_t xTicksToWait)
{
    (void)xSemaphore; (void)xTicksToWait;
    return pdPASS;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    (void)xSemaphore;
    return pdPASS;
}
inline void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    (void)xSemaphore;
}

inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore,
                                          TickType_t xTicksToWait)
{
    (void)xSemaphore; (void)xTicksToWait;
    return pdPASS;
}

inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore)
{
    (void)xSemaphore;
    return pdPASS;
}

#endif // SEMAPHORE_MOCK_H
