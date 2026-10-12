#pragma once
#include <cstdint>
#include <stdexcept>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
using esp_timer_cb_t = void (*)(void*);
using esp_timer_handle_t = void*;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
    esp_timer_cb_t callback = nullptr;
    void* arg = nullptr;
    int dispatch_method = ESP_TIMER_TASK;
    const char* name = nullptr;
};
inline esp_err_t esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t*) {
    throw std::logic_error("Native schema test created device timer");
}
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t) {
    throw std::logic_error("Native schema test started device timer");
}
inline esp_err_t esp_timer_stop(esp_timer_handle_t) {
    throw std::logic_error("Native schema test stopped device timer");
}
inline esp_err_t esp_timer_delete(esp_timer_handle_t) {
    throw std::logic_error("Native schema test deleted device timer");
}

inline int64_t esp_timer_get_time() {
    throw std::logic_error("Native schema test touched device clock telemetry");
}
