#ifndef FREERTOS_MOCK_H
#define FREERTOS_MOCK_H

#include <stdint.h>
#include <stddef.h>

#define pdTRUE         1
#define pdFALSE        0
#define pdPASS         1
#define pdFAIL         0
#define portMAX_DELAY  0xFFFFFFFFUL

#define pdMS_TO_TICKS(ms)  ((ms) / portTICK_PERIOD_MS)
#define portTICK_PERIOD_MS 1

#define configMAX_PRIORITIES 25
#define tskNO_AFFINITY       0x7FFFFFFF

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;

#endif // FREERTOS_MOCK_H
