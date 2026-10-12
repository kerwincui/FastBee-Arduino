#pragma once
#include "core/PeripheralConfig.h"
#include "core/PeripheralExecution.h"

namespace FastBee {
namespace PeripheralActionContract {
// Shared pure target contract for preflight and saved/imported rules.
// This does not admit a runtime instance or replace command/value validation.
// Preserve action IDs and the existing save-path safety restrictions.
inline bool requiresPeripheralTarget(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
        case ExecActionType::ACTION_CALL_PERIPHERAL:
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
        case ExecActionType::ACTION_STEPPER_CONTROL:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
        case ExecActionType::ACTION_SENSOR_READ:
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
        case ExecActionType::ACTION_OLED_DISPLAY:
        case ExecActionType::ACTION_RF_SEND:
        case ExecActionType::ACTION_UART_SEND:
        case ExecActionType::ACTION_RTC_CONTROL:
        case ExecActionType::ACTION_RTC_READ:
        case ExecActionType::ACTION_RTC_SET:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_SD_STATUS:
        case ExecActionType::ACTION_THERMOSTAT:
        case ExecActionType::ACTION_SD_RECORD:
            return true;
        default:
            return false;
    }
}

inline bool isSensorTarget(PeripheralType type) {
    return isGpioDigitalInputType(type) ||
           type == PeripheralType::GPIO_ANALOG_INPUT ||
           type == PeripheralType::ADC ||
           type == PeripheralType::SENSOR_GENERIC ||
           type == PeripheralType::ONE_WIRE ||
           type == PeripheralType::ENCODER ||
           type == PeripheralType::RADAR_SENSOR ||
           type == PeripheralType::RF_MODULE;
}

inline bool matchesPeripheralTarget(uint8_t actionType,
                                          const PeripheralConfig& target) {
    if (target.isExpanderChannel()) {
        const auto action = static_cast<ExecActionType>(actionType);
        if (action == ExecActionType::ACTION_SET_PWM || action == ExecActionType::ACTION_SET_DAC ||
            action == ExecActionType::ACTION_BREATHE) return false;
    }
    const PeripheralType type = target.type;
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_THERMOSTAT:
            return type == PeripheralType::GPIO_DIGITAL_OUTPUT && !target.isExpanderChannel() &&
                   target.pinCount == 1 && !target.params.gpio.passive &&
                   !target.params.gpio.invertLogic && target.params.gpio.initialState == GPIOState::STATE_HIGH;
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_GPIO_TOGGLE:
            return type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
                   type == PeripheralType::GPIO_PWM_OUTPUT ||
                   type == PeripheralType::GPIO_ANALOG_OUTPUT;
        case ExecActionType::ACTION_BLINK:
            return type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
                   type == PeripheralType::GPIO_PWM_OUTPUT;
        case ExecActionType::ACTION_BREATHE:
            return type == PeripheralType::GPIO_PWM_OUTPUT ||
                   type == PeripheralType::GPIO_ANALOG_OUTPUT;
        case ExecActionType::ACTION_SET_PWM:
            return type == PeripheralType::GPIO_PWM_OUTPUT ||
                   type == PeripheralType::GPIO_ANALOG_OUTPUT ||
                   type == PeripheralType::PWM_SERVO ||
                   (type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
                    target.params.gpio.passive);
        case ExecActionType::ACTION_SET_DAC:
            return type == PeripheralType::DAC;
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
            return type == PeripheralType::NEO_PIXEL;
        case ExecActionType::ACTION_STEPPER_CONTROL:
            return type == PeripheralType::STEPPER_MOTOR;
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
            return target.isModbusPeripheral();
        case ExecActionType::ACTION_SENSOR_READ:
            return isSensorTarget(type);
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
            return type == PeripheralType::SEVEN_SEGMENT_TM1637;
        case ExecActionType::ACTION_OLED_DISPLAY:
            return type == PeripheralType::LCD;
        case ExecActionType::ACTION_RF_SEND:
            return type == PeripheralType::RF_MODULE;
        case ExecActionType::ACTION_UART_SEND:
            return type == PeripheralType::UART;
        case ExecActionType::ACTION_RTC_CONTROL:
        case ExecActionType::ACTION_RTC_READ:
        case ExecActionType::ACTION_RTC_SET:
            return type == PeripheralType::DS1302_RTC;
        case ExecActionType::ACTION_SD_STATUS:
        case ExecActionType::ACTION_SD_RECORD:
            return type == PeripheralType::SDIO;
        case ExecActionType::ACTION_CALL_PERIPHERAL:
            // This generic action has a command-dependent capability check in
            // the executor. The shared invariant here is a real, stable target.
            return true;
        default:
            return true;
    }
}

} // namespace PeripheralActionContract
} // namespace FastBee
