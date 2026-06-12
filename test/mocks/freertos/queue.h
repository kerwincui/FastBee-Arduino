#ifndef QUEUE_MOCK_H
#define QUEUE_MOCK_H

#include "FreeRTOS.h"

typedef void* QueueHandle_t;

inline QueueHandle_t xQueueCreate(const UBaseType_t uxQueueLength,
                                   const UBaseType_t uxItemSize)
{
    (void)uxQueueLength; (void)uxItemSize;
    return nullptr;
}

inline BaseType_t xQueueSend(QueueHandle_t xQueue,
                              const void* pvItemToQueue,
                              TickType_t xTicksToWait)
{
    (void)xQueue; (void)pvItemToQueue; (void)xTicksToWait;
    return pdPASS;
}

inline BaseType_t xQueueReceive(QueueHandle_t xQueue,
                                 void* pvBuffer,
                                 TickType_t xTicksToWait)
{
    (void)xQueue; (void)pvBuffer; (void)xTicksToWait;
    return pdPASS;
}

inline UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t xQueue)
{
    (void)xQueue;
    return 0;
}

#endif // QUEUE_MOCK_H
