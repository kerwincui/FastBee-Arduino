#pragma once
#include <cstring>
#include "core/ChipConfig.h"
#include "core/PeripheralActionContract.h"
#include "core/SensorDriverMetadata.h"
#include "peripherals/drivers/ADS1115Contract.h"

namespace FastBee {
// Describes the implemented path. It does not authorize execution, apply
// settings, probe a device, guarantee a deadline, or schedule a retry.
struct ActionContractMetadata {
    bool covered = false;
    const char* schema = "unknown";
    const char* result = "unknown";
    const char* idempotence = "unknown";
    const char* replay = "unknown";
    const char* timeoutScope = "unknown";
    uint32_t timeoutBudgetMs = 0; // Zero is absent, NOT an unlimited/zero timeout.
    const char* fault = "unknown";
};

inline ActionContractMetadata describeActionContract(uint8_t actionType,
        const PeripheralConfig* target, const char* command = nullptr,
        const PeripheralConfig* provider = nullptr) {
    using A = ExecActionType;
    if (!target || !PeripheralActionContract::requiresPeripheralTarget(actionType) ||
        !PeripheralActionContract::matchesPeripheralTarget(actionType, *target)) return {};
    const auto action = static_cast<A>(actionType);
    if (action == A::ACTION_SET_DAC) {
#if CHIP_HAS_DAC
        return {true, "dac.byte", "sdk_ack_only", "absolute_command_same_config",
            "no_automatic_replay", "final_write_admission_zero_wait_whole_deadline_unknown", 0,
            "retain_failed_lease_output_unknown"};
#else
        return {};
#endif
    }
    if (action == A::ACTION_THERMOSTAT) {
        // The executor only observes the continuous controller. Rule enable/
        // disable and that controller's fault policy are separate operations.
        return {true, "thermostat.v1", "controller_status_only", "observation_only",
            "no_automatic_replay", "unknown", 0, "controller_owned_safe_off_attempt"};
    }
    if (action == A::ACTION_SD_RECORD) {
        return {true, "sd.record.v1", "recorder_status_only", "observation_only",
            "no_automatic_replay", "sd_spi_admission_bounded_sdk_io_deadline_unknown", 0,
            "latch_unknown_write_drop_queue_explicit_recovery"};
    }
    if (action == A::ACTION_SENSOR_READ) {
        if (target->type != PeripheralType::SENSOR_GENERIC ||
            !std::memchr(target->params.sensor.driver, 0, sizeof(target->params.sensor.driver)) ||
            !sensorNameEquals(target->params.sensor.driver, "ADS1115")) return {};
        ADS1115ConversionConfig conversion;
        const auto& params = target->params.sensor;
        if (!makeADS1115ConversionConfig(params.mux, params.fullScaleMv, params.dataRate, conversion) ||
            !ads1115ScanMaskValid(params.scanMask)) return {};
        return {true, "sensor.ads1115", "sample_with_quality", "cache_aware_sample_with_original_age",
            "no_automatic_replay", "conversion_budget_not_whole_action_deadline",
            ads1115ReadBudgetMs(params.dataRate, params.scanMask),
            params.scanMask ? "invalidate_whole_scan_on_any_failure" : "invalidate_failed_sample"};
    }
    if (action == A::ACTION_CALL_PERIPHERAL && target->type == PeripheralType::PCA9685_PWM) {
        // Caller must use parsePca9685Command first. Generic JSON/string
        // commands, useReceivedValue and prepare/start are not classified.
        if (!command) return {};
        if (!std::strcmp(command, "stop"))
            return {true, "pca9685.command", "local_acceptance_only", "state_dependent_stop",
                "release_cleanup_only_after_stop", "unknown", 0, "retain_failed_oe_lease"};
        if (std::strcmp(command, "duty") && std::strcmp(command, "pulse") &&
            std::strcmp(command, "angle") && std::strcmp(command, "enable") &&
            std::strcmp(command, "disable")) return {};
        return {true, "pca9685.command", "local_acceptance_only", "absolute_command_same_session",
            "no_automatic_replay", "pm_and_i2c_admission_zero_wait_deadline_unknown", 0,
            "disable_still_owned_oe_on_fault"};
    }
    const bool fixed = action == A::ACTION_HIGH || action == A::ACTION_LOW ||
        action == A::ACTION_HIGH_INVERTED || action == A::ACTION_LOW_INVERTED;
    const bool toggle = action == A::ACTION_GPIO_TOGGLE;
    if (!(fixed || toggle) || target->type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
        target->params.gpio.passive) return {};
    const char* fault = "no_automatic_safe_state";
    if (target->isExpanderChannel()) {
        if (!provider || provider->type != PeripheralType::IO_EXPANDER ||
            !std::memchr(provider->params.ioExpander.driver, 0, sizeof(provider->params.ioExpander.driver)) ||
            !std::memchr(target->params.gpio.providerId, 0, sizeof(target->params.gpio.providerId)) ||
            provider->id != target->params.gpio.providerId ||
            !sensorNameEquals(provider->params.ioExpander.driver, "MCP23017")) return {};
        fault = "mcp23017_faulted_no_latch_replay";
    }
    return {true, "gpio.level", "local_acceptance_only",
        toggle ? "non_idempotent" : "absolute_command_same_config",
        "no_automatic_replay", "unknown", 0, fault};
}
} // namespace FastBee
