#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mutex>
#include <chrono>
#include <cassert>
#include <cstdlib>

constexpr unsigned MALLOC_CAP_INTERNAL = 1U, MALLOC_CAP_8BIT = 2U;
inline bool adcTestTableFails = false;
inline unsigned adcTestTableAttempts = 0, adcTestTableAllocations = 0, adcTestTableFrees = 0;
inline size_t adcTestTableBytes = 0;
inline void* adcTestTableStorage = nullptr;
inline void* heap_caps_malloc(size_t bytes, unsigned capabilities) {
    assert(capabilities == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++adcTestTableAttempts;
    if (adcTestTableFails) return nullptr;
    assert(!adcTestTableStorage);
    adcTestTableStorage = std::malloc(bytes);
    if (adcTestTableStorage) {
        ++adcTestTableAllocations;
        adcTestTableBytes = bytes;
    }
    return adcTestTableStorage;
}
inline void heap_caps_free(void* memory) {
    assert(memory && memory == adcTestTableStorage);
    ++adcTestTableFrees;
    std::free(memory);
    adcTestTableStorage = nullptr;
    adcTestTableBytes = 0;
}

#ifndef ADC_TEST_UNITS
#define ADC_TEST_UNITS 2
#endif
#ifndef ADC_TEST_CHANNELS
#define ADC_TEST_CHANNELS 10
#endif
#define SOC_ADC_PERIPH_NUM ADC_TEST_UNITS
#define SOC_ADC_MAX_CHANNEL_NUM ADC_TEST_CHANNELS
#if defined(ADC_TEST_C3)
#define SOC_ADC_CHANNEL_NUM(u) ((u) == 0 ? 5 : 1)
#else
#define SOC_ADC_CHANNEL_NUM(u) ADC_TEST_CHANNELS
#endif
#ifndef SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED
#define SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED 0
#endif
#ifndef ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
#endif
#ifndef ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
#define ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED 0
#endif
using esp_err_t = int;
using adc_unit_t = int;
using adc_channel_t = int;
using adc_atten_t = int;
using adc_bitwidth_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NOT_SUPPORTED = 1, ESP_ERR_INVALID_ARG = 2,
    ESP_ERR_NO_MEM = 3, ESP_ERR_NOT_FOUND = 4, ESP_ERR_TIMEOUT = 5, ESP_ERR_INVALID_STATE = 6;
constexpr int ADC_UNIT_1 = 0, ADC_UNIT_2 = 1, ADC_CHANNEL_0 = 0,
    ADC_BITWIDTH_12 = 12, ADC_ULP_MODE_DISABLE = 0;
struct UnitHandle { int unit; int attenuation[ADC_TEST_CHANNELS]{}; };
using adc_oneshot_unit_handle_t = UnitHandle*;
struct CalibrationHandle { int unit, channel, attenuation; };
using adc_cali_handle_t = CalibrationHandle*;
struct adc_oneshot_unit_init_cfg_t { int unit_id = 0, clk_src = 0, ulp_mode = 0; };
struct adc_oneshot_chan_cfg_t { int atten = 0, bitwidth = 0; };
struct adc_cali_curve_fitting_config_t { int unit_id = 0, chan = 0, atten = 0, bitwidth = 0; };
struct adc_cali_line_fitting_config_t { int unit_id = 0, atten = 0, bitwidth = 0, default_vref = 0; };
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*, adc_oneshot_unit_handle_t*);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t*);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int*);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
esp_err_t adc_oneshot_io_to_channel(int, adc_unit_t*, adc_channel_t*);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*, adc_cali_handle_t*);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t*, adc_cali_handle_t*);
esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t);
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int, int*);
using SemaphoreHandle_t = std::timed_mutex*;
using TickType_t = uint32_t;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(value) (value)
extern bool adcTestMutexFails;
extern unsigned adcTestMutexCreates;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    ++adcTestMutexCreates;
    return adcTestMutexFails ? nullptr : new std::timed_mutex;
}
inline void vSemaphoreDelete(SemaphoreHandle_t value) { delete value; }
inline int xSemaphoreTake(SemaphoreHandle_t value, TickType_t ticks) {
    return value->try_lock_for(std::chrono::milliseconds(ticks));
}
inline void xSemaphoreGive(SemaphoreHandle_t value) { value->unlock(); }
inline void delayMicroseconds(unsigned) {}
enum peripheral_bus_type_t { ESP32_BUS_TYPE_INIT, ESP32_BUS_TYPE_GPIO, ESP32_BUS_TYPE_RMT_TX };
extern bool adcTestForeignPin;
inline peripheral_bus_type_t perimanGetPinBusType(uint8_t) {
    return adcTestForeignPin ? ESP32_BUS_TYPE_RMT_TX : ESP32_BUS_TYPE_INIT;
}
