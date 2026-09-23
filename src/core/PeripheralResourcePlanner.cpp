#include <math.h>
#include <string.h>

#include <ArduinoJson.h>

#include "core/PeripheralConfig.h"
#include "core/PeripheralResourcePlanner.h"

namespace FastBee {
namespace {

constexpr const char* ANONYMOUS_OWNER = "<anonymous>";
constexpr uint8_t SENSOR_TYPE_ULTRASONIC = 3;
constexpr uint8_t SENSOR_TYPE_CURRENT = 4;
constexpr uint8_t SENSOR_TYPE_VOLTAGE = 5;

bool isLocalI2cSensor(const PeripheralConfig& config) {
    return config.type == PeripheralType::SENSOR_GENERIC &&
           config.params.sensor.driver[0] != '\0';
}

bool isGenericAnalogSensor(const PeripheralConfig& config) {
    return config.type == PeripheralType::SENSOR_GENERIC &&
           (config.params.sensor.sensorType == SENSOR_TYPE_CURRENT ||
            config.params.sensor.sensorType == SENSOR_TYPE_VOLTAGE);
}

uint32_t i2cFrequency(const PeripheralConfig& config) {
    if (isLocalI2cSensor(config)) {
        return config.params.sensor.frequency > 0
            ? config.params.sensor.frequency : 100000U;
    }
    if (config.type == PeripheralType::LCD1602 ||
        config.type == PeripheralType::LCD) {
        return 100000U;
    }
    return config.params.i2c.frequency > 0
        ? config.params.i2c.frequency : 100000U;
}

uint8_t i2cAddress(const PeripheralConfig& config) {
    if (isLocalI2cSensor(config)) return config.params.sensor.address;
    if (config.type == PeripheralType::LCD1602) {
        return config.params.lcd1602.address;
    }
    return config.type == PeripheralType::I2C ? config.params.i2c.address : 0;
}

bool sameText(const char* left, const char* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    return strcmp(left, right) == 0;
}

bool samePins(const ResourceClaim& left, const ResourceClaim& right) {
    if (left.pinCount != right.pinCount) return false;
    for (uint8_t i = 0; i < left.pinCount && i < 4; ++i) {
        if (left.pins[i] != right.pins[i]) return false;
    }
    return true;
}

const char* ownerFor(const PeripheralConfig& config, const char* overrideId) {
    if (overrideId && overrideId[0] != '\0') return overrideId;
    return config.id.length() > 0 ? config.id.c_str() : ANONYMOUS_OWNER;
}

uint8_t requiredPinCount(const PeripheralConfig& config) {
    switch (config.type) {
        case PeripheralType::UART:
        case PeripheralType::I2C:
        case PeripheralType::ENCODER:
        case PeripheralType::LCD1602:
            return 2;
        case PeripheralType::SPI:
            return 4;
        case PeripheralType::RFID_MFRC522:
            return 5;
        case PeripheralType::SDIO:
            return config.params.sdcard.interface == 0 ? 6 : 4;
        case PeripheralType::DS1302_RTC:
            return 3;
        case PeripheralType::STEPPER_MOTOR:
            return 4;
        case PeripheralType::MODBUS_DEVICE:
        case PeripheralType::DEVICE_EVENT:
        case PeripheralType::UNCONFIGURED:
            return 0;
        case PeripheralType::SENSOR_GENERIC:
            if (isLocalI2cSensor(config) ||
                config.params.sensor.sensorType == SENSOR_TYPE_ULTRASONIC) {
                return 2;
            }
            return config.pinCount > 0 ? 1 : 0;
        case PeripheralType::LCD:
            if (config.params.lcd.interface == 2) return 2;
            if (config.params.lcd.interface == 1) return 4;
            return config.pinCount;
        default:
            return 1;
    }
}

bool isPwmConsumer(const PeripheralConfig& config) {
    return config.type == PeripheralType::GPIO_PWM_OUTPUT ||
           config.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
           config.type == PeripheralType::PWM_SERVO ||
           (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
            config.params.gpio.passive);
}

uint8_t pwmChannel(const PeripheralConfig& config) {
    return config.type == PeripheralType::PWM_SERVO
        ? config.params.servo.pwmChannel
        : config.params.gpio.pwmChannel;
}

uint32_t pwmFrequency(const PeripheralConfig& config) {
    if (config.type == PeripheralType::PWM_SERVO) {
        return config.params.servo.frequency > 0
            ? config.params.servo.frequency
            : SERVO_DEFAULT_FREQUENCY_HZ;
    }
    return config.params.gpio.pwmFrequency > 0
        ? config.params.gpio.pwmFrequency
        : 1000U;
}

uint8_t pwmResolution(const PeripheralConfig& config) {
    if (config.type == PeripheralType::PWM_SERVO) {
        return config.params.servo.pwmResolution > 0
            ? config.params.servo.pwmResolution
            : SERVO_DEFAULT_PWM_RESOLUTION;
    }
    return config.params.gpio.pwmResolution > 0
        ? config.params.gpio.pwmResolution
        : 8U;
}

uint16_t requiredCapabilities(const PeripheralConfig& config, uint8_t index) {
    switch (config.type) {
        case PeripheralType::UART:
            return index == 0 ? PIN_CAP_INPUT : PIN_CAP_OUTPUT;
        case PeripheralType::I2C:
        case PeripheralType::LCD1602:
            return PIN_CAP_INPUT | PIN_CAP_OUTPUT;
        case PeripheralType::SPI:
            return index == 0 ? PIN_CAP_INPUT : PIN_CAP_OUTPUT;
        case PeripheralType::RFID_MFRC522:
            return index == 2 ? PIN_CAP_INPUT : PIN_CAP_OUTPUT;
        case PeripheralType::SDIO:
            if (config.params.sdcard.interface == 1) {
                return index == 2 ? PIN_CAP_INPUT : PIN_CAP_OUTPUT;
            }
            return PIN_CAP_INPUT | PIN_CAP_OUTPUT;
        case PeripheralType::ENCODER:
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE:
        case PeripheralType::RADAR_SENSOR:
        case PeripheralType::IR_RECEIVER:
            return PIN_CAP_INPUT;
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
            return PIN_CAP_INPUT | PIN_CAP_PULL_UP;
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
            return PIN_CAP_INPUT | PIN_CAP_PULL_DOWN;
        case PeripheralType::GPIO_ANALOG_INPUT:
        case PeripheralType::ADC:
            return PIN_CAP_INPUT | PIN_CAP_ADC;
        case PeripheralType::GPIO_TOUCH:
            return PIN_CAP_INPUT | PIN_CAP_TOUCH;
        case PeripheralType::DAC:
            return PIN_CAP_OUTPUT | PIN_CAP_DAC;
        case PeripheralType::DS1302_RTC:
            return index == 1 ? PIN_CAP_INPUT | PIN_CAP_OUTPUT : PIN_CAP_OUTPUT;
        case PeripheralType::SENSOR_GENERIC:
            if (isLocalI2cSensor(config)) {
                return PIN_CAP_INPUT | PIN_CAP_OUTPUT;
            }
            if (config.params.sensor.sensorType == SENSOR_TYPE_ULTRASONIC) {
                return index == 0 ? PIN_CAP_OUTPUT : PIN_CAP_INPUT;
            }
            if (isGenericAnalogSensor(config)) {
                return PIN_CAP_INPUT | PIN_CAP_ADC;
            }
            // DHT/one-wire style sensors drive a request pulse before reading.
            return PIN_CAP_INPUT | PIN_CAP_OUTPUT;
        case PeripheralType::RF_MODULE:
            return config.params.rf.mode == 1 ? PIN_CAP_INPUT : PIN_CAP_OUTPUT;
        case PeripheralType::LCD:
            if (config.params.lcd.interface == 2) return PIN_CAP_INPUT | PIN_CAP_OUTPUT;
            if (config.params.lcd.interface == 1 && index == 0) return PIN_CAP_INPUT;
            return PIN_CAP_OUTPUT;
        default:
            return PIN_CAP_OUTPUT;
    }
}

PinRole roleForPin(const PeripheralConfig& config, uint8_t index) {
    switch (config.type) {
        case PeripheralType::UART:
            return index == 0 ? PinRole::UART_RX : PinRole::UART_TX;
        case PeripheralType::I2C:
        case PeripheralType::LCD1602:
            return index == 0 ? PinRole::I2C_SDA : PinRole::I2C_SCL;
        case PeripheralType::SPI:
            if (index == 0) return PinRole::SPI_MISO;
            if (index == 1) return PinRole::SPI_MOSI;
            if (index == 2) return PinRole::SPI_SCK;
            return PinRole::SPI_CS;
        case PeripheralType::RFID_MFRC522:
            if (index == 0) return PinRole::SPI_SCK;
            if (index == 1) return PinRole::SPI_MOSI;
            if (index == 2) return PinRole::SPI_MISO;
            if (index == 3) return PinRole::SPI_CS;
            return PinRole::OUTPUT_SIGNAL;
        case PeripheralType::SDIO:
            if (config.params.sdcard.interface == 1) {
                if (index == 0) return PinRole::SPI_SCK;
                if (index == 1) return PinRole::SPI_MOSI;
                if (index == 2) return PinRole::SPI_MISO;
                return PinRole::SPI_CS;
            }
            return PinRole::SDMMC_SIGNAL;
        case PeripheralType::ENCODER:
            return index == 0 ? PinRole::ENCODER_A : PinRole::ENCODER_B;
        case PeripheralType::SENSOR_GENERIC:
            if (isLocalI2cSensor(config)) {
                return index == 0 ? PinRole::I2C_SDA : PinRole::I2C_SCL;
            }
            if (config.params.sensor.sensorType == SENSOR_TYPE_ULTRASONIC) {
                return index == 0 ? PinRole::OUTPUT_SIGNAL : PinRole::INPUT_SIGNAL;
            }
            if (isGenericAnalogSensor(config)) return PinRole::ADC_INPUT;
            return PinRole::GENERIC;
        case PeripheralType::GPIO_ANALOG_INPUT:
        case PeripheralType::ADC:
            return PinRole::ADC_INPUT;
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE:
        case PeripheralType::GPIO_TOUCH:
        case PeripheralType::RADAR_SENSOR:
        case PeripheralType::IR_RECEIVER:
            return PinRole::INPUT_SIGNAL;
        case PeripheralType::NEO_PIXEL:
            return PinRole::RMT_SIGNAL;
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            return PinRole::PWM_OUTPUT;
        case PeripheralType::LCD:
            if (config.params.lcd.interface == 2) {
                return index == 0 ? PinRole::I2C_SDA : PinRole::I2C_SCL;
            }
            return PinRole::OUTPUT_SIGNAL;
        default:
            return PinRole::OUTPUT_SIGNAL;
    }
}

bool isI2cPin(const PeripheralConfig& config, uint8_t index) {
    return (config.type == PeripheralType::I2C ||
            config.type == PeripheralType::LCD1602 ||
            isLocalI2cSensor(config) ||
            (config.type == PeripheralType::LCD && config.params.lcd.interface == 2)) &&
           index < 2;
}

bool isSpiBusPin(const PeripheralConfig& config, uint8_t index) {
    if (config.type == PeripheralType::SPI) return index < 3;
    if (config.type == PeripheralType::RFID_MFRC522) return index < 3;
    return config.type == PeripheralType::SDIO &&
           config.params.sdcard.interface == 1 && index < 3;
}

PreflightIssueCode exhaustionCode(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::UART_PORT: return PreflightIssueCode::UART_PORT_EXHAUSTED;
        case ResourceKind::RMT_CHANNEL: return PreflightIssueCode::RMT_CHANNEL_EXHAUSTED;
        case ResourceKind::ENCODER_SLOT: return PreflightIssueCode::ENCODER_SLOT_EXHAUSTED;
        default: return PreflightIssueCode::DORMANT_RESOURCE_CONFLICT;
    }
}

const char* exhaustionMessage(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::UART_PORT: return "No generic UART port is available";
        case ResourceKind::RMT_CHANNEL: return "No RMT channel is available";
        case ResourceKind::ENCODER_SLOT: return "No encoder interrupt slot is available";
        default: return "No resource instance is available";
    }
}

}  // namespace

void PreflightResult::reset() {
    allowed = true;
    requiresConfirmation = false;
    claimsTruncated = false;
    issuesTruncated = false;
    claimCount = 0;
    issueCount = 0;
}

void PreflightResult::resetIssuesPreservingClaims() {
    allowed = !claimsTruncated;
    requiresConfirmation = false;
    issuesTruncated = false;
    issueCount = 0;
}

bool PreflightResult::hasIssue(PreflightIssueCode code) const {
    for (size_t i = 0; i < issueCount; ++i) {
        if (issues[i].code == code) return true;
    }
    return false;
}

PeripheralResourcePlanner::PeripheralResourcePlanner(
        const BoardProfile& profile, const PreflightContext& context,
        PreflightResult& result, const PeripheralConfig* existing,
        size_t existingCount)
    : _profile(profile), _context(context), _result(result),
      _existing(existing), _existingCount(existingCount) {
    _result.reset();
}

void PeripheralResourcePlanner::addIssue(
        PreflightSeverity severity, PreflightIssueCode code,
        ResourceKind kind, uint8_t unit, uint8_t index,
        const char* ownerId, const char* conflictingOwnerId,
        const char* message) {
    for (size_t i = 0; i < _result.issueCount; ++i) {
        const PreflightIssue& issue = _result.issues[i];
        if (issue.code == code && issue.resourceKind == kind &&
            issue.unit == unit && issue.index == index &&
            sameText(issue.ownerId, ownerId) &&
            sameText(issue.conflictingOwnerId, conflictingOwnerId)) {
            return;
        }
    }

    if (_result.issueCount >= PREFLIGHT_MAX_ISSUES) {
        _result.issuesTruncated = true;
        _result.allowed = false;
        return;
    }

    PreflightIssue& issue = _result.issues[_result.issueCount++];
    issue.severity = severity;
    issue.code = code;
    issue.resourceKind = kind;
    issue.unit = unit;
    issue.index = index;
    issue.ownerId = ownerId;
    issue.conflictingOwnerId = conflictingOwnerId;
    issue.message = message;

    if (severity == PreflightSeverity::ERROR ||
        severity == PreflightSeverity::WARNING_REQUIRES_CONFIRMATION) {
        _result.allowed = false;
    }
    if (severity == PreflightSeverity::WARNING_REQUIRES_CONFIRMATION) {
        _result.requiresConfirmation = true;
    }
}

void PeripheralResourcePlanner::validatePin(
        uint8_t gpio, uint16_t required, const char* ownerId) {
    const BoardPinProfile* pin = findBoardPin(_profile, gpio);
    if (!pin) {
        addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_UNKNOWN,
                 ResourceKind::GPIO, 0, gpio, ownerId, nullptr,
                 "GPIO is not present in the selected board profile");
        return;
    }
    if (pin->policy == PinPolicy::HARD_FORBIDDEN) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_HARD_FORBIDDEN,
                 ResourceKind::GPIO, 0, gpio, ownerId, nullptr,
                 "GPIO is hard-forbidden by the selected board profile");
        return;
    }

    if ((required & PIN_CAP_INPUT) && !(pin->capabilities & PIN_CAP_INPUT)) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_INPUT_UNSUPPORTED,
                 ResourceKind::GPIO, 0, gpio, ownerId, nullptr,
                 "GPIO does not support the required input role");
    }
    if ((required & PIN_CAP_OUTPUT) && !(pin->capabilities & PIN_CAP_OUTPUT)) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_OUTPUT_UNSUPPORTED,
                 ResourceKind::GPIO, 0, gpio, ownerId, nullptr,
                 "GPIO does not support the required output role");
    }
    if ((required & (PIN_CAP_PULL_UP | PIN_CAP_PULL_DOWN)) &&
        !boardPinHasCapabilities(*pin,
            required & (PIN_CAP_PULL_UP | PIN_CAP_PULL_DOWN))) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_PULL_UNSUPPORTED,
                 ResourceKind::GPIO, 0, gpio, ownerId, nullptr,
                 "GPIO does not support the requested internal pull resistor");
    }
    if ((required & PIN_CAP_ADC) && !(pin->capabilities & PIN_CAP_ADC)) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_ADC_UNSUPPORTED,
                 ResourceKind::ADC_CHANNEL, pin->adcUnit, pin->adcChannel,
                 ownerId, nullptr, "GPIO is not connected to an ADC channel");
    }

    validatePinRisks(*pin, required, ownerId);
}

void PeripheralResourcePlanner::validatePinRisks(
        const BoardPinProfile& pin, uint16_t required,
        const char* ownerId) {
    if ((pin.risks & PIN_RISK_STRAPPING) &&
        !(_context.acknowledgements & ACK_STRAPPING_PIN)) {
        addIssue(PreflightSeverity::WARNING_REQUIRES_CONFIRMATION,
                 PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED,
                 ResourceKind::GPIO, 0, pin.gpio, ownerId, nullptr,
                 "Boot-strapping GPIO requires explicit confirmation");
    }

    if ((pin.risks & (PIN_RISK_ONBOARD_RGB | PIN_RISK_LP_I2C)) &&
        !(_context.acknowledgements & ACK_BOARD_SHARED_PIN)) {
        addIssue(PreflightSeverity::WARNING_REQUIRES_CONFIRMATION,
                 PreflightIssueCode::PIN_BOARD_RESOURCE_ACK_REQUIRED,
                 ResourceKind::GPIO, 0, pin.gpio, ownerId, nullptr,
                 "GPIO is shared with an onboard or low-power peripheral");
    }

    if ((pin.risks & PIN_RISK_UART0_CONSOLE) &&
        _context.uart0ConsoleActive) {
        if (_profile.id == BoardProfileId::ESP32_C6_DEVKITC_1) {
            addIssue(PreflightSeverity::ERROR,
                     PreflightIssueCode::PIN_UART0_ACTIVE,
                     ResourceKind::GPIO, 0, pin.gpio, ownerId,
                     "runtime:uart0-console",
                     "GPIO is reserved while the UART0 console is active");
        } else if (!(_context.acknowledgements & ACK_UART0_CONSOLE)) {
            addIssue(PreflightSeverity::WARNING_REQUIRES_CONFIRMATION,
                     PreflightIssueCode::PIN_UART0_ACTIVE,
                     ResourceKind::GPIO, 0, pin.gpio, ownerId,
                     "runtime:uart0-console",
                     "GPIO is shared with the active UART0 console");
        }
    }

    if ((pin.risks & PIN_RISK_USB_SERIAL_JTAG) &&
        _context.usbSerialJtagActive) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_USB_SERIAL_JTAG_ACTIVE,
                 ResourceKind::GPIO, 0, pin.gpio, ownerId,
                 "runtime:usb-serial-jtag",
                 "GPIO is reserved while USB Serial/JTAG is active");
    }

    if ((pin.risks & PIN_RISK_JTAG) && _context.jtagActive) {
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::PIN_JTAG_ACTIVE,
                 ResourceKind::GPIO, 0, pin.gpio, ownerId,
                 "runtime:jtag", "GPIO is reserved while JTAG is active");
    }

    if ((pin.risks & PIN_RISK_ADC2_WIFI) &&
        (required & PIN_CAP_ADC) && _context.wifiActive &&
        !(_context.acknowledgements & ACK_ADC2_WITH_WIFI)) {
        addIssue(PreflightSeverity::WARNING_REQUIRES_CONFIRMATION,
                 PreflightIssueCode::ADC2_WIFI_ACK_REQUIRED,
                 ResourceKind::ADC_CHANNEL, pin.adcUnit, pin.adcChannel,
                 ownerId, "runtime:wifi",
                 "ADC2 can be unavailable or unstable while Wi-Fi is active");
    }
}

void PeripheralResourcePlanner::addGpioClaim(
        uint8_t gpio, PinRole role, ClaimState state, const char* ownerId,
        ResourceShareMode shareMode, ResourceKind shareDomain,
        uint8_t shareUnit) {
    ResourceClaim claim;
    claim.kind = ResourceKind::GPIO;
    claim.state = state;
    claim.shareMode = shareMode;
    claim.pinRole = role;
    claim.unit = shareUnit;
    claim.index = gpio;
    claim.pinCount = 1;
    claim.pins[0] = gpio;
    claim.parameterA = static_cast<uint32_t>(shareDomain);
    claim.ownerId = ownerId;
    addClaim(claim);
}

bool PeripheralResourcePlanner::claimIdentityMatches(
        const ResourceClaim& left, const ResourceClaim& right) const {
    if (left.kind != right.kind) return false;
    switch (left.kind) {
        case ResourceKind::GPIO:
        case ResourceKind::I2C_ADDRESS:
        case ResourceKind::SPI_CS:
        case ResourceKind::ADC_CHANNEL:
            return left.unit == right.unit && left.index == right.index;
        default:
            return left.unit == right.unit;
    }
}

bool PeripheralResourcePlanner::claimConflicts(
        const ResourceClaim& existing, const ResourceClaim& candidate,
        PreflightIssueCode& code, bool& compatibleShare) const {
    compatibleShare = false;
    code = PreflightIssueCode::DORMANT_RESOURCE_CONFLICT;
    if (!claimIdentityMatches(existing, candidate)) return false;
    if (sameText(existing.ownerId, candidate.ownerId)) return false;

    switch (candidate.kind) {
        case ResourceKind::GPIO:
            if (existing.shareMode == ResourceShareMode::SHARED_IF_COMPATIBLE &&
                candidate.shareMode == ResourceShareMode::SHARED_IF_COMPATIBLE &&
                existing.parameterA == candidate.parameterA &&
                existing.unit == candidate.unit &&
                existing.pinRole == candidate.pinRole) {
                compatibleShare = true;
                return true;
            }
            code = (existing.pinRole == PinRole::SPI_CS &&
                    candidate.pinRole == PinRole::SPI_CS)
                ? PreflightIssueCode::SPI_CS_CONFLICT
                : PreflightIssueCode::GPIO_CONFLICT;
            return true;
        case ResourceKind::I2C_BUS:
            if (samePins(existing, candidate)) {
                compatibleShare = true;
            } else {
                code = PreflightIssueCode::I2C_BUS_PINS_MISMATCH;
            }
            return true;
        case ResourceKind::SPI_BUS:
            if (samePins(existing, candidate)) {
                compatibleShare = true;
            } else {
                code = PreflightIssueCode::SPI_BUS_PINS_MISMATCH;
            }
            return true;
        case ResourceKind::LEDC_TIMER:
            if (existing.parameterA == candidate.parameterA &&
                existing.parameterB == candidate.parameterB) {
                compatibleShare = true;
            } else {
                code = PreflightIssueCode::LEDC_TIMER_CONFLICT;
            }
            return true;
        case ResourceKind::I2C_ADDRESS:
            code = PreflightIssueCode::I2C_ADDRESS_CONFLICT;
            return true;
        case ResourceKind::SPI_CS:
            code = PreflightIssueCode::SPI_CS_CONFLICT;
            return true;
        case ResourceKind::LEDC_CHANNEL:
            code = PreflightIssueCode::LEDC_CHANNEL_CONFLICT;
            return true;
        case ResourceKind::ADC_CHANNEL:
            code = PreflightIssueCode::ADC_CHANNEL_CONFLICT;
            return true;
        case ResourceKind::SDMMC_HOST:
            code = PreflightIssueCode::SDMMC_HOST_CONFLICT;
            return true;
        case ResourceKind::UART_PORT:
            code = PreflightIssueCode::UART_PORT_EXHAUSTED;
            return true;
        case ResourceKind::RMT_CHANNEL:
            code = PreflightIssueCode::RMT_CHANNEL_EXHAUSTED;
            return true;
        case ResourceKind::ENCODER_SLOT:
            code = PreflightIssueCode::ENCODER_SLOT_EXHAUSTED;
            return true;
        default:
            return true;
    }
}

bool PeripheralResourcePlanner::addClaim(const ResourceClaim& claim) {
    for (size_t i = 0; i < _result.claimCount; ++i) {
        PreflightIssueCode code = PreflightIssueCode::NONE;
        bool compatibleShare = false;
        if (!claimConflicts(_result.claims[i], claim, code, compatibleShare)) {
            continue;
        }

        if (compatibleShare) {
            addIssue(PreflightSeverity::INFO,
                     PreflightIssueCode::RESOURCE_SHARED, claim.kind,
                     claim.unit, claim.index, claim.ownerId,
                     _result.claims[i].ownerId,
                     "Compatible owners share the same resource");
            continue;
        }

        const bool bothActive =
            claim.state == ClaimState::ACTIVE &&
            _result.claims[i].state == ClaimState::ACTIVE;
        if (bothActive) {
            addIssue(PreflightSeverity::ERROR, code, claim.kind,
                     claim.unit, claim.index, claim.ownerId,
                     _result.claims[i].ownerId,
                     "Active resource claim conflicts with an existing owner");
        } else {
            const PreflightIssueCode dormantCode =
                claim.kind == ResourceKind::GPIO
                    ? PreflightIssueCode::DORMANT_GPIO_CONFLICT
                    : PreflightIssueCode::DORMANT_RESOURCE_CONFLICT;
            addIssue(PreflightSeverity::WARNING, dormantCode, claim.kind,
                     claim.unit, claim.index, claim.ownerId,
                     _result.claims[i].ownerId,
                     "Dormant configuration would conflict if both owners were enabled");
        }
    }

    if (_result.claimCount >= PREFLIGHT_MAX_CLAIMS) {
        _result.claimsTruncated = true;
        addIssue(PreflightSeverity::ERROR,
                 PreflightIssueCode::CLAIM_CAPACITY_EXCEEDED,
                 claim.kind, claim.unit, claim.index, claim.ownerId, nullptr,
                 "Preflight claim capacity was exceeded");
        return false;
    }
    _result.claims[_result.claimCount++] = claim;
    return _result.allowed;
}

bool PeripheralResourcePlanner::allocateFirstFree(
        ResourceKind kind, uint8_t firstUnit, uint8_t count,
        ClaimState state, const char* ownerId, PinRole role) {
    for (uint8_t offset = 0; offset < count; ++offset) {
        const uint8_t unit = static_cast<uint8_t>(firstUnit + offset);
        bool used = false;
        for (size_t i = 0; i < _result.claimCount; ++i) {
            const ResourceClaim& existing = _result.claims[i];
            if (existing.kind == kind && existing.unit == unit &&
                existing.state == ClaimState::ACTIVE &&
                !sameText(existing.ownerId, ownerId)) {
                used = true;
                break;
            }
        }
        if (used) continue;

        ResourceClaim claim;
        claim.kind = kind;
        claim.state = state;
        claim.pinRole = role;
        claim.unit = unit;
        claim.ownerId = ownerId;
        return addClaim(claim);
    }

    addIssue(state == ClaimState::ACTIVE
                 ? PreflightSeverity::ERROR
                 : PreflightSeverity::WARNING,
             exhaustionCode(kind), kind, firstUnit, 0, ownerId, nullptr,
             exhaustionMessage(kind));
    return false;
}

bool PeripheralResourcePlanner::addPeripheral(
        const PeripheralConfig& config, ClaimState state,
        const char* overrideId) {
    const char* ownerId = ownerFor(config, overrideId);

    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        struct DisplayPinClaim {
            int8_t gpio;
            PinRole role;
            uint16_t capabilities;
            ResourceShareMode shareMode;
            ResourceKind shareDomain;
        };
        const auto& display = config.params.display;
        const bool rgb = strcasecmp(display.backend, "rgb") == 0;
        const bool hasControl = rgb && strcasecmp(display.controlBackend, "none") != 0;
        if (hasControl && !display.controlMasksDisjoint()) {
            addIssue(PreflightSeverity::ERROR, PreflightIssueCode::DISPLAY_CONTROL_INVALID,
                     ResourceKind::I2C_ADDRESS, display.controlAddress, 0, ownerId, nullptr,
                     "Display reset, enable, backlight, buzzer, inactive and touch INT masks must be disjoint");
        }
        if (!display.controlTouchSyncValid()) {
            addIssue(PreflightSeverity::ERROR, PreflightIssueCode::DISPLAY_CONTROL_INVALID,
                     ResourceKind::I2C_ADDRESS, display.controlAddress, 0, ownerId, nullptr,
                     "Touch INT reset control requires one expander bit, GT911 and external reset");
        }
        const bool hasTouch = strcasecmp(config.params.display.touchBackend, "none") != 0;
        const DisplayPinClaim displayPins[] = {
            {config.params.display.mosi, PinRole::SPI_MOSI, PIN_CAP_OUTPUT,
             rgb ? ResourceShareMode::EXCLUSIVE : ResourceShareMode::SHARED_IF_COMPATIBLE,
             rgb ? ResourceKind::GPIO : ResourceKind::SPI_BUS},
            {config.params.display.sclk, PinRole::SPI_SCK, PIN_CAP_OUTPUT,
             rgb ? ResourceShareMode::EXCLUSIVE : ResourceShareMode::SHARED_IF_COMPATIBLE,
             rgb ? ResourceKind::GPIO : ResourceKind::SPI_BUS},
            {config.params.display.cs, PinRole::SPI_CS, PIN_CAP_OUTPUT,
             ResourceShareMode::EXCLUSIVE, ResourceKind::GPIO},
            {config.params.display.dc, PinRole::OUTPUT_SIGNAL, PIN_CAP_OUTPUT,
             ResourceShareMode::EXCLUSIVE, ResourceKind::GPIO},
            {config.params.display.rst, PinRole::OUTPUT_SIGNAL, PIN_CAP_OUTPUT,
             ResourceShareMode::EXCLUSIVE, ResourceKind::GPIO},
            {config.params.display.backlight, PinRole::OUTPUT_SIGNAL, PIN_CAP_OUTPUT,
             ResourceShareMode::EXCLUSIVE, ResourceKind::GPIO},
            {hasTouch ? config.params.display.touchSda : static_cast<int8_t>(-1),
             PinRole::I2C_SDA, static_cast<uint16_t>(PIN_CAP_INPUT | PIN_CAP_OUTPUT),
             ResourceShareMode::SHARED_IF_COMPATIBLE, ResourceKind::I2C_BUS},
            {hasTouch ? config.params.display.touchScl : static_cast<int8_t>(-1),
             PinRole::I2C_SCL, static_cast<uint16_t>(PIN_CAP_INPUT | PIN_CAP_OUTPUT),
             ResourceShareMode::SHARED_IF_COMPATIBLE, ResourceKind::I2C_BUS},
            {hasTouch ? config.params.display.touchInt : static_cast<int8_t>(-1),
             PinRole::INPUT_SIGNAL, PIN_CAP_INPUT,
             ResourceShareMode::EXCLUSIVE, ResourceKind::GPIO},
            {hasTouch ? config.params.display.touchRst : static_cast<int8_t>(-1),
             PinRole::OUTPUT_SIGNAL, PIN_CAP_OUTPUT,
             ResourceShareMode::EXCLUSIVE, ResourceKind::GPIO}
        };

        const int8_t requiredPins[] = {
            config.params.display.mosi, config.params.display.sclk,
            config.params.display.cs, rgb ? static_cast<int8_t>(0) : config.params.display.dc,
            hasTouch ? config.params.display.touchSda : static_cast<int8_t>(0),
            hasTouch ? config.params.display.touchScl : static_cast<int8_t>(0)
        };
        for (uint8_t i = 0; i < sizeof(requiredPins) / sizeof(requiredPins[0]); ++i) {
            if (requiredPins[i] < 0) {
                addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_REQUIRED,
                         ResourceKind::GPIO, 0, i, ownerId, nullptr,
                         "A required display/touch pin has no GPIO assignment");
            }
        }

        for (uint8_t i = 0; i < sizeof(displayPins) / sizeof(displayPins[0]); ++i) {
            if (displayPins[i].gpio < 0) continue;
            const uint8_t gpio = static_cast<uint8_t>(displayPins[i].gpio);
            for (uint8_t j = 0; j < i; ++j) {
                if (displayPins[j].gpio == displayPins[i].gpio) {
                    addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_DUPLICATED,
                             ResourceKind::GPIO, 0, gpio, ownerId, ownerId,
                             "One display assigns multiple roles to the same GPIO");
                    break;
                }
            }
            validatePin(gpio, displayPins[i].capabilities, ownerId);
            addGpioClaim(gpio, displayPins[i].role, state, ownerId,
                         displayPins[i].shareMode, displayPins[i].shareDomain, 0);
        }

        if (rgb) {
            for (uint8_t i = 10; i < 32; ++i) {
                const int8_t raw = display.pinAt(i);
                if (raw < 0) {
                    if (i < 30) addIssue(PreflightSeverity::ERROR,
                        PreflightIssueCode::PIN_REQUIRED, ResourceKind::GPIO,
                        0, i, ownerId, nullptr, "RGB data and timing pins are required");
                    continue;
                }
                const uint8_t pin = static_cast<uint8_t>(raw);
                for (uint8_t j = 0; j < i; ++j) {
                    if (display.pinAt(j) == raw) addIssue(PreflightSeverity::ERROR,
                        PreflightIssueCode::PIN_DUPLICATED, ResourceKind::GPIO,
                        0, pin, ownerId, ownerId, "RGB pin assignments overlap");
                }
                validatePin(pin, i < 30 ? PIN_CAP_OUTPUT :
                    static_cast<uint16_t>(PIN_CAP_INPUT | PIN_CAP_OUTPUT), ownerId);
                addGpioClaim(pin, i < 30 ? PinRole::OUTPUT_SIGNAL :
                    (i == 30 ? PinRole::I2C_SDA : PinRole::I2C_SCL), state, ownerId,
                    i < 30 ? ResourceShareMode::EXCLUSIVE : ResourceShareMode::SHARED_IF_COMPATIBLE,
                    i < 30 ? ResourceKind::GPIO : ResourceKind::I2C_BUS, 0);
            }
        }

        if (config.params.display.backlight >= 0) {
            ResourceClaim channel;
            channel.kind = ResourceKind::LEDC_CHANNEL;
            channel.state = state;
            channel.unit = config.params.display.backlightChannel;
            channel.pinRole = PinRole::PWM_OUTPUT;
            channel.ownerId = ownerId;
            if (channel.unit >= _profile.resources.ledcChannelCount) {
                addIssue(PreflightSeverity::ERROR,
                         PreflightIssueCode::LEDC_CHANNEL_INVALID,
                         ResourceKind::LEDC_CHANNEL, channel.unit, 0,
                         ownerId, nullptr,
                         "Display backlight LEDC channel is outside the board limit");
            } else {
                addClaim(channel);
                ResourceClaim timer;
                timer.kind = ResourceKind::LEDC_TIMER;
                timer.state = state;
                timer.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
                timer.unit = static_cast<uint8_t>(channel.unit / 2U);
                timer.parameterA = 5000U;
                timer.parameterB = 8U;
                timer.ownerId = ownerId;
                addClaim(timer);
            }
        }

        if (!rgb && _profile.resources.spiBusCount == 0) {
            addIssue(PreflightSeverity::ERROR, PreflightIssueCode::SPI_BUS_UNAVAILABLE,
                     ResourceKind::SPI_BUS, 0, 0, ownerId, nullptr,
                     "The selected board profile has no user SPI bus");
        } else if (!rgb) {
            ResourceClaim bus;
            bus.kind = ResourceKind::SPI_BUS;
            bus.state = state;
            bus.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
            bus.unit = 0;
            bus.pinCount = 3;
            bus.pins[0] = BOARD_PIN_NONE;
            bus.pins[1] = config.params.display.mosi < 0
                ? BOARD_PIN_NONE : static_cast<uint8_t>(config.params.display.mosi);
            bus.pins[2] = config.params.display.sclk < 0
                ? BOARD_PIN_NONE : static_cast<uint8_t>(config.params.display.sclk);
            bus.parameterA = config.params.display.spiFrequency;
            bus.ownerId = ownerId;
            addClaim(bus);

            ResourceClaim cs;
            cs.kind = ResourceKind::SPI_CS;
            cs.state = state;
            cs.unit = 0;
            cs.index = config.params.display.cs < 0
                ? BOARD_PIN_NONE : static_cast<uint8_t>(config.params.display.cs);
            cs.pinRole = PinRole::SPI_CS;
            cs.ownerId = ownerId;
            addClaim(cs);
        }

        if (hasTouch || hasControl) {
            if (_profile.resources.i2cBusCount == 0) {
                addIssue(PreflightSeverity::ERROR, PreflightIssueCode::I2C_BUS_UNAVAILABLE,
                         ResourceKind::I2C_BUS, 0, 0, ownerId, nullptr,
                         "The selected board profile has no user I2C bus");
            } else {
                ResourceClaim bus;
                bus.kind = ResourceKind::I2C_BUS;
                bus.state = state;
                bus.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
                bus.unit = 0;
                bus.pinCount = 2;
                const int8_t sda = hasTouch ? display.touchSda : display.controlSda;
                const int8_t scl = hasTouch ? display.touchScl : display.controlScl;
                bus.pins[0] = sda < 0 ? BOARD_PIN_NONE : static_cast<uint8_t>(sda);
                bus.pins[1] = scl < 0 ? BOARD_PIN_NONE : static_cast<uint8_t>(scl);
                bus.parameterA = config.params.display.i2cFrequency;
                bus.ownerId = ownerId;
                addClaim(bus);

                // GT911 commonly uses 0x5D. The driver can probe 0x14 as a
                // fallback, so conservatively reserve both possible addresses.
                if (hasTouch) for (uint8_t addressValue : {static_cast<uint8_t>(0x5D),
                                             static_cast<uint8_t>(0x14)}) {
                    ResourceClaim address;
                    address.kind = ResourceKind::I2C_ADDRESS;
                    address.state = state;
                    address.unit = 0;
                    address.index = addressValue;
                    address.ownerId = ownerId;
                    addClaim(address);
                }
                if (hasControl) {
                    ResourceClaim address;
                    address.kind = ResourceKind::I2C_ADDRESS;
                    address.state = state;
                    address.unit = 0;
                    address.index = display.controlAddress;
                    address.ownerId = ownerId;
                    addClaim(address);
                }
            }
        }
        return _result.allowed;
    }

    // Reference-mode sensors intentionally persist no local GPIOs. Resolve
    // against the same configuration snapshot used to construct the graph,
    // regardless of peripheral order, just as the runtime bus resolver does.
    const PeripheralConfig* referencedBus = nullptr;
    if (isLocalI2cSensor(config) && config.params.sensor.busPeripheralId[0]) {
        for (size_t i = 0; _existing && i < _existingCount; ++i) {
            if (sameText(_existing[i].id.c_str(), config.params.sensor.busPeripheralId)) {
                referencedBus = &_existing[i];
                break;
            }
        }
        if (!referencedBus || referencedBus->type != PeripheralType::I2C ||
            !referencedBus->enabled || !referencedBus->params.i2c.isMaster ||
            referencedBus->params.i2c.address != 0 || config.pinCount != 0 ||
            referencedBus->pinCount != 2 ||
            (i2cFrequency(*referencedBus) != 100000 && i2cFrequency(*referencedBus) != 400000)) {
            addIssue(PreflightSeverity::ERROR, PreflightIssueCode::I2C_REFERENCE_INVALID,
                     ResourceKind::I2C_BUS, 0, 0, ownerId, config.params.sensor.busPeripheralId,
                     "Sensor requires an enabled master I2C bus reference without a device address or local pins");
            return false;
        }
    }
    const uint8_t* pins = referencedBus ? referencedBus->pins : config.pins;
    const uint8_t required = requiredPinCount(config);
    const uint8_t declared = referencedBus ? 2 : (config.pinCount > 8 ? 8 : config.pinCount);

    for (uint8_t i = 0; i < required; ++i) {
        if (i >= declared || pins[i] == BOARD_PIN_NONE) {
            addIssue(PreflightSeverity::ERROR,
                     PreflightIssueCode::PIN_REQUIRED,
                     ResourceKind::GPIO, 0, i, ownerId, nullptr,
                     "A required pin role has no GPIO assignment");
        }
    }

    for (uint8_t i = 0; i < declared; ++i) {
        const uint8_t gpio = pins[i];
        if (gpio == BOARD_PIN_NONE) continue;
        for (uint8_t j = 0; j < i; ++j) {
            if (pins[j] == gpio) {
                addIssue(PreflightSeverity::ERROR,
                         PreflightIssueCode::PIN_DUPLICATED,
                         ResourceKind::GPIO, 0, gpio, ownerId, ownerId,
                         "One peripheral assigns multiple roles to the same GPIO");
                break;
            }
        }

        validatePin(gpio, requiredCapabilities(config, i), ownerId);

        const PinRole role = roleForPin(config, i);
        if (isI2cPin(config, i)) {
            addGpioClaim(gpio, role, state, ownerId,
                         ResourceShareMode::SHARED_IF_COMPATIBLE,
                         ResourceKind::I2C_BUS, 0);
        } else if (isSpiBusPin(config, i)) {
            addGpioClaim(gpio, role, state, ownerId,
                         ResourceShareMode::SHARED_IF_COMPATIBLE,
                         ResourceKind::SPI_BUS, 0);
        } else {
            addGpioClaim(gpio, role, state, ownerId,
                         ResourceShareMode::EXCLUSIVE);
        }
    }

    if (config.type == PeripheralType::UART) {
        allocateFirstFree(ResourceKind::UART_PORT,
                          _profile.resources.firstGenericUartPort,
                          _profile.resources.genericUartCount,
                          state, ownerId);
    }

    const bool i2cPeripheral = config.type == PeripheralType::I2C ||
        config.type == PeripheralType::LCD1602 ||
        isLocalI2cSensor(config) ||
        (config.type == PeripheralType::LCD && config.params.lcd.interface == 2);
    if (i2cPeripheral) {
        if (_profile.resources.i2cBusCount == 0) {
            addIssue(PreflightSeverity::ERROR,
                     PreflightIssueCode::I2C_BUS_UNAVAILABLE,
                     ResourceKind::I2C_BUS, 0, 0, ownerId, nullptr,
                     "The selected board profile has no user I2C bus");
        } else {
            ResourceClaim bus;
            bus.kind = ResourceKind::I2C_BUS;
            bus.state = state;
            bus.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
            bus.unit = 0;
            bus.pinCount = 2;
            bus.pins[0] = declared > 0 ? pins[0] : BOARD_PIN_NONE;
            bus.pins[1] = declared > 1 ? pins[1] : BOARD_PIN_NONE;
            bus.parameterA = i2cFrequency(referencedBus ? *referencedBus : config);
            bus.ownerId = ownerId;
            addClaim(bus);

            const uint8_t addressValue = i2cAddress(config);
            if (addressValue > 0) {
                ResourceClaim address;
                address.kind = ResourceKind::I2C_ADDRESS;
                address.state = state;
                address.unit = 0;
                address.index = addressValue;
                address.ownerId = ownerId;
                addClaim(address);
            }
        }
    }

    const bool spiPeripheral = config.type == PeripheralType::SPI ||
        config.type == PeripheralType::RFID_MFRC522 ||
        (config.type == PeripheralType::SDIO && config.params.sdcard.interface == 1);
    if (spiPeripheral) {
        if (_profile.resources.spiBusCount == 0) {
            addIssue(PreflightSeverity::ERROR,
                     PreflightIssueCode::SPI_BUS_UNAVAILABLE,
                     ResourceKind::SPI_BUS, 0, 0, ownerId, nullptr,
                     "The selected board profile has no user SPI bus");
        } else {
            ResourceClaim bus;
            bus.kind = ResourceKind::SPI_BUS;
            bus.state = state;
            bus.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
            bus.unit = 0;
            bus.pinCount = 3;
            if (config.type == PeripheralType::SPI) {
                bus.pins[0] = declared > 0 ? config.pins[0] : BOARD_PIN_NONE;
                bus.pins[1] = declared > 1 ? config.pins[1] : BOARD_PIN_NONE;
                bus.pins[2] = declared > 2 ? config.pins[2] : BOARD_PIN_NONE;
            } else {
                // Normalize SD/RFID SCK,MOSI,MISO order to SPI's MISO,MOSI,SCK.
                bus.pins[0] = declared > 2 ? config.pins[2] : BOARD_PIN_NONE;
                bus.pins[1] = declared > 1 ? config.pins[1] : BOARD_PIN_NONE;
                bus.pins[2] = declared > 0 ? config.pins[0] : BOARD_PIN_NONE;
            }
            bus.parameterA = config.type == PeripheralType::RFID_MFRC522
                ? (config.params.rfid.frequency > 0
                    ? config.params.rfid.frequency : RFID_DEFAULT_FREQUENCY)
                : (config.type == PeripheralType::SPI
                    ? config.params.spi.frequency : config.params.sdcard.frequency);
            bus.ownerId = ownerId;
            addClaim(bus);

            const uint8_t csIndex = 3;
            ResourceClaim cs;
            cs.kind = ResourceKind::SPI_CS;
            cs.state = state;
            cs.unit = 0;
            cs.index = declared > csIndex ? config.pins[csIndex] : BOARD_PIN_NONE;
            cs.pinRole = PinRole::SPI_CS;
            cs.ownerId = ownerId;
            addClaim(cs);
        }
    }

    if (config.type == PeripheralType::SDIO &&
        config.params.sdcard.interface == 0) {
        if (!_profile.resources.sdmmcSupported) {
            addIssue(PreflightSeverity::ERROR,
                     PreflightIssueCode::SDMMC_UNSUPPORTED,
                     ResourceKind::SDMMC_HOST, 0, 0, ownerId, nullptr,
                     "The selected board supports SD cards through SDSPI only");
        } else {
            ResourceClaim host;
            host.kind = ResourceKind::SDMMC_HOST;
            host.state = state;
            host.unit = 0;
            host.ownerId = ownerId;
            addClaim(host);
        }
    }

    if (isPwmConsumer(config)) {
        const uint8_t channel = pwmChannel(config);
        if (channel >= _profile.resources.ledcChannelCount) {
            addIssue(PreflightSeverity::ERROR,
                     PreflightIssueCode::LEDC_CHANNEL_INVALID,
                     ResourceKind::LEDC_CHANNEL, channel, 0,
                     ownerId, nullptr,
                     "LEDC channel is outside the board resource limit");
        } else {
            ResourceClaim channelClaim;
            channelClaim.kind = ResourceKind::LEDC_CHANNEL;
            channelClaim.state = state;
            channelClaim.unit = channel;
            channelClaim.ownerId = ownerId;
            addClaim(channelClaim);

            ResourceClaim timer;
            timer.kind = ResourceKind::LEDC_TIMER;
            timer.state = state;
            timer.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
            timer.unit = static_cast<uint8_t>(channel / 2U);
            timer.parameterA = pwmFrequency(config);
            timer.parameterB = pwmResolution(config);
            timer.ownerId = ownerId;
            if (timer.unit >= _profile.resources.ledcTimerCount) {
                addIssue(PreflightSeverity::ERROR,
                         PreflightIssueCode::LEDC_TIMER_CONFLICT,
                         ResourceKind::LEDC_TIMER, timer.unit, 0,
                         ownerId, nullptr,
                         "LEDC channel maps outside the available timer range");
            } else {
                addClaim(timer);
            }
        }
    }

    if (config.type == PeripheralType::NEO_PIXEL) {
        allocateFirstFree(ResourceKind::RMT_CHANNEL, 0,
                          _profile.resources.rmtChannelCount,
                          state, ownerId, PinRole::RMT_SIGNAL);
    }

    if (config.type == PeripheralType::ENCODER &&
        config.params.encoder.useInterrupt) {
        allocateFirstFree(ResourceKind::ENCODER_SLOT, 0,
                          _profile.resources.encoderSlotCount,
                          state, ownerId);
    }

    if ((config.type == PeripheralType::GPIO_ANALOG_INPUT ||
         config.type == PeripheralType::ADC ||
         isGenericAnalogSensor(config)) && declared > 0 &&
        config.pins[0] != BOARD_PIN_NONE) {
        const BoardPinProfile* pin = findBoardPin(_profile, config.pins[0]);
        if (pin && pin->policy != PinPolicy::HARD_FORBIDDEN &&
            (pin->capabilities & PIN_CAP_ADC)) {
            ResourceClaim adc;
            adc.kind = ResourceKind::ADC_CHANNEL;
            adc.state = state;
            adc.shareMode = ResourceShareMode::EXCLUSIVE;
            adc.unit = pin->adcUnit;
            adc.index = pin->adcChannel;
            adc.pinRole = PinRole::ADC_INPUT;
            adc.pinCount = 1;
            adc.pins[0] = pin->gpio;
            adc.ownerId = ownerId;
            addClaim(adc);
        }
    }

    return _result.allowed;
}

ClaimState candidateClaimState(const PeripheralConfig& candidate,
                               PreflightOperation operation) {
    if (operation == PreflightOperation::ENABLE) return ClaimState::ACTIVE;
    if (operation == PreflightOperation::DISABLE) return ClaimState::DORMANT;
    return candidate.enabled ? ClaimState::ACTIVE : ClaimState::DORMANT;
}

void preflightPeripheralChange(
        const BoardProfile& profile, const PeripheralConfig* existing,
        size_t existingCount, const PeripheralConfig& candidate,
        PreflightOperation operation, const char* excludeId,
        const PreflightContext& context, PreflightResult& result) {
    PeripheralResourcePlanner planner(profile, context, result, existing, existingCount);

    const char* effectiveExclude = excludeId;
    if ((!effectiveExclude || effectiveExclude[0] == '\0') &&
        operation != PreflightOperation::ADD &&
        operation != PreflightOperation::IMPORT &&
        operation != PreflightOperation::TEMPLATE_APPLY &&
        candidate.id.length() > 0) {
        effectiveExclude = candidate.id.c_str();
    }

    if (existing) {
        for (size_t i = 0; i < existingCount; ++i) {
            if (effectiveExclude && effectiveExclude[0] != '\0' &&
                sameText(existing[i].id.c_str(), effectiveExclude)) {
                continue;
            }
            planner.addPeripheral(existing[i],
                existing[i].enabled ? ClaimState::ACTIVE : ClaimState::DORMANT);
        }
    }

    // Existing peripherals establish the resource graph. Their own policy and
    // consistency diagnostics belong to the resources endpoint, not to the
    // candidate mutation that is evaluated below.
    result.resetIssuesPreservingClaims();

    if (operation != PreflightOperation::REMOVE) {
        planner.addPeripheral(candidate,
                              candidateClaimState(candidate, operation));
    }
}

const char* preflightIssueCodeName(PreflightIssueCode code) {
    switch (code) {
        case PreflightIssueCode::NONE: return "NONE";
        case PreflightIssueCode::PIN_REQUIRED: return "PIN_REQUIRED";
        case PreflightIssueCode::PIN_DUPLICATED: return "PIN_DUPLICATED";
        case PreflightIssueCode::PIN_UNKNOWN: return "PIN_UNKNOWN";
        case PreflightIssueCode::PIN_HARD_FORBIDDEN: return "PIN_HARD_FORBIDDEN";
        case PreflightIssueCode::PIN_INPUT_UNSUPPORTED: return "PIN_INPUT_UNSUPPORTED";
        case PreflightIssueCode::PIN_OUTPUT_UNSUPPORTED: return "PIN_OUTPUT_UNSUPPORTED";
        case PreflightIssueCode::PIN_PULL_UNSUPPORTED: return "PIN_PULL_UNSUPPORTED";
        case PreflightIssueCode::PIN_ADC_UNSUPPORTED: return "PIN_ADC_UNSUPPORTED";
        case PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED: return "PIN_STRAPPING_ACK_REQUIRED";
        case PreflightIssueCode::PIN_BOARD_RESOURCE_ACK_REQUIRED: return "PIN_BOARD_RESOURCE_ACK_REQUIRED";
        case PreflightIssueCode::PIN_UART0_ACTIVE: return "PIN_UART0_ACTIVE";
        case PreflightIssueCode::PIN_USB_SERIAL_JTAG_ACTIVE: return "PIN_USB_SERIAL_JTAG_ACTIVE";
        case PreflightIssueCode::PIN_JTAG_ACTIVE: return "PIN_JTAG_ACTIVE";
        case PreflightIssueCode::ADC2_WIFI_ACK_REQUIRED: return "ADC2_WIFI_ACK_REQUIRED";
        case PreflightIssueCode::GPIO_CONFLICT: return "GPIO_CONFLICT";
        case PreflightIssueCode::DORMANT_GPIO_CONFLICT: return "DORMANT_GPIO_CONFLICT";
        case PreflightIssueCode::I2C_BUS_UNAVAILABLE: return "I2C_BUS_UNAVAILABLE";
        case PreflightIssueCode::I2C_BUS_PINS_MISMATCH: return "I2C_BUS_PINS_MISMATCH";
        case PreflightIssueCode::I2C_REFERENCE_INVALID: return "I2C_REFERENCE_INVALID";
        case PreflightIssueCode::I2C_ADDRESS_CONFLICT: return "I2C_ADDRESS_CONFLICT";
        case PreflightIssueCode::SPI_BUS_UNAVAILABLE: return "SPI_BUS_UNAVAILABLE";
        case PreflightIssueCode::SPI_BUS_PINS_MISMATCH: return "SPI_BUS_PINS_MISMATCH";
        case PreflightIssueCode::SPI_CS_CONFLICT: return "SPI_CS_CONFLICT";
        case PreflightIssueCode::UART_PORT_EXHAUSTED: return "UART_PORT_EXHAUSTED";
        case PreflightIssueCode::LEDC_CHANNEL_INVALID: return "LEDC_CHANNEL_INVALID";
        case PreflightIssueCode::LEDC_CHANNEL_CONFLICT: return "LEDC_CHANNEL_CONFLICT";
        case PreflightIssueCode::LEDC_TIMER_CONFLICT: return "LEDC_TIMER_CONFLICT";
        case PreflightIssueCode::RMT_CHANNEL_EXHAUSTED: return "RMT_CHANNEL_EXHAUSTED";
        case PreflightIssueCode::ADC_CHANNEL_CONFLICT: return "ADC_CHANNEL_CONFLICT";
        case PreflightIssueCode::ENCODER_SLOT_EXHAUSTED: return "ENCODER_SLOT_EXHAUSTED";
        case PreflightIssueCode::SDMMC_UNSUPPORTED: return "SDMMC_UNSUPPORTED";
        case PreflightIssueCode::SDMMC_HOST_CONFLICT: return "SDMMC_HOST_CONFLICT";
        case PreflightIssueCode::DORMANT_RESOURCE_CONFLICT: return "DORMANT_RESOURCE_CONFLICT";
        case PreflightIssueCode::CLAIM_CAPACITY_EXCEEDED: return "CLAIM_CAPACITY_EXCEEDED";
        case PreflightIssueCode::ISSUE_CAPACITY_EXCEEDED: return "ISSUE_CAPACITY_EXCEEDED";
        case PreflightIssueCode::RESOURCE_SHARED: return "RESOURCE_SHARED";
        case PreflightIssueCode::DISPLAY_CONTROL_INVALID: return "DISPLAY_CONTROL_INVALID";
        default: return "UNKNOWN_ISSUE";
    }
}

const char* preflightSeverityName(PreflightSeverity severity) {
    switch (severity) {
        case PreflightSeverity::INFO: return "info";
        case PreflightSeverity::WARNING: return "warning";
        case PreflightSeverity::WARNING_REQUIRES_CONFIRMATION:
            return "warning-requires-confirmation";
        case PreflightSeverity::ERROR: return "error";
        default: return "unknown";
    }
}

const char* resourceKindName(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::GPIO: return "gpio";
        case ResourceKind::I2C_BUS: return "i2c_bus";
        case ResourceKind::I2C_ADDRESS: return "i2c_address";
        case ResourceKind::SPI_BUS: return "spi_bus";
        case ResourceKind::SPI_CS: return "spi_cs";
        case ResourceKind::UART_PORT: return "uart_port";
        case ResourceKind::LEDC_CHANNEL: return "ledc_channel";
        case ResourceKind::LEDC_TIMER: return "ledc_timer";
        case ResourceKind::RMT_CHANNEL: return "rmt_channel";
        case ResourceKind::ADC_CHANNEL: return "adc_channel";
        case ResourceKind::ENCODER_SLOT: return "encoder_slot";
        case ResourceKind::SDMMC_HOST: return "sdmmc_host";
        default: return "unknown";
    }
}

}  // namespace FastBee
