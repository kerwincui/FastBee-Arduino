// ============================================================
// 原生测试 mock: esp_log.h
// LoggerSystem.h 引用 ESP-IDF 的 vprintf_like_t 类型，
// 原生环境仅提供类型定义与常用宏桩。
// ============================================================
#ifndef FASTBEE_TEST_MOCK_ESP_LOG_H
#define FASTBEE_TEST_MOCK_ESP_LOG_H

#include <stdarg.h>

typedef int (*vprintf_like_t)(const char*, va_list);

#ifndef ESP_LOGE
#define ESP_LOGE(tag, format, ...) ((void)0)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, format, ...) ((void)0)
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(tag, format, ...) ((void)0)
#endif
#ifndef ESP_LOGD
#define ESP_LOGD(tag, format, ...) ((void)0)
#endif

#endif  // FASTBEE_TEST_MOCK_ESP_LOG_H
