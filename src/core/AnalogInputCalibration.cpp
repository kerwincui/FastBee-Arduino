#include "core/AnalogInputCalibration.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <math.h>

namespace FastBee {
namespace {
struct CalibrationSlot {
    adc_cali_handle_t handle = nullptr;
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    uint8_t attenuation = 0;
};
// Bounded cache: ordinary channels share a handle; per-channel compensation
// chips retain channel identity. Config changes cannot grow heap indefinitely.
CalibrationSlot slots[8];
uint8_t replacement = 0;

void releaseCalibration(adc_cali_handle_t handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(handle);
#else
    (void)handle;
#endif
}
}

bool calibratedAdcVoltage(uint8_t pin, uint8_t attenuation, uint8_t resolution,
                          uint16_t raw, float& volts) {
    volts = NAN;
    if (attenuation > 3 || resolution < 9 || resolution > 12 || raw >= (1U << resolution)) return false;
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_oneshot_io_to_channel(pin, &unit, &channel) != ESP_OK) return false;
#if !SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED
    channel = ADC_CHANNEL_0;
#endif
    CalibrationSlot* selected = nullptr;
    CalibrationSlot* empty = nullptr;
    for (auto& slot : slots) {
        if (slot.handle && slot.unit == unit && slot.channel == channel && slot.attenuation == attenuation) {
            selected = &slot;
            break;
        }
        if (!slot.handle && !empty) empty = &slot;
    }
    if (!selected) {
        selected = empty ? empty : &slots[replacement++ % 8];
        if (selected->handle) releaseCalibration(selected->handle);
        selected->handle = nullptr;
        esp_err_t result = ESP_ERR_NOT_SUPPORTED;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t calibration = {};
        calibration.unit_id = unit;
        calibration.chan = channel;
        calibration.atten = static_cast<adc_atten_t>(attenuation);
        calibration.bitwidth = ADC_BITWIDTH_12;
        result = adc_cali_create_scheme_curve_fitting(&calibration, &selected->handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_line_fitting_config_t calibration = {};
        calibration.unit_id = unit;
        calibration.atten = static_cast<adc_atten_t>(attenuation);
        calibration.bitwidth = ADC_BITWIDTH_12;
        result = adc_cali_create_scheme_line_fitting(&calibration, &selected->handle);
#endif
        if (result != ESP_OK || !selected->handle) return false;
        selected->unit = unit;
        selected->channel = channel;
        selected->attenuation = attenuation;
    }
    // Arduino's public raw result is right-shifted for reduced resolutions.
    // The calibration curve is created at 12 bits on the supported targets.
    const int raw12 = static_cast<int>(raw) << (12 - resolution);
    int millivolts = 0;
    if (adc_cali_raw_to_voltage(selected->handle, raw12, &millivolts) != ESP_OK || millivolts < 0) return false;
    volts = static_cast<float>(millivolts) / 1000.0f;
    return true;
}
}
