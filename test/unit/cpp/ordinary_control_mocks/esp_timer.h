#pragma once
#include <cstdint>
#include <cassert>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;
using esp_timer_cb_t = void (*)(void*);
struct MockOrdinaryTimer { esp_timer_cb_t callback; void* argument; bool running = false; };
using esp_timer_handle_t = MockOrdinaryTimer*;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
    esp_timer_cb_t callback = nullptr;
    void* arg = nullptr;
    int dispatch_method = 0;
    const char* name = nullptr;
};
inline int64_t ordinaryClockUs = 1000000;
inline bool timerCreateOk = true, timerStartOk = true, timerStopOk = true, timerDeleteOk = true;
inline bool timerPartialCreate = false;
inline unsigned timerCreates = 0, timerStarts = 0, timerStops = 0, timerDeletes = 0, timerHandles = 0;
inline esp_timer_cb_t lastTimerCallback = nullptr;
inline void* lastTimerArgument = nullptr;
inline int64_t esp_timer_get_time() { return ordinaryClockUs; }
inline esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* handle) {
    ++timerCreates;
    if (timerCreateOk || timerPartialCreate) {
        *handle = new MockOrdinaryTimer{args->callback, args->arg, false}; ++timerHandles;
        lastTimerCallback = args->callback; lastTimerArgument = args->arg;
    }
    return timerCreateOk ? ESP_OK : ESP_FAIL;
}
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t intervalUs) {
    assert(timer && intervalUs >= 1000 && intervalUs <= 65535000);
    ++timerStarts; if (!timerStartOk) return ESP_FAIL;
    timer->running = true; return ESP_OK;
}
inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    assert(timer); ++timerStops; if (!timerStopOk) return ESP_FAIL;
    if (!timer->running) return ESP_ERR_INVALID_STATE;
    timer->running = false; return ESP_OK;
}
inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    assert(timer); ++timerDeletes; if (!timerDeleteOk) return ESP_FAIL;
    assert(!timer->running); delete timer; --timerHandles; return ESP_OK;
}
