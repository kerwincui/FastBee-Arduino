#include "./network/handlers/PeripheralRouteHandler.h"
#include "network/LiteralStringMatch.h"
#include "core/MemoryBudget.h"
#include "PeripheralRouteReadSupport.h"
#include "./network/handlers/HandlerUtils.h"
#include "network/JsonFragmentsResponse.h"
#include "network/FixedJsonField.h"
#include "network/NetworkManager.h"
#include "network/NetworkResourceConfig.h"
#include "./network/WebHandlerContext.h"
#include "core/FeatureFlags.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/PeripheralManager.h"
#include "core/RtcBootSeedConfig.h"
#include "core/ResourceProfile.h"
#include "core/SensorDriverCache.h"
#include "core/SensorConfigSupport.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterContract.h"
#include "core/ActionValueContract.h"
#include "core/DacWriteAccess.h"
#include "network/HttpOutputControl.h"
#include <cmath>
#include <cstdlib>
#include "core/DriverRegistry.h"
#include "core/RuntimeFeatureManager.h"
#include "core/I2CBusManager.h"
#include "peripherals/SDCardManager.h"
#include "network/SDCardStatusJson.h"
#include "network/RuntimeResourceJson.h"
#include "network/RuntimePinResourceJson.h"
#include "core/SpiBusInitializer.h"
#include "core/SdAccess.h"
#if FASTBEE_ENABLE_SCREEN_UI
#include "screen/ScreenRuntime.h"
#endif
#if FASTBEE_ENABLE_SENSOR_DRIVER
#include "peripherals/SensorDriver.h"
#endif
#if FASTBEE_ENABLE_DS1302
#include "peripherals/drivers/DS1302Driver.h"
#include "core/RtcAccess.h"
#endif
#if FASTBEE_ENABLE_LCD1602
#include "peripherals/drivers/LCD1602Driver.h"
#endif
#if FASTBEE_ENABLE_RFID
#include "peripherals/drivers/RFIDDriver.h"
#endif
#include <ArduinoJson.h>
#include <Wire.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <math.h>
#include <new>
#include <vector>
#include <esp_system.h>
#include "systems/LoggerSystem.h"

namespace {
using namespace FastBee::PeripheralRouteReadDetail;
bool allocatePreflightWorkspace(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                                std::unique_ptr<FastBee::PreflightResult>& workspace) {
    // The caller already owns both planner and configuration locks. Keep this
    // large scratch result off callback stacks and out of idle resident RAM.
    workspace.reset(new (std::nothrow) FastBee::PreflightResult);
    if (workspace) return true;
    ctx->sendError(request, 503, "PREFLIGHT_MEMORY_UNAVAILABLE: resource planner workspace unavailable");
    return false;
}

constexpr uint32_t SENSOR_DEFAULT_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t SENSOR_MIN_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t SENSOR_MAX_SAMPLE_INTERVAL_MS = 86400000UL;
constexpr size_t LCD1602_API_MAX_BYTES = 128;

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
    static constexpr uint8_t DAYS[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) return 0;
    return month == 2 && isLeapYear(year) ? 29 : DAYS[month - 1];
}

uint8_t calculateDayOfWeek(int year, int month, int day) {
    // Sakamoto returns Sunday=0. DS1302 uses Monday=1 ... Sunday=7.
    static constexpr uint8_t OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int adjustedYear = year;
    if (month < 3) --adjustedYear;
    const int sundayBased = (adjustedYear + adjustedYear / 4 - adjustedYear / 100 +
                             adjustedYear / 400 + OFFSETS[month - 1] + day) % 7;
    return sundayBased == 0 ? 7 : static_cast<uint8_t>(sundayBased);
}

String bytesToHex(const uint8_t* data, size_t length) {
    String output;
    output.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        char byteText[4];
        snprintf(byteText, sizeof(byteText), i + 1 < length ? "%02X " : "%02X", data[i]);
        output += byteText;
    }
    return output;
}

uint32_t normalizeSensorSampleInterval(int value) {
    if (value <= 0) return SENSOR_DEFAULT_SAMPLE_INTERVAL_MS;
    if (static_cast<uint32_t>(value) < SENSOR_MIN_SAMPLE_INTERVAL_MS) return SENSOR_DEFAULT_SAMPLE_INTERVAL_MS;
    if (static_cast<uint32_t>(value) > SENSOR_MAX_SAMPLE_INTERVAL_MS) return SENSOR_MAX_SAMPLE_INTERVAL_MS;
    return static_cast<uint32_t>(value);
}

bool isAnalogInputPeripheral(PeripheralType type) {
    return type == PeripheralType::GPIO_ANALOG_INPUT || type == PeripheralType::ADC;
}

bool isDigitalInputPeripheral(PeripheralType type) {
    return type == PeripheralType::GPIO_DIGITAL_INPUT ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
           type == PeripheralType::GPIO_INTERRUPT_RISING ||
           type == PeripheralType::GPIO_INTERRUPT_FALLING ||
           type == PeripheralType::GPIO_INTERRUPT_CHANGE;
}

const char* userConfigurablePeripheralTypeError(PeripheralType type) {
    if (type == PeripheralType::MODBUS_DEVICE) {
        return "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus";
    }
    if (!isPeripheralTypeImplemented(type)) {
        return "UNSUPPORTED_PERIPHERAL_TYPE: current firmware does not implement this type";
    }
    return nullptr;
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addDigitalInputParams(JsonObject params, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(params, "inputMode", config.params.gpio.inputMode);
    FastBee::setFixedJsonField(params, "invertLogic", config.params.gpio.invertLogic);
    FastBee::setFixedJsonField(params, "debounceMs", config.params.gpio.debounceMs ? config.params.gpio.debounceMs : 50);
    FastBee::setFixedJsonField(params, "reportMode", config.params.gpio.reportMode);
    FastBee::setFixedJsonField(params, "pairedInputId", config.params.gpio.pairedInputId);
    FastBee::setFixedJsonField(params, "pairDebounceMs", config.params.gpio.pairDebounceMs ? config.params.gpio.pairDebounceMs : 200);
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addAnalogInputParams(JsonObject params, const PeripheralConfig& config, bool includeAdvanced = true) {
    FastBee::setFixedJsonField(params, "attenuation", config.params.adc.attenuation);
    FastBee::setFixedJsonField(params, "resolution", config.params.adc.resolution);
    if (includeAdvanced) FastBee::setFixedJsonField(params, "sampleRate", config.params.adc.sampleRate);
    FastBee::setFixedJsonField(params, "sampleCount", config.params.adc.sampleCount ? config.params.adc.sampleCount : 1);
    FastBee::setFixedJsonField(params, "filterMode", config.params.adc.filterMode);
    if (includeAdvanced) {
        FastBee::setFixedJsonField(params, "signalMode", config.params.adc.signalMode);
        FastBee::setFixedJsonField(params, "conversionEnabled", config.params.adc.conversionEnabled);
        FastBee::setFixedJsonField(params, "clampEnabled", config.params.adc.clampEnabled);
        FastBee::setFixedJsonField(params, "calibratedVoltage", config.params.adc.calibratedVoltage);
        FastBee::setFixedJsonField(params, "decimalPlaces", config.params.adc.decimalPlaces);
        FastBee::setFixedJsonField(params, "inputMin", config.params.adc.inputMin);
        FastBee::setFixedJsonField(params, "inputMax", config.params.adc.inputMax);
        FastBee::setFixedJsonField(params, "engineeringMin", config.params.adc.engineeringMin);
        FastBee::setFixedJsonField(params, "engineeringMax", config.params.adc.engineeringMax);
        FastBee::setFixedJsonField(params, "engineeringOffset", config.params.adc.engineeringOffset);
        FastBee::setFixedJsonField(params, "vRef", config.params.adc.vRef);
        FastBee::setFixedJsonField(params, "voltageRatio", config.params.adc.voltageRatio);
        FastBee::setFixedJsonField(params, "shuntResistance", config.params.adc.shuntResistance);
        FastBee::setFixedJsonField(params, "inputGain", config.params.adc.inputGain);
        FastBee::setFixedJsonField(params, "inputOffset", config.params.adc.inputOffset);
        FastBee::setFixedJsonField(params, "rawMin", config.params.adc.rawMin);
        FastBee::setFixedJsonField(params, "rawMax", config.params.adc.rawMax);
    }
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addServoParams(JsonObject params, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(params, "frequency", config.params.servo.frequency);
    FastBee::setFixedJsonField(params, "minPulse", config.params.servo.minPulse);
    FastBee::setFixedJsonField(params, "maxPulse", config.params.servo.maxPulse);
    FastBee::setFixedJsonField(params, "minAngle", config.params.servo.minAngle);
    FastBee::setFixedJsonField(params, "maxAngle", config.params.servo.maxAngle);
    FastBee::setFixedJsonField(params, "initialAngle", config.params.servo.initialAngle);
    FastBee::setFixedJsonField(params, "inverted", config.params.servo.inverted);
    FastBee::setFixedJsonField(params, "releaseAfterMove", config.params.servo.releaseAfterMove);
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addUartParams(JsonObject params, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(params, "baudRate", config.params.uart.baudRate);
    FastBee::setFixedJsonField(params, "dataBits", config.params.uart.dataBits);
    FastBee::setFixedJsonField(params, "stopBits", config.params.uart.stopBits);
    FastBee::setFixedJsonField(params, "parity", config.params.uart.parity);
    FastBee::setFixedJsonField(params, "framing", uartFramingName(config));
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addEncoderParams(JsonObject params, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(params, "resolution", config.params.encoder.resolution);
    FastBee::setFixedJsonField(params, "useInterrupt", config.params.encoder.useInterrupt);
}

void addDisplayParams(JsonObject params, const PeripheralConfig& config);

// These fields are required by both the editor and capability-based action
// selectors, including compact lists on devices without PSRAM.
#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addGenericPeripheralFields(JsonObject object, const PeripheralConfig& config) {
    const bool logicalIo = config.type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
        isDigitalInputPeripheral(config.type);
    if (!logicalIo && config.type != PeripheralType::IO_EXPANDER &&
        config.type != PeripheralType::PCA9685_PWM &&
        config.type != PeripheralType::SENSOR_GENERIC &&
        config.type != PeripheralType::SCREEN_DISPLAY) return;
    JsonObject params = object["params"].is<JsonObject>()
        ? object["params"].as<JsonObject>() : object["params"].to<JsonObject>();
    if (logicalIo) {
        FastBee::setFixedJsonField(params, "ioSource", config.params.gpio.ioSource);
        FastBee::setFixedJsonField(params, "providerId", JsonString(config.params.gpio.providerId, false));
        FastBee::setFixedJsonField(params, "channel", config.params.gpio.channel);
        FastBee::setFixedJsonField(params, "initialState", static_cast<int>(config.params.gpio.initialState));
        FastBee::setFixedJsonField(params, "invertLogic", config.params.gpio.invertLogic);
        FastBee::setFixedJsonField(params, "nativeControlEligible", config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
            !config.isExpanderChannel() && config.pinCount == 1 && !config.params.gpio.passive &&
            !config.params.gpio.invertLogic && config.params.gpio.initialState == GPIOState::STATE_HIGH);
    } else if (config.type == PeripheralType::PCA9685_PWM) {
        FastBee::serializePca9685Config(params, config);
    } else if (config.type == PeripheralType::IO_EXPANDER) {
        FastBee::setFixedJsonField(params, "driver", JsonString(config.params.ioExpander.driver, false));
        FastBee::setFixedJsonField(params, "busPeripheralId", JsonString(config.params.ioExpander.busPeripheralId, false));
        FastBee::setFixedJsonField(params, "resetPeripheralId", JsonString(config.params.ioExpander.resetPeripheralId, false));
        FastBee::setFixedJsonField(params, "address", config.params.ioExpander.address);
        FastBee::setFixedJsonField(params, "sampleInterval", config.params.ioExpander.sampleInterval);
    } else if (config.type == PeripheralType::SENSOR_GENERIC) {
        FastBee::serializeSensorConfig(params, config);
    } else {
        addDisplayParams(params, config);
    }
    const char* feature = config.type == PeripheralType::SCREEN_DISPLAY
        ? "screenPanel"
        : config.type == PeripheralType::IO_EXPANDER || config.type == PeripheralType::PCA9685_PWM || config.isExpanderChannel()
        ? "ioExpander" : config.type == PeripheralType::SENSOR_GENERIC && config.params.sensor.driver[0]
            ? "i2cSensors" : nullptr;
    if (feature) {
        FastBee::setFixedJsonField(object, "feature", feature);
        FastBee::setFixedJsonField(object, "featureActive", FastBee::RuntimeFeatureManager::instance().shouldEnable(feature));
    }
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addP1PeripheralParams(JsonObject params, const PeripheralConfig& config) {
    if (config.type == PeripheralType::SDIO) {
        FastBee::setFixedJsonField(params, "interface", config.params.sdcard.interface);
        FastBee::setFixedJsonField(params, "frequency", config.params.sdcard.frequency);
    } else if (config.type == PeripheralType::DS1302_RTC) {
        FastBee::setFixedJsonField(params, "use24Hour", config.params.ds1302.use24Hour);
        FastBee::setFixedJsonField(params, "trickleCharge", config.params.ds1302.trickleCharge);
        FastBee::serializeRtcBootSeedConfig(params, config.params.ds1302.bootSeed,
                                           config.params.ds1302.utcOffsetMinutes);
    } else if (config.type == PeripheralType::LCD1602) {
        FastBee::setFixedJsonField(params, "cols", config.params.lcd1602.cols);
        FastBee::setFixedJsonField(params, "rows", config.params.lcd1602.rows);
        FastBee::setFixedJsonField(params, "address", config.params.lcd1602.address);
        FastBee::setFixedJsonField(params, "backlight", config.params.lcd1602.backlight);
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        FastBee::setFixedJsonField(params, "frequency", config.params.rfid.frequency);
    } else if (config.type == PeripheralType::SENSOR_GENERIC) {
        FastBee::serializeSensorConfig(params, config);
    }
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addDisplayParams(JsonObject params, const PeripheralConfig& config) {
    const DisplayConfigParams& display = config.params.display;
    FastBee::setFixedJsonField(params, "backend", JsonString(display.backend, false));
    FastBee::setFixedJsonField(params, "driver", JsonString(display.driver, false));
    FastBee::setFixedJsonField(params, "touchBackend", JsonString(display.touchBackend, false));
    FastBee::setFixedJsonField(params, "width", display.width);
    FastBee::setFixedJsonField(params, "height", display.height);
    FastBee::setFixedJsonField(params, "rotation", display.rotation);
    FastBee::setFixedJsonField(params, "colorDepth", display.colorDepth);
    FastBee::setFixedJsonField(params, "bufferLines", display.bufferLines);
    FastBee::setFixedJsonField(params, "brightness", display.brightness);
    FastBee::setFixedJsonField(params, "backlightChannel", display.backlightChannel);
    FastBee::setFixedJsonField(params, "backlightActiveHigh", display.backlightActiveHigh);
    FastBee::setFixedJsonField(params, "spiFrequency", display.spiFrequency);
    FastBee::setFixedJsonField(params, "i2cFrequency", display.i2cFrequency);
    FastBee::setFixedJsonField(params, "rgbInitProfile", JsonString(display.rgbInitProfile, false));
    FastBee::setFixedJsonField(params, "rgbFrequency", display.rgbFrequency);
    FastBee::setFixedJsonField(params, "hsyncFrontPorch", display.hsyncFrontPorch);
    FastBee::setFixedJsonField(params, "hsyncPulseWidth", display.hsyncPulseWidth);
    FastBee::setFixedJsonField(params, "hsyncBackPorch", display.hsyncBackPorch);
    FastBee::setFixedJsonField(params, "vsyncFrontPorch", display.vsyncFrontPorch);
    FastBee::setFixedJsonField(params, "vsyncPulseWidth", display.vsyncPulseWidth);
    FastBee::setFixedJsonField(params, "vsyncBackPorch", display.vsyncBackPorch);
    FastBee::setFixedJsonField(params, "hsyncIdleLow", display.hsyncIdleLow);
    FastBee::setFixedJsonField(params, "vsyncIdleLow", display.vsyncIdleLow);
    FastBee::setFixedJsonField(params, "pclkActiveNeg", display.pclkActiveNeg);
    FastBee::setFixedJsonField(params, "controlBackend", JsonString(display.controlBackend, false));
    FastBee::setFixedJsonField(params, "controlAddress", display.controlAddress);
    FastBee::setFixedJsonField(params, "controlResetMask", display.controlResetMask);
    FastBee::setFixedJsonField(params, "controlBacklightMask", display.controlBacklightMask);
    FastBee::setFixedJsonField(params, "controlEnableMask", display.controlEnableMask);
    FastBee::setFixedJsonField(params, "controlBuzzerMask", display.controlBuzzerMask);
    FastBee::setFixedJsonField(params, "touchBuzzerDurationMs", display.touchBuzzerDurationMs);
    FastBee::setFixedJsonField(params, "controlInactiveMask", display.controlInactiveMask);
    FastBee::setFixedJsonField(params, "controlTouchIntMask", display.controlTouchIntMask);
    JsonObject pins = params["pins"].to<JsonObject>();
    FastBee::setFixedJsonField(pins, "mosi", display.mosi);
    FastBee::setFixedJsonField(pins, "sclk", display.sclk);
    FastBee::setFixedJsonField(pins, "cs", display.cs);
    FastBee::setFixedJsonField(pins, "dc", display.dc);
    FastBee::setFixedJsonField(pins, "rst", display.rst);
    FastBee::setFixedJsonField(pins, "backlight", display.backlight);
    FastBee::setFixedJsonField(pins, "touchSda", display.touchSda);
    FastBee::setFixedJsonField(pins, "touchScl", display.touchScl);
    FastBee::setFixedJsonField(pins, "touchInt", display.touchInt);
    FastBee::setFixedJsonField(pins, "touchRst", display.touchRst);
    JsonArray rgbPins = pins["rgbDataPins"].to<JsonArray>();
    for (uint8_t i = 0; i < 16; ++i) rgbPins.add(display.rgbDataPins[i]);
    FastBee::setFixedJsonField(pins, "rgbDe", display.rgbDe);
    FastBee::setFixedJsonField(pins, "rgbVsync", display.rgbVsync);
    FastBee::setFixedJsonField(pins, "rgbHsync", display.rgbHsync);
    FastBee::setFixedJsonField(pins, "rgbPclk", display.rgbPclk);
    FastBee::setFixedJsonField(pins, "controlSda", display.controlSda);
    FastBee::setFixedJsonField(pins, "controlScl", display.controlScl);
}

// Match PeripheralManager's protocol-owned UART reservation. This is a
// configuration capability, independent of whether the catalog has been fetched.
#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addPeripheralDiagnosticCapability(JsonObject target, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(target, "implemented", isPeripheralTypeImplemented(config.type));
    if (!config.enabled) FastBee::setFixedJsonField(target, "diagnosticBlockedReason", "disabled");
    if (config.type == PeripheralType::UART) {
        String id = config.id;
        id.toLowerCase();
        if (id.startsWith("modbus") || id.startsWith("rs485")) {
            FastBee::setFixedJsonField(target, "diagnosticBlockedReason", "protocol.modbus");
        }
    }
}

int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parseStrictHex(const String& input, uint8_t* output, size_t capacity,
                    size_t& length, String& errorMsg) {
    length = 0;
    int highNibble = -1;
    for (size_t i = 0; i < input.length(); ++i) {
        const char value = input[i];
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') continue;

        const int nibble = hexNibble(value);
        if (nibble < 0) {
            errorMsg = String("INVALID_HEX: unexpected character at offset ") + String(i);
            return false;
        }
        if (highNibble < 0) {
            highNibble = nibble;
            continue;
        }
        if (length >= capacity) {
            errorMsg = String("PAYLOAD_TOO_LARGE: maximum ") + String(capacity) + " bytes";
            return false;
        }
        output[length++] = static_cast<uint8_t>((highNibble << 4) | nibble);
        highNibble = -1;
    }

    if (highNibble >= 0) {
        errorMsg = "INVALID_HEX: hexadecimal payload must contain complete byte pairs";
        return false;
    }
    if (length == 0) {
        errorMsg = "INVALID_HEX: payload is empty";
        return false;
    }
    return true;
}

int mutationErrorStatus(const String& errorMsg) {
    if (errorMsg.startsWith("PERSISTENCE_") ||
        errorMsg.startsWith("ROLLBACK_FAILED")) return 500;
    if (errorMsg.startsWith("UART_RESOURCE_EXHAUSTED") ||
        errorMsg.startsWith("ENCODER_RESOURCE_EXHAUSTED")) return 409;
    if (errorMsg.startsWith("EXTERNALLY_MANAGED_PERIPHERAL_TYPE") ||
        errorMsg.startsWith("UNSUPPORTED_PERIPHERAL_TYPE")) return 422;
    return 400;
}

bool applyServoParams(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                      PeripheralConfig& config, bool resetDefaults,
                      String& errorMsg) {
    if (resetDefaults) initializeServoDefaults(config);
    const auto& current = config.params.servo;
    const int frequency = ctx->getParamInt(request, "frequency", current.frequency);
    const int minPulse = ctx->getParamInt(request, "minPulse", current.minPulse);
    const int maxPulse = ctx->getParamInt(request, "maxPulse", current.maxPulse);
    const int minAngle = ctx->getParamInt(request, "minAngle", current.minAngle);
    const int maxAngle = ctx->getParamInt(request, "maxAngle", current.maxAngle);
    const int initialAngle = ctx->getParamInt(request, "initialAngle", current.initialAngle);

    if (frequency < 40 || frequency > 400 || minPulse < 100 || minPulse > 5000 ||
        maxPulse < 100 || maxPulse > 5000 || minPulse >= maxPulse ||
        maxPulse >= (1000000 / frequency) || minAngle < 0 || maxAngle > 360 ||
        minAngle >= maxAngle || initialAngle < minAngle || initialAngle > maxAngle) {
        errorMsg = "INVALID_SERVO_CONFIG: 频率、脉宽或角度范围无效";
        return false;
    }

    config.params.servo.frequency = static_cast<uint32_t>(frequency);
    config.params.servo.minPulse = static_cast<uint16_t>(minPulse);
    config.params.servo.maxPulse = static_cast<uint16_t>(maxPulse);
    config.params.servo.minAngle = static_cast<int16_t>(minAngle);
    config.params.servo.maxAngle = static_cast<int16_t>(maxAngle);
    config.params.servo.initialAngle = static_cast<int16_t>(initialAngle);
    config.params.servo.inverted = ctx->getParamBool(request, "inverted", current.inverted);
    config.params.servo.releaseAfterMove = ctx->getParamBool(
        request, "releaseAfterMove", current.releaseAfterMove);
    return true;
}

bool applyP1PeripheralParams(WebHandlerContext* ctx,
                             AsyncWebServerRequest* request,
                             PeripheralConfig& config, bool resetDefaults,
                             String& errorMsg) {
    if (config.type == PeripheralType::SDIO) {
        if (resetDefaults) initializeSdCardDefaults(config);
        config.params.sdcard.interface = static_cast<uint8_t>(
            ctx->getParamInt(request, "interface", config.params.sdcard.interface));
        const int frequency = ctx->getParamInt(
            request, "frequency", static_cast<int>(config.params.sdcard.frequency));
        if (config.params.sdcard.interface != SDCARD_INTERFACE_SPI ||
            frequency < 100000 || frequency > 40000000) {
            errorMsg = "INVALID_SD_CONFIG: 仅支持 100kHz-40MHz 的 SDSPI";
            return false;
        }
        config.params.sdcard.frequency = static_cast<uint32_t>(frequency);
    } else if (config.type == PeripheralType::DS1302_RTC) {
        if (resetDefaults) initializeDs1302Defaults(config);
        if (!FastBee::parseRtcBootSeedForm(
            request->hasParam("bootSeed", true) || request->hasParam("bootSeed"),
            ctx->getParamValue(request, "bootSeed", ""),
            request->hasParam("utcOffsetMinutes", true) || request->hasParam("utcOffsetMinutes"),
            ctx->getParamValue(request, "utcOffsetMinutes", ""),
            config.params.ds1302.bootSeed, config.params.ds1302.utcOffsetMinutes, errorMsg)) return false;
        config.params.ds1302.use24Hour = ctx->getParamBool(
            request, "use24Hour", config.params.ds1302.use24Hour);
        const int charge = ctx->getParamInt(
            request, "trickleCharge", config.params.ds1302.trickleCharge);
        switch (charge) {
            case 0x00: case 0xA5: case 0xA6: case 0xA7:
            case 0xA9: case 0xAA: case 0xAB:
                config.params.ds1302.trickleCharge = static_cast<uint8_t>(charge);
                break;
            default:
                errorMsg = "INVALID_DS1302_CHARGE_MODE";
                return false;
        }
    } else if (config.type == PeripheralType::LCD1602) {
        if (resetDefaults) initializeLcd1602Defaults(config);
        const int cols = ctx->getParamInt(request, "cols", config.params.lcd1602.cols);
        const int rows = ctx->getParamInt(request, "rows", config.params.lcd1602.rows);
        const int address = ctx->getParamInt(request, "address", config.params.lcd1602.address);
        if (!((cols == 16 && rows == 2) || (cols == 20 && rows == 4)) ||
            (address != 0 && !((address >= 0x20 && address <= 0x27) ||
                               (address >= 0x38 && address <= 0x3F)))) {
            errorMsg = "INVALID_LCD1602_CONFIG: 尺寸或 I2C 地址无效";
            return false;
        }
        config.params.lcd1602.cols = static_cast<uint8_t>(cols);
        config.params.lcd1602.rows = static_cast<uint8_t>(rows);
        config.params.lcd1602.address = static_cast<uint8_t>(address);
        config.params.lcd1602.backlight = ctx->getParamBool(
            request, "backlight", config.params.lcd1602.backlight);
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        if (resetDefaults) initializeRfidDefaults(config);
        config.params.rfid.frequency = RFID_DEFAULT_FREQUENCY;
    } else if (config.type == PeripheralType::SENSOR_GENERIC) {
        if (resetDefaults) initializeSensorDefaults(config);
        JsonDocument sensorParams;
        const char* const keys[] = {"sensorType", "sampleInterval", "driver", "driverName",
            "address", "i2cAddress", "mode", "frequency", "busPeripheralId",
            "shuntResistance", "maxCurrent", "conversionTimeUs", "averaging"};
        for (const char* key : keys) {
            if (request->hasParam(key, true) || request->hasParam(key)) {
                sensorParams[key] = ctx->getParamValue(request, key, "");
            }
        }
        if (sensorParams.overflowed()) { errorMsg = "SENSOR_PARAMS_MEMORY_UNAVAILABLE"; return false; }
        if (!FastBee::parseSensorConfig(sensorParams.as<JsonObjectConst>(), config, errorMsg) ||
            !FastBee::validateSensorConfig(config, errorMsg)) return false;
    }
    return true;
}

void initializeAdcDefaults(PeripheralConfig& config) {
    config.params.adc.attenuation = 3;
    config.params.adc.resolution = 12;
    config.params.adc.sampleRate = 0;
    config.params.adc.sampleCount = 1;
    config.params.adc.filterMode = 0;
    config.params.adc.signalMode = static_cast<uint8_t>(AnalogSignalMode::RAW);
    config.params.adc.conversionEnabled = false;
    config.params.adc.clampEnabled = false;
    config.params.adc.calibratedVoltage = false;
    config.params.adc.decimalPlaces = 2;
    config.params.adc.inputMin = 0.0f;
    config.params.adc.inputMax = 0.0f;
    config.params.adc.engineeringMin = 0.0f;
    config.params.adc.engineeringMax = 100.0f;
    config.params.adc.engineeringOffset = 0.0f;
    config.params.adc.vRef = 0.0f;
    config.params.adc.voltageRatio = 0.0f;
    config.params.adc.shuntResistance = 0.0f;
    config.params.adc.inputGain = 0.0f;
    config.params.adc.inputOffset = 0.0f;
    config.params.adc.rawMin = 0;
    config.params.adc.rawMax = 0;
}

float getRequestFloat(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                      const char* name, float fallback) {
    String raw = ctx->getParamValue(request, name, "");
    if (raw.isEmpty()) return fallback;
    float value = raw.toFloat();
    return (isnan(value) || isinf(value)) ? fallback : value;
}

uint8_t normalizeAdcSampleCount(int value, uint8_t fallback = 1);
uint8_t normalizeAdcFilterMode(int value);

int clampParamInt(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

void applyDigitalInputParams(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                             PeripheralConfig& config) {
    int inputMode = ctx->getParamInt(request, "inputMode", config.params.gpio.inputMode);
    if (inputMode < static_cast<int>(DigitalInputMode::AUTO) ||
        inputMode > static_cast<int>(DigitalInputMode::PULL_DOWN)) {
        inputMode = static_cast<int>(DigitalInputMode::AUTO);
    }
    config.params.gpio.inputMode = static_cast<uint8_t>(inputMode);
    config.params.gpio.invertLogic = ctx->getParamBool(request, "invertLogic",
                                                        config.params.gpio.invertLogic);
    int debounceMs = ctx->getParamInt(request, "debounceMs", config.params.gpio.debounceMs);
    if (debounceMs < 0) debounceMs = 0;
    if (debounceMs > 5000) debounceMs = 5000;
    config.params.gpio.debounceMs = static_cast<uint16_t>(debounceMs);
    int reportMode = ctx->getParamInt(request, "reportMode", config.params.gpio.reportMode);
    if (reportMode < static_cast<int>(DigitalReportMode::ON_CHANGE) ||
        reportMode > static_cast<int>(DigitalReportMode::EVENT_ONLY)) {
        reportMode = static_cast<int>(DigitalReportMode::ON_CHANGE);
    }
    config.params.gpio.reportMode = static_cast<uint8_t>(reportMode);
    const String peerId = ctx->getParamValue(request, "pairedInputId", config.params.gpio.pairedInputId);
    if (peerId.length() >= sizeof(config.params.gpio.pairedInputId)) {
        // Keep invalid input detectable by validateConfig; never silently target
        // a different peripheral by truncating the requested ID.
        memset(config.params.gpio.pairedInputId, '?', sizeof(config.params.gpio.pairedInputId));
    } else {
        peerId.toCharArray(config.params.gpio.pairedInputId, sizeof(config.params.gpio.pairedInputId));
    }
    const int pairMs = ctx->getParamInt(request, "pairDebounceMs", config.params.gpio.pairDebounceMs);
    config.params.gpio.pairDebounceMs = (pairMs == 0 || (pairMs >= 20 && pairMs <= 5000))
        ? static_cast<uint16_t>(pairMs) : UINT16_MAX;
}

void applyAnalogInputParams(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                            PeripheralConfig& config, bool resetDefaults = false) {
    if (resetDefaults) initializeAdcDefaults(config);
    auto& adc = config.params.adc;
    const int attenuationDefault = resetDefaults ? 3 : adc.attenuation;
    const int resolutionDefault = (resetDefaults || adc.resolution < 9 || adc.resolution > 12)
        ? 12 : adc.resolution;
    adc.attenuation = static_cast<uint8_t>(clampParamInt(
        ctx->getParamInt(request, "attenuation", attenuationDefault), 0, 3));
    adc.resolution = static_cast<uint8_t>(clampParamInt(
        ctx->getParamInt(request, "resolution", resolutionDefault), 9, 12));
    adc.sampleRate = static_cast<uint16_t>(clampParamInt(
        ctx->getParamInt(request, "sampleRate", adc.sampleRate), 0, 65535));
    adc.sampleCount = normalizeAdcSampleCount(
        ctx->getParamInt(request, "sampleCount", adc.sampleCount ? adc.sampleCount : 1));
    adc.filterMode = normalizeAdcFilterMode(
        ctx->getParamInt(request, "filterMode", adc.filterMode));

    int signalMode = clampParamInt(ctx->getParamInt(
        request, "signalMode", adc.signalMode),
        static_cast<int>(AnalogSignalMode::RAW),
        static_cast<int>(AnalogSignalMode::CUSTOM_LINEAR));
    adc.signalMode = static_cast<uint8_t>(signalMode);
    adc.conversionEnabled = ctx->getParamBool(request, "conversionEnabled", adc.conversionEnabled);
    adc.clampEnabled = ctx->getParamBool(request, "clampEnabled", adc.clampEnabled);
    adc.calibratedVoltage = ctx->getParamBool(request, "calibratedVoltage", adc.calibratedVoltage);

    int decimalPlaces = ctx->getParamInt(request, "decimalPlaces", adc.decimalPlaces);
    if (decimalPlaces < 0) decimalPlaces = 0;
    if (decimalPlaces > 6) decimalPlaces = 6;
    adc.decimalPlaces = static_cast<uint8_t>(decimalPlaces);

    adc.inputMin = getRequestFloat(ctx, request, "inputMin", adc.inputMin);
    adc.inputMax = getRequestFloat(ctx, request, "inputMax", adc.inputMax);
    adc.engineeringMin = getRequestFloat(ctx, request, "engineeringMin", adc.engineeringMin);
    adc.engineeringMax = getRequestFloat(ctx, request, "engineeringMax",
                                         adc.engineeringMax == 0.0f ? 100.0f : adc.engineeringMax);
    adc.engineeringOffset = getRequestFloat(ctx, request, "engineeringOffset", adc.engineeringOffset);
    adc.vRef = getRequestFloat(ctx, request, "vRef", adc.vRef);
    adc.voltageRatio = getRequestFloat(ctx, request, "voltageRatio", adc.voltageRatio);
    adc.shuntResistance = getRequestFloat(ctx, request, "shuntResistance", adc.shuntResistance);
    adc.inputGain = getRequestFloat(ctx, request, "inputGain", adc.inputGain);
    adc.inputOffset = getRequestFloat(ctx, request, "inputOffset", adc.inputOffset);

    adc.rawMin = static_cast<uint16_t>(clampParamInt(
        ctx->getParamInt(request, "rawMin", adc.rawMin), 0, 65535));
    adc.rawMax = static_cast<uint16_t>(clampParamInt(
        ctx->getParamInt(request, "rawMax", adc.rawMax), 0, 65535));
}

// GPIO_ANALOG_INPUT is a legacy AI spelling, but it uses the same ADC
// parameter union as the dedicated ADC type. Keep it out of the generic GPIO
// branch so its resolution/filter settings survive API round trips.
uint8_t normalizeAdcSampleCount(int value, uint8_t fallback) {
    if (value <= 0) value = fallback > 0 ? fallback : 1;
    if (value > 16) value = 16;
    return static_cast<uint8_t>(value);
}

uint8_t normalizeAdcFilterMode(int value) {
    return value == 1 ? 1 : 0;
}

bool parsePeripheralPins(const String& value, uint8_t* pins, uint8_t& count,
                         String& errorMsg) {
    count = 0;
    if (value.isEmpty()) return true;
    int start = 0;
    while (start <= static_cast<int>(value.length())) {
        const int comma = value.indexOf(',', start);
        String part = comma < 0 ? value.substring(start) : value.substring(start, comma);
        part.trim();
        if (part.isEmpty()) {
            errorMsg = "INVALID_PIN_LIST: 引脚列表包含空项";
            return false;
        }
        if (count >= 8) {
            errorMsg = "INVALID_PIN_LIST: 最多允许 8 个引脚";
            return false;
        }
        for (size_t i = 0; i < part.length(); ++i) {
            if (part[i] < '0' || part[i] > '9') {
                errorMsg = String("INVALID_PIN_LIST: 非法引脚 '") + part + "'";
                return false;
            }
        }
        const long pin = part.toInt();
        if (pin < 0 || pin > 254) {
            errorMsg = String("INVALID_PIN_LIST: 引脚超出范围 '") + part + "'";
            return false;
        }
        pins[count++] = static_cast<uint8_t>(pin);
        if (comma < 0) break;
        start = comma + 1;
    }
    return true;
}

bool hasPeripheralRequestField(AsyncWebServerRequest* request, const char* name) {
    return request->hasParam(name, true) || request->hasParam(name);
}

bool readDisplayRequestInteger(WebHandlerContext* ctx,
                               AsyncWebServerRequest* request,
                               const char* name, int32_t fallback,
                               int32_t minimum, int32_t maximum,
                               int32_t& value, String& error) {
    value = fallback;
    if (!hasPeripheralRequestField(request, name)) {
        if (value >= minimum && value <= maximum) return true;
        error = String("DISPLAY_PARAMS_INVALID: invalid ") + name;
        return false;
    }

    String text = ctx->getParamValue(request, name, "");
    text.trim();
    size_t offset = 0;
    bool negative = false;
    if (text.length() > 0 && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        offset = 1;
    }
    uint64_t magnitude = 0;
    bool valid = text.length() > offset;
    for (size_t i = offset; valid && i < text.length(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            valid = false;
            break;
        }
        const uint8_t digit = static_cast<uint8_t>(c - '0');
        if (magnitude > (static_cast<uint64_t>(INT32_MAX) + 1ULL - digit) / 10ULL) {
            valid = false;
            break;
        }
        magnitude = magnitude * 10ULL + digit;
    }
    const int64_t parsed = negative
        ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    if (!valid || parsed < minimum || parsed > maximum) {
        error = String("DISPLAY_PARAMS_INVALID: invalid ") + name;
        return false;
    }
    value = static_cast<int32_t>(parsed);
    return true;
}

bool applyDisplayRequestParameters(WebHandlerContext* ctx,
                                   AsyncWebServerRequest* request,
                                   PeripheralConfig& config,
                                   bool resetDefaults, String& error) {
    if (resetDefaults || config.params.display.backend[0] == '\0') {
        initializeDisplayDefaults(config);
    }
    if (config.pinCount != 0) {
        error = "DISPLAY_PINS_INVALID: use named display pins, not pins[]";
        return false;
    }

    DisplayConfigParams params = config.params.display;
    String backend = ctx->getParamValue(
        request, "displayBackend", params.backend);
    String driver = ctx->getParamValue(
        request, "displayDriver", params.driver);
    String touch = ctx->getParamValue(
        request, "touchBackend", params.touchBackend);
    backend.trim();
    driver.trim();
    touch.trim();
    backend.toLowerCase();
    driver.toLowerCase();
    touch.toLowerCase();
    const bool rgb = backend == "rgb" && driver == "st7701";
    if ((!rgb && (backend != "spi_tft" ||
        (driver != "st7789" && driver != "ili9341"))) ||
        (touch != "none" && touch != "gt911")) {
        error = "DISPLAY_BACKEND_UNSUPPORTED: expected spi_tft st7789/ili9341 or rgb st7701 and none/gt911";
        return false;
    }

    int32_t width, height, rotation, colorDepth, bufferLines, brightness;
    int32_t backlightChannel, spiFrequency, i2cFrequency;
    int32_t mosi, sclk, cs, dc, rst, backlight;
    int32_t touchSda, touchScl, touchInt, touchRst;
    if (!readDisplayRequestInteger(ctx, request, "width", params.width,
            rgb ? 480 : 128, rgb ? 480 : 320, width, error) ||
        !readDisplayRequestInteger(ctx, request, "height", params.height,
            rgb ? 480 : 128, rgb ? 480 : 320, height, error) ||
        !readDisplayRequestInteger(ctx, request, "rotation", params.rotation,
            0, 3, rotation, error) ||
        !readDisplayRequestInteger(ctx, request, "colorDepth", params.colorDepth,
            16, 16, colorDepth, error) ||
        !readDisplayRequestInteger(ctx, request, "bufferLines", params.bufferLines,
            1, 64, bufferLines, error) ||
        !readDisplayRequestInteger(ctx, request, "brightness", params.brightness,
            0, 100, brightness, error) ||
        !readDisplayRequestInteger(ctx, request, "backlightChannel", params.backlightChannel,
            0, CHIP_MAX_PWM_CH > 0 ? CHIP_MAX_PWM_CH - 1 : 0, backlightChannel, error) ||
        !readDisplayRequestInteger(ctx, request, "spiFrequency",
            static_cast<int32_t>(params.spiFrequency), 1000000, 80000000,
            spiFrequency, error) ||
        !readDisplayRequestInteger(ctx, request, "i2cFrequency",
            static_cast<int32_t>(params.i2cFrequency), 100000, 400000,
            i2cFrequency, error) ||
        !readDisplayRequestInteger(ctx, request, "mosi", params.mosi,
            -1, 127, mosi, error) ||
        !readDisplayRequestInteger(ctx, request, "sclk", params.sclk,
            -1, 127, sclk, error) ||
        !readDisplayRequestInteger(ctx, request, "cs", params.cs,
            -1, 127, cs, error) ||
        !readDisplayRequestInteger(ctx, request, "dc", params.dc,
            -1, 127, dc, error) ||
        !readDisplayRequestInteger(ctx, request, "rst", params.rst,
            -1, 127, rst, error) ||
        !readDisplayRequestInteger(ctx, request, "backlight", params.backlight,
            -1, 127, backlight, error) ||
        !readDisplayRequestInteger(ctx, request, "touchSda", params.touchSda,
            -1, 127, touchSda, error) ||
        !readDisplayRequestInteger(ctx, request, "touchScl", params.touchScl,
            -1, 127, touchScl, error) ||
        !readDisplayRequestInteger(ctx, request, "touchInt", params.touchInt,
            -1, 127, touchInt, error) ||
        !readDisplayRequestInteger(ctx, request, "touchRst", params.touchRst,
            -1, 127, touchRst, error)) {
        return false;
    }
    if (i2cFrequency != 100000 && i2cFrequency != 400000) {
        error = "DISPLAY_PARAMS_INVALID: i2cFrequency must be 100000 or 400000";
        return false;
    }

    copyPeripheralConfigText(params.backend, backend.c_str());
    copyPeripheralConfigText(params.driver, driver.c_str());
    copyPeripheralConfigText(params.touchBackend, touch.c_str());
    params.width = static_cast<uint16_t>(width);
    params.height = static_cast<uint16_t>(height);
    params.rotation = static_cast<uint8_t>(rotation);
    params.colorDepth = static_cast<uint8_t>(colorDepth);
    params.bufferLines = static_cast<uint8_t>(bufferLines);
    params.brightness = static_cast<uint8_t>(brightness);
    params.backlightChannel = static_cast<uint8_t>(backlightChannel);
    params.backlightActiveHigh = ctx->getParamBool(
        request, "backlightActiveHigh", params.backlightActiveHigh);
    params.spiFrequency = static_cast<uint32_t>(spiFrequency);
    params.i2cFrequency = static_cast<uint32_t>(i2cFrequency);
    params.mosi = static_cast<int8_t>(mosi);
    params.sclk = static_cast<int8_t>(sclk);
    params.cs = static_cast<int8_t>(cs);
    params.dc = static_cast<int8_t>(dc);
    params.rst = static_cast<int8_t>(rst);
    params.backlight = static_cast<int8_t>(backlight);
    params.touchSda = static_cast<int8_t>(touchSda);
    params.touchScl = static_cast<int8_t>(touchScl);
    params.touchInt = static_cast<int8_t>(touchInt);
    params.touchRst = static_cast<int8_t>(touchRst);
    String control = ctx->getParamValue(request, "controlBackend", params.controlBackend);
    String profile = ctx->getParamValue(request, "rgbInitProfile", params.rgbInitProfile);
    control.trim(); profile.trim();
    if ((control != "none" && control != "tca9554" && control != "ch32v003") ||
        profile != "st7701_type1") {
        error = "DISPLAY_PARAMS_INVALID: unsupported controlBackend or rgbInitProfile";
        return false;
    }
    copyPeripheralConfigText(params.controlBackend, control.c_str());
    copyPeripheralConfigText(params.rgbInitProfile, profile.c_str());
    int32_t extra;
#define READ_DISPLAY_EXTRA(field, minimum, maximum) \
    if (!readDisplayRequestInteger(ctx, request, #field, params.field, minimum, maximum, extra, error)) return false; \
    params.field = static_cast<decltype(params.field)>(extra)
    READ_DISPLAY_EXTRA(rgbFrequency, 1000000, 16000000);
    READ_DISPLAY_EXTRA(hsyncFrontPorch, 0, 1023);
    READ_DISPLAY_EXTRA(hsyncPulseWidth, 1, 1023);
    READ_DISPLAY_EXTRA(hsyncBackPorch, 0, 1023);
    READ_DISPLAY_EXTRA(vsyncFrontPorch, 0, 1023);
    READ_DISPLAY_EXTRA(vsyncPulseWidth, 1, 1023);
    READ_DISPLAY_EXTRA(vsyncBackPorch, 0, 1023);
    READ_DISPLAY_EXTRA(rgbDe, -1, 127);
    READ_DISPLAY_EXTRA(rgbVsync, -1, 127);
    READ_DISPLAY_EXTRA(rgbHsync, -1, 127);
    READ_DISPLAY_EXTRA(rgbPclk, -1, 127);
    READ_DISPLAY_EXTRA(controlSda, -1, 127);
    READ_DISPLAY_EXTRA(controlScl, -1, 127);
    READ_DISPLAY_EXTRA(controlAddress, 0, 127);
    READ_DISPLAY_EXTRA(controlResetMask, 0, 255);
    READ_DISPLAY_EXTRA(controlBacklightMask, 0, 255);
    READ_DISPLAY_EXTRA(controlEnableMask, 0, 255);
    READ_DISPLAY_EXTRA(controlBuzzerMask, 0, 255);
    READ_DISPLAY_EXTRA(touchBuzzerDurationMs, 20, 150);
    READ_DISPLAY_EXTRA(controlInactiveMask, 0, 255);
    READ_DISPLAY_EXTRA(controlTouchIntMask, 0, 255);
#undef READ_DISPLAY_EXTRA
    if (!params.controlMasksDisjoint()) {
        error = "DISPLAY_CONTROL_INVALID: control masks must be disjoint";
        return false;
    }
    if (params.controlBuzzerMask && control != "ch32v003") {
        error = "DISPLAY_CONTROL_INVALID: buzzer mask requires ch32v003";
        return false;
    }
    if (!params.controlTouchSyncValid()) {
        error = "DISPLAY_CONTROL_INVALID: invalid touch INT reset control";
        return false;
    }
    params.hsyncIdleLow = ctx->getParamBool(request, "hsyncIdleLow", params.hsyncIdleLow);
    params.vsyncIdleLow = ctx->getParamBool(request, "vsyncIdleLow", params.vsyncIdleLow);
    params.pclkActiveNeg = ctx->getParamBool(request, "pclkActiveNeg", params.pclkActiveNeg);
    for (uint8_t i = 0; i < 16; ++i) {
        const String field = String("rgbData") + String(i);
        if (!readDisplayRequestInteger(ctx, request, field.c_str(), params.rgbDataPins[i],
                                       -1, 127, extra, error)) return false;
        params.rgbDataPins[i] = static_cast<int8_t>(extra);
    }
    config.params.display = params;
    return true;
}

bool readIoRequestInteger(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                         const char* name, uint32_t fallback, uint32_t minimum,
                         uint32_t maximum, uint32_t& value, String& error,
                         bool allowHex = false) {
    value = fallback;
    if (!hasPeripheralRequestField(request, name)) {
        if (value >= minimum && value <= maximum) return true;
        error = String("IO_CONFIG_INVALID: invalid ") + name;
        return false;
    }
    String text = ctx->getParamValue(request, name, "");
    text.trim();
    size_t offset = 0;
    uint32_t base = 10;
    if (allowHex && text.length() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        offset = 2;
    }
    value = 0;
    bool valid = text.length() > offset;
    for (size_t i = offset; valid && i < text.length(); ++i) {
        const char c = text[i];
        uint32_t digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit >= base || value > maximum / base ||
            (value == maximum / base && digit > maximum % base)) valid = false;
        else value = value * base + digit;
    }
    if (!valid || value < minimum || value > maximum) {
        error = String("IO_CONFIG_INVALID: invalid ") + name;
        return false;
    }
    return true;
}

bool applyIoPeripheralRequestParameters(WebHandlerContext* ctx,
                                       AsyncWebServerRequest* request,
                                       PeripheralConfig& config, bool resetDefaults,
                                       String& error) {
    if (config.type == PeripheralType::PCA9685_PWM) {
        const String encoded = ctx->getParamValue(request, "pca9685", "");
        JsonDocument parameters;
        if (encoded.isEmpty() || encoded.length() > 2048 ||
            deserializeJson(parameters, encoded.c_str(), encoded.length()) || !parameters.is<JsonObject>()) {
            error = "PCA_CONFIG_INVALID"; return false;
        }
        return FastBee::parsePca9685Config(parameters.as<JsonObjectConst>(), config, error);
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        auto params = config.params.ioExpander;
        String driver = ctx->getParamValue(request,
            hasPeripheralRequestField(request, "driver") ? "driver" : "driverName",
            !resetDefaults && params.driver[0] ? params.driver : "PCF8574");
        driver.trim();
        driver.toUpperCase();
        if (driver != "PCF8574" && driver != "MCP23017") {
            error = "IO_PROVIDER_INVALID: unsupported driver"; return false;
        }
        String reference = ctx->getParamValue(request, "busPeripheralId",
            resetDefaults ? "" : params.busPeripheralId);
        reference.trim();
        if (!FastBee::H1Parameter::acceptsReference(FastBee::H1Parameter::ioBus,
                reference.c_str(), reference.length())) {
            error = "IO_PROVIDER_INVALID: bus reference is too long"; return false;
        }
        String reset = ctx->getParamValue(request, "resetPeripheralId",
            resetDefaults ? "" : params.resetPeripheralId);
        reset.trim();
        if (driver == "MCP23017" ? !FastBee::H1Parameter::acceptsReference(
                FastBee::H1Parameter::mcpReset, reset.c_str(), reset.length()) : !reset.isEmpty()) {
            error = "RESET_REFERENCE_INVALID: MCP requires a dedicated native output reference"; return false;
        }
        uint32_t address = 0, interval = 0;
        if (!readIoRequestInteger(ctx, request,
                hasPeripheralRequestField(request, "address") ? "address" : "i2cAddress",
                !resetDefaults && params.address ? params.address : FastBee::H1Parameter::ioAddress.defaultValue,
                FastBee::H1Parameter::ioAddress.minimum, FastBee::H1Parameter::ioAddress.maximum, address, error, true) ||
            !readIoRequestInteger(ctx, request, "sampleInterval",
                !resetDefaults && params.sampleInterval ? params.sampleInterval : FastBee::H1Parameter::ioInterval.defaultValue,
                FastBee::H1Parameter::ioInterval.minimum, FastBee::H1Parameter::ioInterval.maximum, interval, error)) return false;
        if ((!reference.isEmpty() && config.pinCount != 0) ||
            (reference.isEmpty() && (config.pinCount != 2 || config.pins[0] == config.pins[1]))) {
            error = "I2C_REFERENCE_INVALID: choose a bus reference or two distinct SDA/SCL pins";
            return false;
        }
        strlcpy(params.driver, driver.c_str(), sizeof(params.driver));
        strlcpy(params.resetPeripheralId, reset.c_str(), sizeof(params.resetPeripheralId));
        strlcpy(params.busPeripheralId, reference.c_str(), sizeof(params.busPeripheralId));
        params.address = static_cast<uint8_t>(address);
        params.sampleInterval = static_cast<uint16_t>(interval);
        config.params.ioExpander = params;
    } else if (config.isGPIOPeripheral() && !isAnalogInputConfigType(config.type)) {
        auto params = config.params.gpio;
        String source = ctx->getParamValue(request, "ioSource",
            !resetDefaults && params.ioSource == 1 ? "expander" : "gpio");
        source.trim();
        source.toLowerCase();
        if (source != "gpio" && source != "0" && source != "expander" && source != "1") {
            error = "IO_CHANNEL_INVALID: invalid ioSource"; return false;
        }
        params.ioSource = source == "expander" || source == "1" ? 1 : 0;
        if (!params.ioSource) {
            params.providerId[0] = '\0';
            params.channel = 0;
        } else {
            if ((config.type != PeripheralType::GPIO_DIGITAL_INPUT &&
                 config.type != PeripheralType::GPIO_DIGITAL_OUTPUT) || config.pinCount != 0) {
                error = "IO_CHANNEL_INVALID: use plain digital input/output without MCU pins";
                return false;
            }
            String provider = ctx->getParamValue(request, "providerId",
                resetDefaults ? "" : params.providerId);
            provider.trim();
            if (provider.isEmpty() || provider.length() >= sizeof(params.providerId)) {
                error = "IO_CHANNEL_INVALID: providerId is required and must fit 40 characters";
                return false;
            }
            uint32_t channel = 0, initial = 0, inputMode = 0;
            if (!readIoRequestInteger(ctx, request, "channel", resetDefaults ? 0 : params.channel,
                    0, 15, channel, error) ||
                !readIoRequestInteger(ctx, request, "initialState", static_cast<uint32_t>(params.initialState),
                    0, 1, initial, error) ||
                !readIoRequestInteger(ctx, request, "inputMode", params.inputMode,
                    0, 0, inputMode, error)) return false;
            String inverted = ctx->getParamValue(request, "invertLogic", params.invertLogic ? "true" : "false");
            inverted.trim();
            inverted.toLowerCase();
            if (inverted != "true" && inverted != "false" && inverted != "1" && inverted != "0") {
                error = "IO_CHANNEL_INVALID: invalid invertLogic"; return false;
            }
            strlcpy(params.providerId, provider.c_str(), sizeof(params.providerId));
            params.channel = static_cast<uint8_t>(channel);
            params.initialState = static_cast<GPIOState>(initial);
            params.inputMode = static_cast<uint8_t>(inputMode);
            params.invertLogic = inverted == "true" || inverted == "1";
        }
        config.params.gpio = params;
    }
    return true;
}

bool applyPeripheralRequestParameters(WebHandlerContext* ctx,
                                      AsyncWebServerRequest* request,
                                      PeripheralConfig& config,
                                      bool resetDefaults,
                                      String& errorMsg) {
    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        if (!applyDisplayRequestParameters(ctx, request, config,
                                           resetDefaults, errorMsg)) {
            return false;
        }
    } else if (config.type == PeripheralType::UART) {
        if (resetDefaults) initializeUartDefaults(config);
        config.params.uart.baudRate = ctx->getParamInt(
            request, "baudRate", config.params.uart.baudRate ?
                config.params.uart.baudRate : UART_DEFAULT_BAUD_RATE);
        config.params.uart.dataBits = ctx->getParamInt(
            request, "dataBits", config.params.uart.dataBits ?
                config.params.uart.dataBits : UART_DEFAULT_DATA_BITS);
        config.params.uart.stopBits = ctx->getParamInt(
            request, "stopBits", config.params.uart.stopBits ?
                config.params.uart.stopBits : UART_DEFAULT_STOP_BITS);
        config.params.uart.parity = ctx->getParamInt(
            request, "parity", config.params.uart.parity);
    } else if (config.type == PeripheralType::ENCODER) {
        if (resetDefaults) initializeEncoderDefaults(config);
        const int resolution = ctx->getParamInt(
            request, "resolution", config.params.encoder.resolution ?
                config.params.encoder.resolution : ENCODER_DEFAULT_RESOLUTION);
        if (resolution < 1 || resolution > UINT16_MAX) {
            errorMsg =
                "INVALID_ENCODER_RESOLUTION: expected 1-65535 pulses per revolution";
            return false;
        }
        config.params.encoder.resolution = static_cast<uint16_t>(resolution);
        config.params.encoder.useInterrupt = ctx->getParamBool(
            request, "useInterrupt",
            resetDefaults ? true : config.params.encoder.useInterrupt);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = ctx->getParamInt(
            request, "frequency", resetDefaults || !config.params.i2c.frequency
                ? 100000 : config.params.i2c.frequency);
        config.params.i2c.address = ctx->getParamInt(
            request, "address", config.params.i2c.address);
        config.params.i2c.isMaster = ctx->getParamBool(
            request, "isMaster", resetDefaults ? true : config.params.i2c.isMaster);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = ctx->getParamInt(
            request, "frequency", resetDefaults || !config.params.spi.frequency
                ? 1000000 : config.params.spi.frequency);
        config.params.spi.mode = ctx->getParamInt(
            request, "mode", config.params.spi.mode);
        config.params.spi.msbFirst = ctx->getParamBool(
            request, "msbFirst", resetDefaults ? true : config.params.spi.msbFirst);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (!applyServoParams(ctx, request, config, resetDefaults, errorMsg)) {
            return false;
        }
    } else if (config.isGPIOPeripheral() &&
               !isAnalogInputConfigType(config.type)) {
        config.params.gpio.initialState = static_cast<GPIOState>(ctx->getParamInt(
            request, "initialState", resetDefaults ? 0 :
                static_cast<int>(config.params.gpio.initialState)));
        config.params.gpio.pwmChannel = ctx->getParamInt(
            request, "pwmChannel", resetDefaults ? 0 : config.params.gpio.pwmChannel);
        config.params.gpio.pwmFrequency = ctx->getParamInt(
            request, "pwmFrequency", resetDefaults ? 1000 :
                config.params.gpio.pwmFrequency);
        config.params.gpio.pwmResolution = ctx->getParamInt(
            request, "pwmResolution", resetDefaults ? 8 :
                config.params.gpio.pwmResolution);
        config.params.gpio.defaultDuty = ctx->getParamInt(
            request, "defaultDuty", resetDefaults ? 0 :
                config.params.gpio.defaultDuty);
        config.params.gpio.passive = ctx->getParamBool(
            request, "passive", resetDefaults ? false : config.params.gpio.passive);
        if (isDigitalInputPeripheral(config.type)) {
            applyDigitalInputParams(ctx, request, config);
        }
    } else if (isAnalogInputConfigType(config.type)) {
        applyAnalogInputParams(ctx, request, config, resetDefaults);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = ctx->getParamInt(
            request, "channel", resetDefaults ? 1 : config.params.dac.channel);
        config.params.dac.defaultValue = ctx->getParamInt(
            request, "defaultValue", resetDefaults ? 0 :
                config.params.dac.defaultValue);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = ctx->getParamInt(
            request, "stepsPerRevolution",
            resetDefaults || !config.params.stepper.stepsPerRevolution
                ? 2048 : config.params.stepper.stepsPerRevolution);
        config.params.stepper.speed = ctx->getParamInt(
            request, "speed", resetDefaults || !config.params.stepper.speed
                ? 8 : config.params.stepper.speed);
    } else if (config.type == PeripheralType::LCD) {
        const int width = ctx->getParamInt(
            request, "width", resetDefaults || !config.params.lcd.width
                ? 128 : config.params.lcd.width);
        const int height = ctx->getParamInt(
            request, "height", resetDefaults || !config.params.lcd.height
                ? 64 : config.params.lcd.height);
        const int interfaceValue = ctx->getParamInt(
            request, "interface", resetDefaults ? 2 : config.params.lcd.interface);
        const int contrast = ctx->getParamInt(
            request, "contrast", resetDefaults || !config.params.lcd.contrast
                ? 255 : config.params.lcd.contrast);
        config.params.lcd.width = static_cast<uint8_t>(
            width > 255 ? 255 : (width < 0 ? 0 : width));
        config.params.lcd.height = static_cast<uint8_t>(
            height > 255 ? 255 : (height < 0 ? 0 : height));
        config.params.lcd.interface = static_cast<uint8_t>(
            interfaceValue < 0 ? 2 : interfaceValue);
        config.params.lcd.contrast = static_cast<uint8_t>(
            contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = ctx->getParamInt(
            request, "count", resetDefaults || !config.params.neopixel.count
                ? 1 : config.params.neopixel.count);
        int brightness = ctx->getParamInt(
            request, "brightness", resetDefaults ? 64 :
                config.params.neopixel.brightness);
        count = clampParamInt(count, 1, 64);
        brightness = clampParamInt(brightness, 0, 255);
        config.params.neopixel.count = static_cast<uint16_t>(count);
        config.params.neopixel.brightness = static_cast<uint8_t>(brightness);
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = ctx->getParamInt(
            request, "mode", resetDefaults ? 0 : config.params.rf.mode);
        int pulseWidth = ctx->getParamInt(
            request, "pulseWidth", resetDefaults || !config.params.rf.pulseWidth
                ? 350 : config.params.rf.pulseWidth);
        int repeat = ctx->getParamInt(
            request, "repeat", resetDefaults || !config.params.rf.repeat
                ? 8 : config.params.rf.repeat);
        int bitLength = ctx->getParamInt(
            request, "bitLength", resetDefaults || !config.params.rf.bitLength
                ? 24 : config.params.rf.bitLength);
        if (mode != 1) mode = 0;
        if (pulseWidth < 100) pulseWidth = 350;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 1) repeat = 8;
        if (repeat > 20) repeat = 20;
        if (bitLength < 1) bitLength = 24;
        if (bitLength > 32) bitLength = 32;
        config.params.rf.mode = static_cast<uint8_t>(mode);
        config.params.rf.pulseWidth = static_cast<uint16_t>(pulseWidth);
        config.params.rf.repeat = static_cast<uint8_t>(repeat);
        config.params.rf.bitLength = static_cast<uint8_t>(bitLength);
        config.params.rf.activeHigh = ctx->getParamBool(
            request, "activeHigh", resetDefaults ? true : config.params.rf.activeHigh);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = ctx->getParamInt(
            request, "debounceMs", resetDefaults || !config.params.radar.debounceMs
                ? 50 : config.params.radar.debounceMs);
        int holdMs = ctx->getParamInt(
            request, "holdMs", resetDefaults || !config.params.radar.holdMs
                ? 2000 : config.params.radar.holdMs);
        debounceMs = clampParamInt(debounceMs, 0, 5000);
        holdMs = clampParamInt(holdMs, 0, 60000);
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = ctx->getParamBool(
            request, "activeHigh", resetDefaults ? true :
                config.params.radar.activeHigh);
        config.params.radar.debounceMs = static_cast<uint16_t>(debounceMs);
        config.params.radar.holdMs = static_cast<uint16_t>(holdMs);
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        if (!applyP1PeripheralParams(ctx, request, config, resetDefaults,
                                     errorMsg)) {
            return false;
        }
    } else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int brightness = ctx->getParamInt(
            request, "brightness", resetDefaults ? 2 :
                config.params.segment.brightness);
        config.params.segment.brightness = static_cast<uint8_t>(
            clampParamInt(brightness, 0, 7));
    }
    return applyIoPeripheralRequestParameters(ctx, request, config, resetDefaults, errorMsg);
}

bool isListMemoryCriticallyLow() {
    return FastBee::MemoryBudget::dramFree() < 4096 || FastBee::MemoryBudget::dramLargest() < 1536;
}

bool shouldForceCompactList() {
    return FastBee::MemoryBudget::dramFree() < 16384 || FastBee::MemoryBudget::dramLargest() < 8192;
}

String escapePeriphJsonString(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// 无源蜂鸣器标识序列化：GPIO数字输出 + passive 时写入 passive=true。
// 前端据此将动作类型映射为“设置PWM”（无源蜂鸣器由 PWM 占空比驱动）。
// 提取为唯一辅助函数，避免多个序列化点（列表/单条/详情）逻辑重复与不一致。
template <typename TJson>
void addPassiveBuzzerFlag(TJson& doc, const PeripheralConfig& config) {
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive) {
        doc["passive"] = true;
    }
}

String mcpGroupReviewToken(uint64_t version) {
    if (!version) return "";
    // An optimistic review token, not an authorization credential. Auth and
    // developer-mode admission remain mandatory on the mutation endpoint.
    static const uint32_t boot[] = {esp_random(), esp_random(), esp_random(), esp_random()};
    char value[49];
    snprintf(value, sizeof(value), "%08lx%08lx%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(boot[0]), static_cast<unsigned long>(boot[1]),
        static_cast<unsigned long>(boot[2]), static_cast<unsigned long>(boot[3]),
        static_cast<unsigned long>(version >> 32), static_cast<unsigned long>(version & UINT32_MAX));
    return String(value);
}

bool buildMcpGroupPreview(PeripheralManager& manager, const String& id,
                          JsonDocument& doc, String& error) {
    RecursiveMutexGuard lock(manager.getMutex(), pdMS_TO_TICKS(50));
    if (!lock.isLocked()) { error = "IO_GROUP_BUSY: configuration unavailable"; return false; }
    PeripheralConfig parent;
    if (!manager.getPeripheralSnapshot(id, parent) || parent.type != PeripheralType::IO_EXPANDER ||
        strcasecmp(parent.params.ioExpander.driver, "MCP23017") != 0 || parent.enabled) {
        error = "IO_GROUP_INVALID: select a disabled MCP provider"; return false;
    }
    const String token = mcpGroupReviewToken(manager.getConfigurationReviewVersion());
    if (token.length() != 48) { error = "IO_GROUP_BUSY: review identity unavailable"; return false; }
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "id", id);
    FastBee::setFixedJsonField(data, "groupToken", token);
    auto describe = [](JsonObject object, const PeripheralConfig& config) {
        FastBee::setFixedJsonField(object, "id", config.id);
        FastBee::setFixedJsonField(object, "name", config.name);
        FastBee::setFixedJsonField(object, "type", static_cast<int>(config.type));
        FastBee::setFixedJsonField(object, "enabled", config.enabled);
        addGenericPeripheralFields(object, config);
        JsonArray pins = object["pins"].to<JsonArray>();
        for (uint8_t i = 0; i < config.pinCount && i < 8; ++i) pins.add(config.pins[i]);
        if (config.type == PeripheralType::I2C) {
            FastBee::setFixedJsonField(object, "frequency", config.params.i2c.frequency ? config.params.i2c.frequency : 100000);
        }
    };
    describe(data["provider"].to<JsonObject>(), parent);
    PeripheralConfig dependency;
    if (manager.getPeripheralSnapshot(parent.params.ioExpander.resetPeripheralId, dependency))
        describe(data["reset"].to<JsonObject>(), dependency);
    if (parent.params.ioExpander.busPeripheralId[0] &&
        manager.getPeripheralSnapshot(parent.params.ioExpander.busPeripheralId, dependency))
        describe(data["bus"].to<JsonObject>(), dependency);
    JsonArray channels = data["channels"].to<JsonArray>();
    size_t count = 0;
    manager.forEachPeripheral([&](const PeripheralConfig& child) {
        if (!child.isExpanderChannel() ||
            strncmp(child.params.gpio.providerId, id.c_str(), sizeof(child.params.gpio.providerId)) != 0) return;
        if (++count <= 16) describe(channels.add<JsonObject>(), child);
    });
    if (count == 0 || count > 16 || doc.overflowed()) {
        error = "IO_GROUP_INVALID: group incomplete or preview capacity exceeded"; return false;
    }
    return true;
}

constexpr uint32_t PREFLIGHT_ACK_MASK =
    FastBee::ACK_STRAPPING_PIN |
    FastBee::ACK_BOARD_SHARED_PIN |
    FastBee::ACK_UART0_CONSOLE |
    FastBee::ACK_ADC2_WITH_WIFI;

bool sameText(const char* left, const char* right) {
    if (!left || !right) return left == right;
    return strcmp(left, right) == 0;
}

bool compiledBoardProfileSupported() {
#if CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3 || CHIP_IS_ESP32C6 || CHIP_IS_ESP32S3
    return true;
#else
    return false;
#endif
}

const FastBee::BoardProfile* resolveBoardProfile(const String& profileId) {
    if (profileId.isEmpty()) {
        return compiledBoardProfileSupported()
            ? &FastBee::getCompiledBoardProfile() : nullptr;
    }
    const FastBee::BoardProfile& pz = FastBee::getPuzhongPzEsp32Profile();
    const FastBee::BoardProfile& c6 = FastBee::getEsp32C6DevKitC1Profile();
    if (profileId == pz.key || profileId == "0") return &pz;
    if (profileId == c6.key || profileId == "1") return &c6;
    const FastBee::BoardProfile& c3 = FastBee::getEsp32C3Profile();
    if (profileId == c3.key || profileId == "3") return &c3;
    const FastBee::BoardProfile& s3Octal = FastBee::getEsp32S3Profile(true);
    const FastBee::BoardProfile& s3 = FastBee::getEsp32S3Profile(false);
    if (profileId == s3Octal.key || profileId == "2") return &s3Octal;
    if (profileId == s3.key) return &s3;
    const auto& classic = FastBee::getEsp32ModuleProfile(false);
    const auto& classicPsram = FastBee::getEsp32ModuleProfile(true);
    if (profileId == classic.key || profileId == "4") return &classic;
    if (profileId == classicPsram.key || profileId == "5") return &classicPsram;
    return nullptr;
}


const char* pinRoleName(FastBee::PinRole role) {
    switch (role) {
        case FastBee::PinRole::GENERIC: return "generic";
        case FastBee::PinRole::INPUT_SIGNAL: return "input";
        case FastBee::PinRole::OUTPUT_SIGNAL: return "output";
        case FastBee::PinRole::I2C_SDA: return "i2c_sda";
        case FastBee::PinRole::I2C_SCL: return "i2c_scl";
        case FastBee::PinRole::SPI_MISO: return "spi_miso";
        case FastBee::PinRole::SPI_MOSI: return "spi_mosi";
        case FastBee::PinRole::SPI_SCK: return "spi_sck";
        case FastBee::PinRole::SPI_CS: return "spi_cs";
        case FastBee::PinRole::UART_RX: return "uart_rx";
        case FastBee::PinRole::UART_TX: return "uart_tx";
        case FastBee::PinRole::ADC_INPUT: return "adc_input";
        case FastBee::PinRole::PWM_OUTPUT: return "pwm_output";
        case FastBee::PinRole::RMT_SIGNAL: return "rmt_signal";
        case FastBee::PinRole::ENCODER_A: return "encoder_a";
        case FastBee::PinRole::ENCODER_B: return "encoder_b";
        case FastBee::PinRole::SDMMC_SIGNAL: return "sdmmc_signal";
        default: return "unknown";
    }
}

const char* claimStateName(FastBee::ClaimState state) {
    return state == FastBee::ClaimState::ACTIVE ? "active" : "dormant";
}

const char* shareModeName(FastBee::ResourceShareMode mode) {
    return mode == FastBee::ResourceShareMode::SHARED_IF_COMPATIBLE
        ? "shared_if_compatible" : "exclusive";
}


FastBee::PreflightContext makePreflightContext(
        const FastBee::BoardProfile& profile, uint32_t acknowledgements,
        void* networkManager) {
    FastBee::PreflightContext context;
    context.wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
    context.uart0ConsoleActive = true;
    context.usbSerialJtagActive =
        profile.id == FastBee::BoardProfileId::ESP32_C6_DEVKITC_1 ||
        profile.id == FastBee::BoardProfileId::ESP32_C3_GENERIC;
    context.jtagActive = false;
    context.acknowledgements = acknowledgements & PREFLIGHT_ACK_MASK;
    if (networkManager) {
        context.ethernet = FastBee::ethernetResourceConfig(
            static_cast<FBNetworkManager*>(networkManager)->getConfig());
        context.cellular = FastBee::cellularResourceConfig(
            static_cast<FBNetworkManager*>(networkManager)->getConfig());
    }
    return context;
}

bool sameResourceIdentity(const FastBee::ResourceClaim& left,
                          const FastBee::ResourceClaim& right) {
    if (left.kind != right.kind) return false;
    switch (left.kind) {
        case FastBee::ResourceKind::GPIO:
            return left.index == right.index;
        case FastBee::ResourceKind::I2C_ADDRESS:
        case FastBee::ResourceKind::SPI_CS:
        case FastBee::ResourceKind::ADC_CHANNEL:
            return left.unit == right.unit && left.index == right.index;
        default:
            return left.unit == right.unit;
    }
}

size_t countUniqueClaims(const FastBee::PreflightResult& result,
                         FastBee::ResourceKind kind, bool activeOnly) {
    size_t count = 0;
    for (size_t i = 0; i < result.claimCount; ++i) {
        const FastBee::ResourceClaim& claim = result.claims[i];
        if (claim.kind != kind ||
            (activeOnly && claim.state != FastBee::ClaimState::ACTIVE)) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            const FastBee::ResourceClaim& earlier = result.claims[j];
            if (earlier.kind != kind ||
                (activeOnly && earlier.state != FastBee::ClaimState::ACTIVE)) {
                continue;
            }
            if (sameResourceIdentity(earlier, claim)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) ++count;
    }
    return count;
}

size_t countProfilePins(const FastBee::BoardProfile& profile,
                        uint16_t capability) {
    size_t count = 0;
    for (size_t i = 0; i < profile.pinCount; ++i) {
        const FastBee::BoardPinProfile& pin = profile.pins[i];
        if (pin.policy != FastBee::PinPolicy::HARD_FORBIDDEN &&
            (pin.capabilities & capability) == capability) {
            ++count;
        }
    }
    return count;
}

size_t resourceCapacity(const FastBee::BoardProfile& profile,
                        FastBee::ResourceKind kind) {
    switch (kind) {
        case FastBee::ResourceKind::GPIO:
            return countProfilePins(profile, FastBee::PIN_CAP_INPUT);
        case FastBee::ResourceKind::I2C_BUS:
            return profile.resources.i2cBusCount;
        case FastBee::ResourceKind::I2C_ADDRESS:
            return 127;
        case FastBee::ResourceKind::SPI_BUS:
            return profile.resources.spiBusCount;
        case FastBee::ResourceKind::SPI_CS:
            return countProfilePins(profile, FastBee::PIN_CAP_OUTPUT);
        case FastBee::ResourceKind::UART_PORT:
            return profile.resources.genericUartCount;
        case FastBee::ResourceKind::LEDC_CHANNEL:
            return profile.resources.ledcChannelCount;
        case FastBee::ResourceKind::LEDC_TIMER:
            return profile.resources.ledcTimerCount;
        case FastBee::ResourceKind::RMT_CHANNEL:
            return profile.resources.rmtChannelCount;
        case FastBee::ResourceKind::ADC_CHANNEL:
            return countProfilePins(profile, FastBee::PIN_CAP_ADC);
        case FastBee::ResourceKind::ENCODER_SLOT:
            return profile.resources.encoderSlotCount;
        case FastBee::ResourceKind::SDMMC_HOST:
            return profile.resources.sdmmcSupported ? 1 : 0;
        default:
            return 0;
    }
}

const char* resourceLabel(FastBee::ResourceKind kind) {
    switch (kind) {
        case FastBee::ResourceKind::GPIO: return "GPIO";
        case FastBee::ResourceKind::I2C_BUS: return "I2C buses";
        case FastBee::ResourceKind::I2C_ADDRESS: return "I2C addresses";
        case FastBee::ResourceKind::SPI_BUS: return "SPI buses";
        case FastBee::ResourceKind::SPI_CS: return "SPI chip selects";
        case FastBee::ResourceKind::UART_PORT: return "UART ports";
        case FastBee::ResourceKind::LEDC_CHANNEL: return "LEDC channels";
        case FastBee::ResourceKind::LEDC_TIMER: return "LEDC timers";
        case FastBee::ResourceKind::RMT_CHANNEL: return "RMT channels";
        case FastBee::ResourceKind::ADC_CHANNEL: return "ADC channels";
        case FastBee::ResourceKind::ENCODER_SLOT: return "Encoder slots";
        case FastBee::ResourceKind::SDMMC_HOST: return "SDMMC hosts";
        default: return "Unknown";
    }
}

void addResourceRows(JsonArray rows, const FastBee::BoardProfile& profile,
                     const FastBee::PreflightResult& result) {
    static constexpr FastBee::ResourceKind KINDS[] = {
        FastBee::ResourceKind::GPIO,
        FastBee::ResourceKind::I2C_BUS,
        FastBee::ResourceKind::I2C_ADDRESS,
        FastBee::ResourceKind::SPI_BUS,
        FastBee::ResourceKind::SPI_CS,
        FastBee::ResourceKind::UART_PORT,
        FastBee::ResourceKind::LEDC_CHANNEL,
        FastBee::ResourceKind::LEDC_TIMER,
        FastBee::ResourceKind::RMT_CHANNEL,
        FastBee::ResourceKind::ADC_CHANNEL,
        FastBee::ResourceKind::ENCODER_SLOT,
        FastBee::ResourceKind::SDMMC_HOST
    };
    for (FastBee::ResourceKind kind : KINDS) {
        const size_t used = countUniqueClaims(result, kind, true);
        const size_t configured = countUniqueClaims(result, kind, false);
        const size_t capacity = resourceCapacity(profile, kind);
        JsonObject row = rows.add<JsonObject>();
        FastBee::setFixedJsonField(row, "id", FastBee::resourceKindName(kind));
        FastBee::setFixedJsonField(row, "label", resourceLabel(kind));
        FastBee::setFixedJsonField(row, "used", used);
        FastBee::setFixedJsonField(row, "configured", configured);
        FastBee::setFixedJsonField(row, "capacity", capacity);
        FastBee::setFixedJsonField(row, "available", used < capacity ? capacity - used : 0);
        FastBee::setFixedJsonField(row, "status", capacity == 0 ? "unavailable"
            : (used > capacity ? "overcommitted"
                : (used == capacity ? "full" : "available")));
    }
}

uint32_t requiredAcknowledgements(const FastBee::PreflightResult& result) {
    uint32_t required = FastBee::ACK_NONE;
    for (size_t i = 0; i < result.issueCount; ++i) {
        const FastBee::PreflightIssue& issue = result.issues[i];
        if (issue.severity !=
            FastBee::PreflightSeverity::WARNING_REQUIRES_CONFIRMATION) {
            continue;
        }
        switch (issue.code) {
            case FastBee::PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED:
                required |= FastBee::ACK_STRAPPING_PIN;
                break;
            case FastBee::PreflightIssueCode::PIN_BOARD_RESOURCE_ACK_REQUIRED:
                required |= FastBee::ACK_BOARD_SHARED_PIN;
                break;
            case FastBee::PreflightIssueCode::PIN_UART0_ACTIVE:
                required |= FastBee::ACK_UART0_CONSOLE;
                break;
            case FastBee::PreflightIssueCode::ADC2_WIFI_ACK_REQUIRED:
                required |= FastBee::ACK_ADC2_WITH_WIFI;
                break;
            default:
                break;
        }
    }
    return required;
}

bool hasHardPreflightError(const FastBee::PreflightResult& result) {
    if (result.claimsTruncated || result.issuesTruncated) return true;
    for (size_t i = 0; i < result.issueCount; ++i) {
        if (result.issues[i].severity == FastBee::PreflightSeverity::ERROR) {
            return true;
        }
    }
    return false;
}

void addPreflightIssues(JsonArray issues,
                        const FastBee::PreflightResult& result) {
    for (size_t i = 0; i < result.issueCount; ++i) {
        const FastBee::PreflightIssue& issue = result.issues[i];
        JsonObject item = issues.add<JsonObject>();
        FastBee::setFixedJsonField(item, "code", FastBee::preflightIssueCodeName(issue.code));
        FastBee::setFixedJsonField(item, "severity", FastBee::preflightSeverityName(issue.severity));
        FastBee::setFixedJsonField(item, "requiresConfirmation", issue.severity ==
            FastBee::PreflightSeverity::WARNING_REQUIRES_CONFIRMATION);
        FastBee::setFixedJsonField(item, "resourceKind", FastBee::resourceKindName(issue.resourceKind));
        FastBee::setFixedJsonField(item, "unit", issue.unit);
        FastBee::setFixedJsonField(item, "index", issue.index);
        if (issue.ownerId) FastBee::setFixedJsonField(item, "ownerId", issue.ownerId);
        if (issue.conflictingOwnerId) {
            FastBee::setFixedJsonField(item, "conflictingOwnerId", issue.conflictingOwnerId);
        }
        FastBee::setFixedJsonField(item, "message", issue.message ? issue.message : "");
    }
}

void addCandidateClaims(JsonArray claims,
                         const FastBee::PreflightResult& result,
                         const char* candidateId) {
    for (size_t i = 0; i < result.claimCount; ++i) {
        const FastBee::ResourceClaim& claim = result.claims[i];
        if (!sameText(claim.ownerId, candidateId)) continue;
        JsonObject item = claims.add<JsonObject>();
        FastBee::setFixedJsonField(item, "resourceKind", FastBee::resourceKindName(claim.kind));
        FastBee::setFixedJsonField(item, "state", claimStateName(claim.state));
        FastBee::setFixedJsonField(item, "shareMode", shareModeName(claim.shareMode));
        FastBee::setFixedJsonField(item, "role", pinRoleName(claim.pinRole));
        FastBee::setFixedJsonField(item, "unit", claim.unit);
        FastBee::setFixedJsonField(item, "index", claim.index);
        FastBee::setFixedJsonField(item, "ownerId", claim.ownerId ? claim.ownerId : "");
        FastBee::setFixedJsonField(item, "parameterA", claim.parameterA);
        FastBee::setFixedJsonField(item, "parameterB", claim.parameterB);
        JsonArray pins = item["pins"].to<JsonArray>();
        for (uint8_t pinIndex = 0; pinIndex < claim.pinCount; ++pinIndex) {
            if (claim.pins[pinIndex] != FastBee::BOARD_PIN_NONE) {
                pins.add(claim.pins[pinIndex]);
            }
        }
    }
}

const char* resolvedPinRole(const PeripheralConfig& config, uint8_t index) {
    switch (config.type) {
        case PeripheralType::UART: return index == 0 ? "RX" : "TX";
        case PeripheralType::I2C:
        case PeripheralType::LCD1602:
            return index == 0 ? "SDA" : "SCL";
        case PeripheralType::SPI:
            if (index == 0) return "MISO";
            if (index == 1) return "MOSI";
            if (index == 2) return "SCK";
            return "CS";
        case PeripheralType::SDIO:
            if (config.params.sdcard.interface == 1) {
                if (index == 0) return "SCK";
                if (index == 1) return "MOSI";
                if (index == 2) return "MISO";
                return "CS";
            }
            return "SDMMC";
        case PeripheralType::SENSOR_GENERIC:
            if (config.params.sensor.driver[0] != '\0') {
                return index == 0 ? "SDA" : "SCL";
            }
            if (config.params.sensor.sensorType == 3) {
                return index == 0 ? "TRIG" : "ECHO";
            }
            return "DATA";
        case PeripheralType::PWM_SERVO: return "PWM";
        case PeripheralType::STEPPER_MOTOR:
            if (index == 0) return "IN1";
            if (index == 1) return "IN2";
            if (index == 2) return "IN3";
            return "IN4";
        case PeripheralType::ENCODER: return index == 0 ? "A" : "B";
        case PeripheralType::SEVEN_SEGMENT_TM1637:
            return index == 0 ? "CLK" : "DIO";
        case PeripheralType::DS1302_RTC:
            if (index == 0) return "CE";
            if (index == 1) return "IO";
            return "SCLK";
        case PeripheralType::RFID_MFRC522:
            if (index == 0) return "SCK";
            if (index == 1) return "MOSI";
            if (index == 2) return "MISO";
            if (index == 3) return "SS";
            return "RST";
        default:
            return "GPIO";
    }
}

void addResolvedPins(JsonObject data, const PeripheralConfig& candidate) {
    JsonArray pins = data["resolvedPins"].to<JsonArray>();
    JsonArray roles = data["pinRoles"].to<JsonArray>();
    const uint8_t count = candidate.pinCount > 8 ? 8 : candidate.pinCount;
    for (uint8_t index = 0; index < count; ++index) {
        if (candidate.pins[index] == FastBee::BOARD_PIN_NONE) continue;
        pins.add(candidate.pins[index]);
        roles.add(resolvedPinRole(candidate, index));
    }
}

void sendJsonWithStatus(AsyncWebServerRequest* request, int status,
                         JsonDocument& doc) {
    const size_t responseSize = measureJson(doc);
    if (HandlerUtils::checkResponseMemory(request, responseSize)) return;
    String payload;
    payload.reserve(responseSize + 1);
    serializeJson(doc, payload);
    HandlerUtils::sendWithClose(
        request, request->beginResponse(status, "application/json", payload));
}

const char* preflightOperationName(FastBee::PreflightOperation operation) {
    switch (operation) {
        case FastBee::PreflightOperation::ADD: return "add";
        case FastBee::PreflightOperation::UPDATE: return "update";
        case FastBee::PreflightOperation::ENABLE: return "enable";
        default: return "unsupported";
    }
}

bool parsePreflightOperation(const String& value,
                             FastBee::PreflightOperation& operation) {
    if (FastBee::stringEqualsIgnoreCaseLiteral(value, "add")) {
        operation = FastBee::PreflightOperation::ADD;
        return true;
    }
    if (FastBee::stringEqualsIgnoreCaseLiteral(value, "update")) {
        operation = FastBee::PreflightOperation::UPDATE;
        return true;
    }
    if (FastBee::stringEqualsIgnoreCaseLiteral(value, "enable")) {
        operation = FastBee::PreflightOperation::ENABLE;
        return true;
    }
    return false;
}

void resetPeripheralTypeDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    if (isAnalogInputConfigType(config.type)) {
        initializeAdcDefaults(config);
    } else if (config.type == PeripheralType::UART) {
        initializeUartDefaults(config);
    } else if (config.type == PeripheralType::ENCODER) {
        initializeEncoderDefaults(config);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        initializeServoDefaults(config);
    } else if (config.type == PeripheralType::SDIO) {
        initializeSdCardDefaults(config);
    } else if (config.type == PeripheralType::DS1302_RTC) {
        initializeDs1302Defaults(config);
    } else if (config.type == PeripheralType::LCD1602) {
        initializeLcd1602Defaults(config);
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        initializeRfidDefaults(config);
    } else if (config.type == PeripheralType::SENSOR_GENERIC) {
        initializeSensorDefaults(config);
    } else if (config.type == PeripheralType::SCREEN_DISPLAY) {
        initializeDisplayDefaults(config);
    }
}

bool buildPreflightCandidate(WebHandlerContext* ctx,
                             AsyncWebServerRequest* request,
                             PeripheralManager& manager,
                             FastBee::PreflightOperation operation,
                             PeripheralConfig& candidate,
                             String& excludeId, String& errorMsg,
                             int& errorStatus) {
    bool resetDefaults = false;
    if (operation == FastBee::PreflightOperation::ADD) {
        candidate.id = ctx->getParamValue(request, "id", "");
        candidate.name = ctx->getParamValue(request, "name", "");
        candidate.type = static_cast<PeripheralType>(
            ctx->getParamInt(request, "type", 0));
        candidate.enabled = ctx->getParamBool(request, "enabled", false);
        resetDefaults = true;
    } else {
        excludeId = ctx->getParamValue(request, "id", "");
        if (excludeId.isEmpty()) {
            errorMsg = "Missing id parameter";
            errorStatus = 400;
            return false;
        }
        if (!manager.getPeripheralSnapshot(excludeId, candidate)) {
            errorMsg = "Peripheral not found";
            errorStatus = 404;
            return false;
        }
        if (candidate.type == PeripheralType::MODBUS_DEVICE) {
            errorMsg = userConfigurablePeripheralTypeError(candidate.type);
            errorStatus = 422;
            return false;
        }
        if (operation == FastBee::PreflightOperation::ENABLE) {
            candidate.enabled = true;
        } else {
            const String name = ctx->getParamValue(request, "name", "");
            if (!name.isEmpty()) candidate.name = name;
            if (request->hasParam("type", true)) {
                const PeripheralType newType = static_cast<PeripheralType>(
                    ctx->getParamInt(request, "type",
                                     static_cast<int>(candidate.type)));
                if (newType != candidate.type) {
                    candidate.type = newType;
                    resetPeripheralTypeDefaults(candidate);
                    resetDefaults = true;
                }
            }
            candidate.enabled = ctx->getParamBool(
                request, "enabled", candidate.enabled);
        }
    }

    const char* typeError = userConfigurablePeripheralTypeError(candidate.type);
    if (typeError) {
        errorMsg = typeError;
        errorStatus = 422;
        return false;
    }

    if (operation != FastBee::PreflightOperation::ENABLE) {
        const String pins = ctx->getParamValue(request, "pins", "");
        const bool shouldParse = operation == FastBee::PreflightOperation::ADD ||
            request->hasParam("pins", true);
        if (shouldParse &&
            !parsePeripheralPins(pins, candidate.pins, candidate.pinCount,
                                 errorMsg)) {
            errorStatus = 400;
            return false;
        }
        if (!applyPeripheralRequestParameters(ctx, request, candidate,
                                               resetDefaults, errorMsg)) {
            errorStatus = 400;
            return false;
        }
        if (candidate.type == PeripheralType::PWM_SERVO &&
            operation == FastBee::PreflightOperation::ADD) {
            candidate.enabled = false;
        }
    }

    if (candidate.id.isEmpty()) candidate.id = "__preflight_candidate";
    return true;
}

void addPreflightResponse(JsonDocument& doc,
                          const FastBee::BoardProfile& profile,
                          const FastBee::PreflightResult& result,
                          const PeripheralConfig& candidate,
                          FastBee::PreflightOperation operation,
                          bool success, bool allowed,
                          const char* message = nullptr) {
    doc["success"] = success;
    doc["allowed"] = allowed;
    doc["requiresConfirmation"] = result.requiresConfirmation;
    doc["requiredAcknowledgements"] = requiredAcknowledgements(result);
    if (message) doc["message"] = message;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "operation", preflightOperationName(operation));
    FastBee::setFixedJsonField(data, "profileId", profile.key);
    FastBee::setFixedJsonField(data, "allowed", allowed);
    FastBee::setFixedJsonField(data, "requiresConfirmation", result.requiresConfirmation);
    FastBee::setFixedJsonField(data, "requiredAcknowledgements", requiredAcknowledgements(result));
    FastBee::setFixedJsonField(data, "claimsTruncated", result.claimsTruncated);
    FastBee::setFixedJsonField(data, "issuesTruncated", result.issuesTruncated);
    addPreflightIssues(data["issues"].to<JsonArray>(), result);
    addCandidateClaims(data["claims"].to<JsonArray>(), result,
                        candidate.id.c_str());
    addResolvedPins(data, candidate);
    addResourceRows(data["resources"].to<JsonArray>(), profile, result);
}

bool sendBlockedPreflight(AsyncWebServerRequest* request,
                          const FastBee::BoardProfile& profile,
                          const FastBee::PreflightResult& result,
                          const PeripheralConfig& candidate,
                          FastBee::PreflightOperation operation) {
    if (result.allowed && !result.claimsTruncated && !result.issuesTruncated) {
        return false;
    }
    JsonDocument doc;
    doc["code"] = "PERIPHERAL_PREFLIGHT_BLOCKED";
    addPreflightResponse(doc, profile, result, candidate, operation, false,
                         false,
                         "Peripheral resource preflight blocked the change");
    sendJsonWithStatus(request, 409, doc);
    return true;
}
}

PeripheralRouteHandler::PeripheralRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    preflightMutex = xSemaphoreCreateMutex();
}

PeripheralRouteHandler::~PeripheralRouteHandler() {
    if (preflightMutex) {
        vSemaphoreDelete(preflightMutex);
        preflightMutex = nullptr;
    }
}

void PeripheralRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 注意：更具体的路由必须放在通用路由之前

    server->on("/api/boards/profiles", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
                  handleGetBoardProfiles(request);
              });

    server->on("/api/peripherals/resources", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
                  handleGetPeripheralResources(request);
              });

    server->on("/api/peripherals/preflight", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
                  handlePreflightPeripheral(request);
              });

    server->on("/api/peripherals/config/preflight", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleImportConfiguration(request, false); });
    server->on("/api/peripherals/config/import", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleImportConfiguration(request, true); });

    server->on("/api/peripherals/types", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheralTypes(request); });

    server->on("/api/peripherals/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnablePeripheral(request); });

    server->on("/api/peripherals/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisablePeripheral(request); });

    server->on("/api/peripherals/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheralStatus(request); });

    server->on("/api/peripherals/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleReadPeripheral(request); });

    server->on("/api/peripherals/validate-pins", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleValidatePins(request); });

    server->on("/api/peripherals/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleWritePeripheral(request); });

    server->on("/api/peripherals/", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheral(request); });

    server->on("/api/peripherals/", HTTP_PUT,
              [this](AsyncWebServerRequest* request) { handleUpdatePeripheral(request); });

    server->on("/api/peripherals/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdatePeripheral(request); });

    server->on("/api/peripherals/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeletePeripheral(request); });

    server->on("/api/peripherals", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripherals(request); });

    server->on("/api/peripherals", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddPeripheral(request); });
}


void PeripheralRouteHandler::handleGetPeripheralResources(
        AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (HandlerUtils::checkLowMemory(request, 24576)) return;

    const String requestedProfile =
        ctx->getParamValue(request, "profileId", "");
    const FastBee::BoardProfile* profile =
        resolveBoardProfile(requestedProfile);
    if (!profile) {
        ctx->sendError(request, 422,
                       "UNSUPPORTED_BOARD_PROFILE: unknown or unavailable profile");
        return;
    }

    PeripheralManager& manager = PeripheralManager::getInstance();
    const FastBee::BoardProfile* active = compiledBoardProfileSupported()
        ? &FastBee::getCompiledBoardProfile() : nullptr;
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "profileId", profile->key);
    FastBee::setFixedJsonField(data, "profileName", profile->displayName);
    FastBee::setFixedJsonField(data, "profileVersion", profile->version);
    FastBee::setFixedJsonField(data, "activeProfileId", active ? active->key : "");
    FastBee::setFixedJsonField(data, "comparison", active == nullptr || !sameText(profile->key, active->key));
    FastBee::setFixedJsonField(data, "claimScope", "configuration");
    FastBee::setFixedJsonField(data, "networkClaims", "w5500,cellular");
    // The legacy resource rows remain configuration-only. Runtime leases are
    // separate and always refer to the active firmware, even for comparisons.
    FastBee::setFixedJsonField(data, "runtimeReservationsIncluded", false);
    FastBee::setFixedJsonField(data, "configuredUseMeaning", "enabled_configuration");
    FastBee::setFixedJsonField(data, "availableMeaning", "configuration_only_not_runtime_free");
    FastBee::setFixedJsonField(data, "configurationStatus", "busy");
    FastBee::setFixedJsonField(data, "resources", nullptr);
    FastBee::setFixedJsonField(data, "diagnostics", nullptr);
    {
        MutexGuard resultLock(preflightMutex, 0);
        if (resultLock.isLocked()) {
            RecursiveMutexGuard managerLock(manager.getMutex(), 0);
            if (managerLock.isLocked()) {
                std::unique_ptr<FastBee::PreflightResult> workspace;
                if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
                FastBee::PreflightResult& preflightResult = *workspace;
                const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
                FastBee::PeripheralResourcePlanner planner(
                    *profile, makePreflightContext(*profile, FastBee::ACK_NONE, ctx->networkManager),
                    preflightResult, existing.data(), existing.size());
                for (const PeripheralConfig& config : existing) {
                    planner.addPeripheral(config, config.enabled ? FastBee::ClaimState::ACTIVE
                                                                 : FastBee::ClaimState::DORMANT);
                }
                FastBee::setFixedJsonField(data, "configurationStatus", "observed");
                FastBee::setFixedJsonField(data, "claimsTruncated", preflightResult.claimsTruncated);
                FastBee::setFixedJsonField(data, "issuesTruncated", preflightResult.issuesTruncated);
                addResourceRows(data["resources"].to<JsonArray>(), *profile, preflightResult);
                addPreflightIssues(data["diagnostics"].to<JsonArray>(), preflightResult);
            }
        }
    }
    JsonObject runtime = data["runtime"].to<JsonObject>();
    FastBee::writeRuntimeResourceHeader(runtime, active ? active->key : "");
    // Each zero-wait registry observation ends before the next begins. No
    // planner/manager lock is held while observing another resource manager.
    {
        FastBee::I2CResourceSnapshot snapshot;
        FastBee::I2CBusManager::instance().snapshotResources(snapshot);
        FastBee::writeI2CResourceJson(runtime["i2c"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::SpiResourceSnapshot snapshot;
        FastBee::SpiBusInitializer::instance().snapshotResources(snapshot);
        FastBee::writeSpiResourceJson(runtime["spi"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::UartResourceSnapshot snapshot;
        FastBee::UartPortLease::snapshotResources(snapshot, CHIP_UART_COUNT);
        FastBee::writeUartResourceJson(runtime["uart"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::ControlOutputResourceSnapshot snapshot;
        manager.snapshotControlOutputResources(snapshot);
        FastBee::writeControlOutputResourceJson(runtime["controlOutput"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::EncoderResourceSnapshot snapshot;
        manager.snapshotEncoderResources(snapshot);
        FastBee::writeEncoderResourceJson(runtime["encoder"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::LedcResourceSnapshot snapshot;
        manager.snapshotLedcResources(snapshot);
        FastBee::writeLedcResourceJson(runtime["ledc"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::BacklightResourceSnapshot snapshot;
#if FASTBEE_ENABLE_SCREEN_UI
        FastBee::Screen::ScreenRuntime::snapshotBacklightResources(snapshot);
#endif
        FastBee::writeBacklightResourceJson(runtime["backlightLedc"].to<JsonObject>(), snapshot, millis());
    }
    {
        FastBee::DhtRmtResourceSnapshot snapshot;
#if FASTBEE_ENABLE_SENSOR_DRIVER
        SensorDriver::snapshotDhtRmtResources(snapshot);
#endif
        FastBee::writeDhtRmtResourceJson(runtime["dhtRmt"].to<JsonObject>(), snapshot);
    }
    FastBee::writeManagedPinResources(runtime, manager);
    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleImportConfiguration(AsyncWebServerRequest* request, bool commit) {
    if (!ctx->requireAuth(request) || !ctx->requireDeveloperMode(request)) return;
    if (!compiledBoardProfileSupported()) {
        ctx->sendError(request, 501, "CONFIG_IMPORT_BOARD_UNSUPPORTED"); return;
    }
    if (!request->hasParam("content", true)) {
        ctx->sendBadRequest(request, "CONFIG_IMPORT_CONTENT_REQUIRED"); return;
    }
    const String& content = request->getParam("content", true)->value();
    if (content.isEmpty() || content.length() > 24 * 1024 || memchr(content.c_str(), '\0', content.length())) {
        ctx->sendBadRequest(request, "CONFIG_IMPORT_SIZE_OR_CONTENT"); return;
    }
    if (HandlerUtils::checkLowMemory(request, content.length() * 2 + 24576)) return;
    uint32_t acknowledgements = 0;
    if (request->hasParam("acknowledgements", true)) {
        const String& value = request->getParam("acknowledgements", true)->value();
        if (value.isEmpty() || value.length() > 2) {
            ctx->sendBadRequest(request, "CONFIG_IMPORT_ACK_INVALID"); return;
        }
        for (size_t i = 0; i < value.length(); ++i) {
            if (value[i] < '0' || value[i] > '9') {
                ctx->sendBadRequest(request, "CONFIG_IMPORT_ACK_INVALID"); return;
            }
            acknowledgements = acknowledgements * 10 + static_cast<unsigned>(value[i] - '0');
        }
        if (acknowledgements & ~PREFLIGHT_ACK_MASK) {
            ctx->sendBadRequest(request, "CONFIG_IMPORT_ACK_INVALID"); return;
        }
    }
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(50));
    auto& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard managerLock(manager.getMutex(), pdMS_TO_TICKS(50));
    if (!resultLock.isLocked() || !managerLock.isLocked() || manager.configurationImportPending()) {
        ctx->sendError(request, 409, "CONFIG_IMPORT_BUSY_OR_PENDING_RESTART"); return;
    }
    const uint64_t version = manager.getConfigurationReviewVersion();
    char versionText[17];
    snprintf(versionText, sizeof(versionText), "%016llx", static_cast<unsigned long long>(version));
    if (!version || (commit && (!request->hasParam("expectedVersion", true) ||
        request->getParam("expectedVersion", true)->value() != versionText))) {
        ctx->sendError(request, 409, "CONFIG_IMPORT_REVISION_CONFLICT"); return;
    }
    FastBeeJsonDocLarge document;
    if (deserializeJson(document, content.c_str(), content.length())) {
        ctx->sendBadRequest(request, "CONFIG_IMPORT_JSON_INVALID"); return;
    }
    std::vector<PeripheralConfig> candidates;
    String error;
    const auto& profile = FastBee::getCompiledBoardProfile();
    std::unique_ptr<FastBee::PreflightResult> workspace;
    if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
    FastBee::PreflightResult& preflightResult = *workspace;
    const bool valid = manager.prepareConfigurationImport(document, candidates,
        makePreflightContext(profile, commit ? acknowledgements : 0, ctx->networkManager), preflightResult, error);
    if (!valid) {
        ctx->sendError(request, 422, error); return;
    }
    if (commit && (!preflightResult.allowed || preflightResult.requiresConfirmation)) {
        ctx->sendError(request, 409, "CONFIG_IMPORT_RESOURCE_CONFIRMATION_REQUIRED"); return;
    }
    if (commit && !manager.commitConfigurationImport(document, version, error)) {
        ctx->sendError(request, 503, error); return;
    }
    JsonDocument response;
    response["name"] = "peripherals.json";
    response["size"] = content.length();
    response["peripheralCount"] = document["peripherals"].size();
    response["version"] = versionText;
    response["profileId"] = profile.key;
    response["allowed"] = true;
    response["requiresConfirmation"] = preflightResult.requiresConfirmation;
    response["requiredAcknowledgements"] = requiredAcknowledgements(preflightResult);
    response["saved"] = commit;
    response["applied"] = false;
    response["restartRequired"] = commit;
    addPreflightIssues(response["issues"].to<JsonArray>(), preflightResult);
    ctx->sendSuccess(request, response);
}

void PeripheralRouteHandler::handlePreflightPeripheral(
        AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!compiledBoardProfileSupported()) {
        ctx->sendError(request, 501,
                       "UNSUPPORTED_BOARD_PROFILE: no mutation profile for this build");
        return;
    }

    FastBee::PreflightOperation operation;
    const String operationText =
        ctx->getParamValue(request, "operation", "add");
    if (!parsePreflightOperation(operationText, operation)) {
        ctx->sendError(request, 400,
                       "INVALID_PREFLIGHT_OPERATION: expected add, update or enable");
        return;
    }

    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    PeripheralManager& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard managerLock(manager.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }

    PeripheralConfig candidate;
    String excludeId;
    String errorMsg;
    int errorStatus = 400;
    if (!buildPreflightCandidate(ctx, request, manager, operation, candidate,
                                 excludeId, errorMsg, errorStatus)) {
        ctx->sendError(request, errorStatus, errorMsg);
        return;
    }

    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    const uint32_t acknowledgements =
        static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK;
    const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
    std::unique_ptr<FastBee::PreflightResult> workspace;
    if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
    FastBee::PreflightResult& preflightResult = *workspace;
    const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
    FastBee::preflightPeripheralChange(
        profile, existing.data(), existing.size(), candidate, operation,
        excludeId, makePreflightContext(profile, acknowledgements, ctx->networkManager),
        preflightResult);

    JsonDocument doc;
    const bool allowed = !hasHardPreflightError(preflightResult);
    addPreflightResponse(doc, profile, preflightResult, candidate, operation,
                         true, allowed);
    HandlerUtils::sendJsonStream(request, doc);
}

namespace {
#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addPeripheralListIdentity(JsonDocument& doc, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(doc, "id", config.id);
    FastBee::setFixedJsonField(doc, "name", config.name);
    FastBee::setFixedJsonField(doc, "type", static_cast<int>(config.type));
    FastBee::setFixedJsonField(doc, "typeName", getPeripheralTypeName(config.type));
    FastBee::setFixedJsonField(doc, "category", getCategoryName(getPeripheralCategory(config.type)));
    FastBee::setFixedJsonField(doc, "enabled", config.enabled);
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void addPeripheralListParams(JsonDocument& doc, const PeripheralConfig& config) {
    if (config.type == PeripheralType::UART) {
        JsonObject params = doc["params"].to<JsonObject>();
        addUartParams(params, config);
    } else if (config.type == PeripheralType::ENCODER) {
        JsonObject params = doc["params"].to<JsonObject>();
        addEncoderParams(params, config);
    } else if (isAnalogInputPeripheral(config.type)) {
        JsonObject params = doc["params"].to<JsonObject>();
        // 列表只带常用字段；详细接口再返回校准参数，控制响应体大小。
        addAnalogInputParams(params, config, false);
    } else if (isDigitalInputPeripheral(config.type)) {
        JsonObject params = doc["params"].to<JsonObject>();
        addDigitalInputParams(params, config);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        JsonObject params = doc["params"].to<JsonObject>();
        addServoParams(params, config);
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        JsonObject params = doc["params"].to<JsonObject>();
        addP1PeripheralParams(params, config);
    }
}
} // namespace

void PeripheralRouteHandler::handleGetPeripherals(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    PeripheralManager& pm = PeripheralManager::getInstance();

    // 如果传了 id 参数，只返回单条外设（避免编辑时加载全部导致内存不足）
    if (request->hasParam("id")) {
        String periphId = request->getParam("id")->value();
        PeripheralConfig config;
        if (!pm.getPeripheralSnapshot(periphId, config)) {
            request->send(404, "application/json", "{\"success\":false,\"message\":\"Peripheral not found\"}");
            return;
        }

        JsonDocument doc;
        addPeripheralListIdentity(doc, config);
        addPeripheralDiagnosticCapability(doc.as<JsonObject>(), config);
        addPassiveBuzzerFlag(doc, config);

        addPeripheralListParams(doc, config);
        addGenericPeripheralFields(doc.as<JsonObject>(), config);

        JsonArray pins = doc["pins"].to<JsonArray>();
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255) {
                pins.add(config.pins[i]);
            }
        }

        PeripheralRuntimeState runtimeState;
        if (pm.getRuntimeStateSnapshot(periphId, runtimeState)) {
            doc["status"] = static_cast<int>(runtimeState.status);
            doc["memorySuspended"] = runtimeState.memorySuspended;
            if (config.type == PeripheralType::UART) {
                addUartRuntime(doc.as<JsonObject>(), runtimeState, config.enabled);
            }
            if (config.type == PeripheralType::ENCODER) {
                EncoderReading reading;
                if (pm.getEncoderReading(periphId, reading)) {
                    addEncoderReading(doc.as<JsonObject>(), reading);
                }
            }
        } else {
            doc["status"] = 0;
            doc["memorySuspended"] = false;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", "{\"success\":true,\"data\":" + response + "}");
        return;
    }

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Peripheral list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }

    String typeFilter = ctx->getParamValue(request, "type", "");
    String categoryFilter = ctx->getParamValue(request, "category", "");
    bool enabledOnly = ctx->getParamValue(request, "enabledOnly", "0") == "1";
    bool requestedCompact = ctx->getParamValue(request, "compact", "0") == "1";
    bool degraded = shouldForceCompactList();
    bool compact = requestedCompact || degraded;

    // 使用指针向量代替深拷贝，大幅减少内存占用（38个外设：指针~152B vs 拷贝~6KB）
    std::vector<const PeripheralConfig*> ptrs;
    ptrs.reserve(40);  // 预估外设数量

    if (!typeFilter.isEmpty()) {
        PeripheralType type = parsePeripheralType(typeFilter.c_str());
        pm.forEachPeripheral([&ptrs, type, enabledOnly](const PeripheralConfig& p) {
            if (p.type == type &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else if (!categoryFilter.isEmpty()) {
        PeripheralCategory category = PeripheralCategory::CATEGORY_GPIO;
        if (categoryFilter == "communication") category = PeripheralCategory::CATEGORY_COMMUNICATION;
        else if (categoryFilter == "gpio") category = PeripheralCategory::CATEGORY_GPIO;
        else if (categoryFilter == "analog") category = PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
        else if (categoryFilter == "debug") category = PeripheralCategory::CATEGORY_DEBUG;
        else if (categoryFilter == "special") category = PeripheralCategory::CATEGORY_SPECIAL;
        pm.forEachPeripheral([&ptrs, category, enabledOnly](const PeripheralConfig& p) {
            if (getPeripheralCategory(p.type) == category &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else {
        pm.forEachPeripheral([&ptrs, enabledOnly](const PeripheralConfig& p) {
            if (p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    }

    // 排序：启用的排前面，然后按名称排序
    std::sort(ptrs.begin(), ptrs.end(), [](const PeripheralConfig* a, const PeripheralConfig* b) {
        if (a->enabled != b->enabled) return a->enabled > b->enabled;
        return strcmp(a->name.c_str(), b->name.c_str()) < 0;
    });

    int total = ptrs.size();

    // 解析分页参数
    int page = 1;
    int pageSize = 10;
    if (request->hasParam("page")) {
        page = request->getParam("page")->value().toInt();
        if (page < 1) page = 1;
    }
    if (request->hasParam("pageSize")) {
        pageSize = request->getParam("pageSize")->value().toInt();
        if (pageSize < 1) pageSize = 1;
        if (pageSize > (compact ? 50 : 20)) pageSize = compact ? 50 : 20;
    }

    // 分页步长始终与前端请求保持一致，避免数据间隙（items被跳过无法显示）。
    // 内存不足时由 compact 模式减少每项序列化字段来节省空间，而非减少条数。
    // 如果连单条序列化都无法完成，由下方 isListMemoryCriticallyLow() 返回 503。
    int effectivePageSize = pageSize;

    // Deletion/filter changes can remove the requested page. Clamp before
    // multiplying to also avoid overflow from an excessively large page.
    page = std::min(page, std::max(1, (total + effectivePageSize - 1) / effectivePageSize));

    // 计算分页范围（统一使用 effectivePageSize）
    int startIdx = (page - 1) * effectivePageSize;
    int endIdx = (startIdx < total) ? std::min(startIdx + effectivePageSize, total) : startIdx;

    // Chunked responses can operate with small contiguous blocks. Only reject
    // when even per-item scratch strings are likely to fail.
    if (isListMemoryCriticallyLow()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Low memory: heap=%lu maxAlloc=%lu",
                 (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
        HandlerUtils::sendJsonError(request, 503, msg, 5);
        return;
    }
    
    // 两遍法：先序列化每个项计算精确大小，再用精确 reserve 构建响应
    int itemCount = endIdx - startIdx;
    
    // 第一遍：序列化所有项到临时 String，累计精确大小
    std::vector<String> serializedItems;
    serializedItems.reserve(itemCount);
    size_t totalItemsSize = 0;
    
    for (int i = startIdx; i < endIdx; i++) {
        const auto& config = *ptrs[i];
        JsonDocument itemDoc;
        addPeripheralListIdentity(itemDoc, config);
        addPeripheralDiagnosticCapability(itemDoc.as<JsonObject>(), config);
        addPassiveBuzzerFlag(itemDoc, config);

        if (!compact) {
            JsonArray pins = itemDoc["pins"].to<JsonArray>();
            for (int j = 0; j < config.pinCount && j < 8; j++) {
                if (config.pins[j] != 255) {
                    pins.add(config.pins[j]);
                }
            }

            addPeripheralListParams(itemDoc, config);

            PeripheralRuntimeState runtimeState;
            if (pm.getRuntimeStateSnapshot(config.id, runtimeState)) {
                itemDoc["status"] = static_cast<int>(runtimeState.status);
                itemDoc["memorySuspended"] = runtimeState.memorySuspended;
                if (config.type == PeripheralType::UART) {
                    addUartRuntime(itemDoc.as<JsonObject>(), runtimeState, config.enabled);
                }
                if (config.type == PeripheralType::ENCODER) {
                    JsonObject encoder = itemDoc["reading"].to<JsonObject>();
                    FastBee::setFixedJsonField(encoder, "count", runtimeState.state.encoder.count);
                    FastBee::setFixedJsonField(encoder, "delta", runtimeState.state.encoder.delta);
                    FastBee::setFixedJsonField(encoder, "direction", runtimeState.state.encoder.direction);
                    FastBee::setFixedJsonField(encoder, "frequencyHz", runtimeState.state.encoder.frequencyHz);
                    FastBee::setFixedJsonField(encoder, "rpm", runtimeState.state.encoder.rpm);
                }
            } else {
                itemDoc["status"] = 0;
                itemDoc["memorySuspended"] = false;
            }
        }

        addGenericPeripheralFields(itemDoc.as<JsonObject>(), config);
        String itemStr;
        serializeJson(itemDoc, itemStr);
        totalItemsSize += itemStr.length();
        serializedItems.push_back(std::move(itemStr));
    }
    
     // ============ Chunked 流式响应（复用 HandlerUtils::sendJsonListChunked）============
    // 旧方案：先构建一个 totalJsonSize 大小的连续 String（~10-20KB），对碎片化堆极不友好。
    // 新方案：items 已在 vector<String> 中，按 index 按需复制到 TCP 发送缓冲（~256B），
    //         堆峰 = items 合计（~6KB），不再产生额外大 String。
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {
        LOG_ERRORF("[Periph] Low heap for chunked response: free=%u", (unsigned)freeHeap);
        HandlerUtils::sendJsonError(request, 503, "Low memory for response");
        return;
    }

    // 构建 header：{"success":true,"total":N,"page":P,"pageSize":S,"data":[
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(pm.getPeripheralCount());
    const int profileRemaining = std::max(0, profileMax - profileUsed);
    const bool runtimeDegraded = HandlerUtils::memoryDegradationActive();

    String header;
    header.reserve(420);
    header = F("{\"success\":true,\"total\":");
    header += String(total);
    header += F(",\"page\":");
    header += String(page);
    header += F(",\"pageSize\":");
    header += String(effectivePageSize);
    if (degraded || runtimeDegraded) {
        header += F(",\"degraded\":true");
    }
    HandlerUtils::appendMemoryDegradationStatus(header);
    header += F(",\"profile\":{\"name\":\"");
    header += FastBee::ResourceProfile::NAME;
    header += F("\",\"max\":");
    header += String(profileMax);
    header += F(",\"used\":");
    header += String(profileUsed);
    header += F(",\"remaining\":");
    header += String(profileRemaining);
    header += F("}");
    header += F(",\"data\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(serializedItems))) {
        LOG_ERRORF("[Periph] beginChunkedResponse failed, heap=%u", (unsigned)ESP.getFreeHeap());
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}

void PeripheralRouteHandler::handleGetPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig configSnapshot;
    const PeripheralConfig* config = pm.getPeripheralSnapshot(id, configSnapshot) ? &configSnapshot : nullptr;

    if (!config) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    FastBee::setFixedJsonField(data, "id", config->id);
    FastBee::setFixedJsonField(data, "name", config->name);
    FastBee::setFixedJsonField(data, "type", static_cast<int>(config->type));
    FastBee::setFixedJsonField(data, "typeName", getPeripheralTypeName(config->type));
    FastBee::setFixedJsonField(data, "enabled", config->enabled);
    addPeripheralDiagnosticCapability(data, *config);
    addPassiveBuzzerFlag(data, *config);

    JsonArray pins = data["pins"].to<JsonArray>();
    for (int i = 0; i < config->pinCount && i < 8; i++) {
        if (config->pins[i] != 255) {
            pins.add(config->pins[i]);
        }
    }

    // 类型特定参数
    JsonObject params = data["params"].to<JsonObject>();
    if (config->type == PeripheralType::UART) {
        addUartParams(params, *config);
    } else if (config->type == PeripheralType::ENCODER) {
        addEncoderParams(params, *config);
    } else if (config->type == PeripheralType::I2C) {
        FastBee::setFixedJsonField(params, "frequency", config->params.i2c.frequency);
        FastBee::setFixedJsonField(params, "address", config->params.i2c.address);
        FastBee::setFixedJsonField(params, "isMaster", config->params.i2c.isMaster);
    } else if (config->type == PeripheralType::SPI) {
        FastBee::setFixedJsonField(params, "frequency", config->params.spi.frequency);
        FastBee::setFixedJsonField(params, "mode", config->params.spi.mode);
        FastBee::setFixedJsonField(params, "msbFirst", config->params.spi.msbFirst);
    } else if (config->type == PeripheralType::PWM_SERVO) {
        addServoParams(params, *config);
    } else if (config->isGPIOPeripheral() && !isAnalogInputConfigType(config->type)) {
        FastBee::setFixedJsonField(params, "initialState", static_cast<int>(config->params.gpio.initialState));
        FastBee::setFixedJsonField(params, "pwmChannel", config->params.gpio.pwmChannel);
        FastBee::setFixedJsonField(params, "pwmFrequency", config->params.gpio.pwmFrequency);
        FastBee::setFixedJsonField(params, "pwmResolution", config->params.gpio.pwmResolution);
        FastBee::setFixedJsonField(params, "defaultDuty", config->params.gpio.defaultDuty);
        if (isDigitalInputPeripheral(config->type)) {
            addDigitalInputParams(params, *config);
        }
    } else if (isAnalogInputConfigType(config->type)) {
        addAnalogInputParams(params, *config);
    } else if (config->type == PeripheralType::DAC) {
        FastBee::setFixedJsonField(params, "channel", config->params.dac.channel);
        FastBee::setFixedJsonField(params, "defaultValue", config->params.dac.defaultValue);
    } else if (config->type == PeripheralType::STEPPER_MOTOR) {
        FastBee::setFixedJsonField(params, "stepsPerRevolution", config->params.stepper.stepsPerRevolution);
        FastBee::setFixedJsonField(params, "speed", config->params.stepper.speed);
    } else if (config->type == PeripheralType::LCD) {
        FastBee::setFixedJsonField(params, "width", config->params.lcd.width);
        FastBee::setFixedJsonField(params, "height", config->params.lcd.height);
        FastBee::setFixedJsonField(params, "interface", config->params.lcd.interface);
    } else if (config->type == PeripheralType::NEO_PIXEL) {
        FastBee::setFixedJsonField(params, "count", config->params.neopixel.count);
        FastBee::setFixedJsonField(params, "brightness", config->params.neopixel.brightness);
    } else if (config->type == PeripheralType::RF_MODULE) {
        FastBee::setFixedJsonField(params, "mode", config->params.rf.mode);
        FastBee::setFixedJsonField(params, "pulseWidth", config->params.rf.pulseWidth);
        FastBee::setFixedJsonField(params, "repeat", config->params.rf.repeat);
        FastBee::setFixedJsonField(params, "bitLength", config->params.rf.bitLength);
        FastBee::setFixedJsonField(params, "activeHigh", config->params.rf.activeHigh);
    } else if (config->type == PeripheralType::RADAR_SENSOR) {
        FastBee::setFixedJsonField(params, "mode", config->params.radar.mode);
        FastBee::setFixedJsonField(params, "activeHigh", config->params.radar.activeHigh);
        FastBee::setFixedJsonField(params, "debounceMs", config->params.radar.debounceMs);
        FastBee::setFixedJsonField(params, "holdMs", config->params.radar.holdMs);
    } else if (config->type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        FastBee::setFixedJsonField(params, "brightness", config->params.segment.brightness);
    } else if (config->type == PeripheralType::SENSOR_GENERIC ||
               config->type == PeripheralType::SDIO ||
               config->type == PeripheralType::DS1302_RTC ||
               config->type == PeripheralType::LCD1602 ||
               config->type == PeripheralType::RFID_MFRC522) {
        addP1PeripheralParams(params, *config);
    }
    addGenericPeripheralFields(data, *config);

    PeripheralRuntimeState runtimeSnapshot;
    const PeripheralRuntimeState* runtimeState =
        pm.getRuntimeStateSnapshot(id, runtimeSnapshot) ? &runtimeSnapshot : nullptr;
    if (runtimeState) {
        JsonObject status = data["status"].to<JsonObject>();
        FastBee::setFixedJsonField(status, "state", static_cast<int>(runtimeState->status));
        FastBee::setFixedJsonField(status, "initTime", runtimeState->initTime);
        FastBee::setFixedJsonField(status, "lastActivity", runtimeState->lastActivity);
        FastBee::setFixedJsonField(status, "errorCount", runtimeState->errorCount);
        FastBee::setFixedJsonField(status, "memorySuspended", runtimeState->memorySuspended);
        if (config->type == PeripheralType::UART) {
            FastBee::setFixedJsonField(status, "framing", uartFramingName(*config));
            addUartRuntime(status, *runtimeState, config->enabled);
        } else if (config->type == PeripheralType::ENCODER) {
            EncoderReading reading;
            if (pm.getEncoderReading(id, reading)) addEncoderReading(status, reading);
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleAddPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    PeripheralConfig config;
    config.id = ctx->getParamValue(request, "id", "");
    config.name = ctx->getParamValue(request, "name", "");
    config.type = static_cast<PeripheralType>(ctx->getParamInt(request, "type", 0));
    config.enabled = ctx->getParamBool(request, "enabled", false);

    const char* typeError = userConfigurablePeripheralTypeError(config.type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }

    String pinsStr = ctx->getParamValue(request, "pins", "");
    String errorMsg;
    if (!parsePeripheralPins(pinsStr, config.pins, config.pinCount, errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    if (config.type == PeripheralType::UART) {
        initializeUartDefaults(config);
        config.params.uart.baudRate = ctx->getParamInt(request, "baudRate", UART_DEFAULT_BAUD_RATE);
        config.params.uart.dataBits = ctx->getParamInt(request, "dataBits", UART_DEFAULT_DATA_BITS);
        config.params.uart.stopBits = ctx->getParamInt(request, "stopBits", UART_DEFAULT_STOP_BITS);
        config.params.uart.parity = ctx->getParamInt(request, "parity", UART_DEFAULT_PARITY);
    } else if (config.type == PeripheralType::ENCODER) {
        initializeEncoderDefaults(config);
        const int resolution = ctx->getParamInt(
            request, "resolution", ENCODER_DEFAULT_RESOLUTION);
        if (resolution < 1 || resolution > UINT16_MAX) {
            ctx->sendError(request, 400,
                           "INVALID_ENCODER_RESOLUTION: expected 1-65535 pulses per revolution");
            return;
        }
        config.params.encoder.resolution = static_cast<uint16_t>(resolution);
        config.params.encoder.useInterrupt = ctx->getParamBool(request, "useInterrupt", true);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = ctx->getParamInt(request, "frequency", 100000);
        config.params.i2c.address = ctx->getParamInt(request, "address", 0);
        config.params.i2c.isMaster = ctx->getParamBool(request, "isMaster", true);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = ctx->getParamInt(request, "frequency", 1000000);
        config.params.spi.mode = ctx->getParamInt(request, "mode", 0);
        config.params.spi.msbFirst = ctx->getParamBool(request, "msbFirst", true);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (!applyServoParams(ctx, request, config, true, errorMsg)) {
            ctx->sendError(request, 400, errorMsg);
            return;
        }
        // Creation never energizes a servo. Test and enable are explicit steps.
        config.enabled = false;
    } else if (config.isGPIOPeripheral() && !isAnalogInputConfigType(config.type)) {
        config.params.gpio.initialState = static_cast<GPIOState>(ctx->getParamInt(request, "initialState", 0));
        config.params.gpio.pwmChannel = ctx->getParamInt(request, "pwmChannel", 0);
        config.params.gpio.pwmFrequency = ctx->getParamInt(request, "pwmFrequency", 1000);
        config.params.gpio.pwmResolution = ctx->getParamInt(request, "pwmResolution", 8);
        config.params.gpio.defaultDuty = ctx->getParamInt(request, "defaultDuty", 0);
        config.params.gpio.passive = ctx->getParamBool(request, "passive", false);
        if (isDigitalInputPeripheral(config.type)) {
            applyDigitalInputParams(ctx, request, config);
        }
    } else if (isAnalogInputConfigType(config.type)) {
        initializeAdcDefaults(config);
        applyAnalogInputParams(ctx, request, config);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = ctx->getParamInt(request, "channel", 1);
        config.params.dac.defaultValue = ctx->getParamInt(request, "defaultValue", 0);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = ctx->getParamInt(request, "stepsPerRevolution", 2048);
        config.params.stepper.speed = ctx->getParamInt(request, "speed", 8);
    } else if (config.type == PeripheralType::LCD) {
        int w = ctx->getParamInt(request, "width", 128);
        int h = ctx->getParamInt(request, "height", 64);
        int iface = ctx->getParamInt(request, "interface", 2);
        int contrast = ctx->getParamInt(request, "contrast", 255);
        config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
        config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
        config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
        config.params.lcd.contrast  = (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = ctx->getParamInt(request, "count", 1);
        int brightness = ctx->getParamInt(request, "brightness", 64);
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        config.params.neopixel.count = (uint16_t)count;
        config.params.neopixel.brightness = (uint8_t)brightness;
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = ctx->getParamInt(request, "mode", 0);
        int pulseWidth = ctx->getParamInt(request, "pulseWidth", 350);
        int repeat = ctx->getParamInt(request, "repeat", 8);
        int bitLength = ctx->getParamInt(request, "bitLength", 24);
        if (mode != 1) mode = 0;
        if (pulseWidth < 100) pulseWidth = 350;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 1) repeat = 8;
        if (repeat > 20) repeat = 20;
        if (bitLength < 1) bitLength = 24;
        if (bitLength > 32) bitLength = 32;
        config.params.rf.mode = (uint8_t)mode;
        config.params.rf.pulseWidth = (uint16_t)pulseWidth;
        config.params.rf.repeat = (uint8_t)repeat;
        config.params.rf.bitLength = (uint8_t)bitLength;
        config.params.rf.activeHigh = ctx->getParamBool(request, "activeHigh", true);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = ctx->getParamInt(request, "debounceMs", 50);
        int holdMs = ctx->getParamInt(request, "holdMs", 2000);
        if (debounceMs < 0) debounceMs = 0;
        if (debounceMs > 5000) debounceMs = 5000;
        if (holdMs < 0) holdMs = 0;
        if (holdMs > 60000) holdMs = 60000;
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = ctx->getParamBool(request, "activeHigh", true);
        config.params.radar.debounceMs = (uint16_t)debounceMs;
        config.params.radar.holdMs = (uint16_t)holdMs;
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        if (!applyP1PeripheralParams(ctx, request, config, true, errorMsg)) {
            ctx->sendError(request, 400, errorMsg);
            return;
        }
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int b = ctx->getParamInt(request, "brightness", 2);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        config.params.segment.brightness = (uint8_t)b;
    }

    // Keep the mutation and standalone preflight endpoints on one final
    // parameter interpretation path while preserving legacy validation above.
    if (!applyPeripheralRequestParameters(ctx, request, config, false,
                                           errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    if (config.id.isEmpty()) {
        config.id = "periph_" + String(millis());
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    RecursiveMutexGuard managerLock(pm.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }
    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> workspace;
        if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
        FastBee::PreflightResult& preflightResult = *workspace;
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ADD, "",
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK, ctx->networkManager),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::ADD)) {
            return;
        }
    }
    if (pm.addPeripheralAndSave(config, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral added");
    } else {
        String msg = errorMsg.isEmpty() ? String("Failed to add peripheral")
                                        : (String("Failed to add peripheral: ") + errorMsg);
        ctx->sendError(request, mutationErrorStatus(errorMsg), msg);
    }
}

void PeripheralRouteHandler::handleUpdatePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    RecursiveMutexGuard managerLock(pm.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    if (config.type == PeripheralType::MODBUS_DEVICE) {
        ctx->sendError(request, 422,
                       userConfigurablePeripheralTypeError(config.type));
        return;
    }

    String name = ctx->getParamValue(request, "name", "");
    if (!name.isEmpty()) config.name = name;

    // 更新外设类型（类型变更时重置参数为默认值）
    if (request->hasParam("type", true)) {
        PeripheralType newType = static_cast<PeripheralType>(ctx->getParamInt(request, "type", static_cast<int>(config.type)));
        if (newType != config.type) {
            config.type = newType;
            memset(&config.params, 0, sizeof(config.params));
            if (isAnalogInputConfigType(config.type)) {
                initializeAdcDefaults(config);
            } else if (config.type == PeripheralType::UART) {
                initializeUartDefaults(config);
            } else if (config.type == PeripheralType::ENCODER) {
                initializeEncoderDefaults(config);
            } else if (config.type == PeripheralType::PWM_SERVO) {
                initializeServoDefaults(config);
            } else if (config.type == PeripheralType::SDIO) {
                initializeSdCardDefaults(config);
            } else if (config.type == PeripheralType::DS1302_RTC) {
                initializeDs1302Defaults(config);
            } else if (config.type == PeripheralType::LCD1602) {
                initializeLcd1602Defaults(config);
            } else if (config.type == PeripheralType::RFID_MFRC522) {
                initializeRfidDefaults(config);
            } else if (config.type == PeripheralType::SENSOR_GENERIC) {
                initializeSensorDefaults(config);
            }
        }
    }

    const char* typeError = userConfigurablePeripheralTypeError(config.type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }

    config.enabled = ctx->getParamBool(request, "enabled", config.enabled);

    String pinsStr = ctx->getParamValue(request, "pins", "");
    String errorMsg;
    if (request->hasParam("pins", true)) {
        if (!parsePeripheralPins(pinsStr, config.pins, config.pinCount, errorMsg)) {
            ctx->sendError(request, 400, errorMsg);
            return;
        }
    }

    if (config.type == PeripheralType::UART) {
        config.params.uart.baudRate = ctx->getParamInt(request, "baudRate", config.params.uart.baudRate);
        config.params.uart.dataBits = ctx->getParamInt(request, "dataBits", config.params.uart.dataBits);
        config.params.uart.stopBits = ctx->getParamInt(request, "stopBits", config.params.uart.stopBits);
        config.params.uart.parity = ctx->getParamInt(request, "parity", config.params.uart.parity);
    } else if (config.type == PeripheralType::ENCODER) {
        const int resolution = ctx->getParamInt(
            request, "resolution", config.params.encoder.resolution ?
                config.params.encoder.resolution : ENCODER_DEFAULT_RESOLUTION);
        if (resolution < 1 || resolution > UINT16_MAX) {
            ctx->sendError(request, 400,
                           "INVALID_ENCODER_RESOLUTION: expected 1-65535 pulses per revolution");
            return;
        }
        config.params.encoder.resolution = static_cast<uint16_t>(resolution);
        config.params.encoder.useInterrupt = ctx->getParamBool(
            request, "useInterrupt", config.params.encoder.useInterrupt);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = ctx->getParamInt(request, "frequency", config.params.i2c.frequency);
        config.params.i2c.address = ctx->getParamInt(request, "address", config.params.i2c.address);
        config.params.i2c.isMaster = ctx->getParamBool(request, "isMaster", config.params.i2c.isMaster);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = ctx->getParamInt(request, "frequency", config.params.spi.frequency);
        config.params.spi.mode = ctx->getParamInt(request, "mode", config.params.spi.mode);
        config.params.spi.msbFirst = ctx->getParamBool(request, "msbFirst", config.params.spi.msbFirst);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (!applyServoParams(ctx, request, config, false, errorMsg)) {
            ctx->sendError(request, 400, errorMsg);
            return;
        }
    } else if (config.isGPIOPeripheral() && !isAnalogInputConfigType(config.type)) {
        config.params.gpio.initialState = static_cast<GPIOState>(ctx->getParamInt(request, "initialState", static_cast<int>(config.params.gpio.initialState)));
        config.params.gpio.pwmChannel = ctx->getParamInt(request, "pwmChannel", config.params.gpio.pwmChannel);
        config.params.gpio.pwmFrequency = ctx->getParamInt(request, "pwmFrequency", config.params.gpio.pwmFrequency);
        config.params.gpio.pwmResolution = ctx->getParamInt(request, "pwmResolution", config.params.gpio.pwmResolution);
        config.params.gpio.defaultDuty = ctx->getParamInt(request, "defaultDuty", config.params.gpio.defaultDuty);
        config.params.gpio.passive = ctx->getParamBool(request, "passive", config.params.gpio.passive);
        if (isDigitalInputPeripheral(config.type)) {
            applyDigitalInputParams(ctx, request, config);
        }
    } else if (isAnalogInputConfigType(config.type)) {
        if (config.params.adc.resolution < 9 || config.params.adc.resolution > 12) {
            initializeAdcDefaults(config);
        } else if (config.params.adc.engineeringMax == 0.0f) {
            config.params.adc.engineeringMax = 100.0f;
        }
        applyAnalogInputParams(ctx, request, config);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = ctx->getParamInt(request, "channel", config.params.dac.channel);
        config.params.dac.defaultValue = ctx->getParamInt(request, "defaultValue", config.params.dac.defaultValue);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = ctx->getParamInt(request, "stepsPerRevolution", config.params.stepper.stepsPerRevolution ? config.params.stepper.stepsPerRevolution : 2048);
        config.params.stepper.speed = ctx->getParamInt(request, "speed", config.params.stepper.speed ? config.params.stepper.speed : 8);
    } else if (config.type == PeripheralType::LCD) {
        int w = ctx->getParamInt(request, "width", config.params.lcd.width ? config.params.lcd.width : 128);
        int h = ctx->getParamInt(request, "height", config.params.lcd.height ? config.params.lcd.height : 64);
        int iface = ctx->getParamInt(request, "interface", config.params.lcd.interface);
        int contrast = ctx->getParamInt(request, "contrast", config.params.lcd.contrast ? config.params.lcd.contrast : 255);
        config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
        config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
        config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
        config.params.lcd.contrast  = (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = ctx->getParamInt(request, "count", config.params.neopixel.count ? config.params.neopixel.count : 1);
        int brightness = ctx->getParamInt(request, "brightness", config.params.neopixel.brightness);
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        config.params.neopixel.count = (uint16_t)count;
        config.params.neopixel.brightness = (uint8_t)brightness;
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = ctx->getParamInt(request, "mode", config.params.rf.mode);
        int pulseWidth = ctx->getParamInt(request, "pulseWidth", config.params.rf.pulseWidth ? config.params.rf.pulseWidth : 350);
        int repeat = ctx->getParamInt(request, "repeat", config.params.rf.repeat ? config.params.rf.repeat : 8);
        int bitLength = ctx->getParamInt(request, "bitLength", config.params.rf.bitLength ? config.params.rf.bitLength : 24);
        if (mode != 1) mode = 0;
        if (pulseWidth < 100) pulseWidth = 350;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 1) repeat = 8;
        if (repeat > 20) repeat = 20;
        if (bitLength < 1) bitLength = 24;
        if (bitLength > 32) bitLength = 32;
        config.params.rf.mode = (uint8_t)mode;
        config.params.rf.pulseWidth = (uint16_t)pulseWidth;
        config.params.rf.repeat = (uint8_t)repeat;
        config.params.rf.bitLength = (uint8_t)bitLength;
        config.params.rf.activeHigh = ctx->getParamBool(request, "activeHigh", config.params.rf.activeHigh);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = ctx->getParamInt(request, "debounceMs", config.params.radar.debounceMs ? config.params.radar.debounceMs : 50);
        int holdMs = ctx->getParamInt(request, "holdMs", config.params.radar.holdMs ? config.params.radar.holdMs : 2000);
        if (debounceMs < 0) debounceMs = 0;
        if (debounceMs > 5000) debounceMs = 5000;
        if (holdMs < 0) holdMs = 0;
        if (holdMs > 60000) holdMs = 60000;
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = ctx->getParamBool(request, "activeHigh", config.params.radar.activeHigh);
        config.params.radar.debounceMs = (uint16_t)debounceMs;
        config.params.radar.holdMs = (uint16_t)holdMs;
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        if (!applyP1PeripheralParams(ctx, request, config, false, errorMsg)) {
            ctx->sendError(request, 400, errorMsg);
            return;
        }
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int b = ctx->getParamInt(request, "brightness", config.params.segment.brightness);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        config.params.segment.brightness = (uint8_t)b;
    }

    if (!applyPeripheralRequestParameters(ctx, request, config, false,
                                           errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> workspace;
        if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
        FastBee::PreflightResult& preflightResult = *workspace;
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::UPDATE, id,
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK, ctx->networkManager),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::UPDATE)) {
            return;
        }
    }

    if (pm.updatePeripheralAndSave(id, config, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral updated");
    } else {
        ctx->sendError(request, mutationErrorStatus(errorMsg),
                       errorMsg.isEmpty() ? "Failed to update peripheral" : errorMsg);
    }
}

void PeripheralRouteHandler::handleDeletePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    if (config.type == PeripheralType::MODBUS_DEVICE) {
        ctx->sendError(request, 422,
                       userConfigurablePeripheralTypeError(config.type));
        return;
    }
    String errorMsg;
    if (pm.removePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral deleted");
    } else {
        const int status = errorMsg.startsWith("外设 '")
            ? 404 : mutationErrorStatus(errorMsg);
        ctx->sendError(request, status,
                       errorMsg.isEmpty() ? "Peripheral not found" : errorMsg);
    }
}

void PeripheralRouteHandler::handleEnablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    RecursiveMutexGuard managerLock(pm.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    const char* typeError = userConfigurablePeripheralTypeError(config.type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }
    config.enabled = true;
    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> workspace;
        if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
        FastBee::PreflightResult& preflightResult = *workspace;
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ENABLE, id,
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK, ctx->networkManager),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::ENABLE)) {
            return;
        }
    }
    String errorMsg;
    const bool group = config.type == PeripheralType::IO_EXPANDER &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0;
    std::vector<String> channelIds;
    if (group) {
        const String expected = ctx->getParamValue(request, "groupToken", "");
        if (expected.length() != 48 || expected != mcpGroupReviewToken(pm.getConfigurationReviewVersion())) {
            ctx->sendError(request, 409, "IO_GROUP_REVIEW_STALE: reload the group preview and confirm again"); return;
        }
        const String encoded = ctx->getParamValue(request, "channelIds", "");
        JsonDocument selection;
        if (encoded.isEmpty() || encoded.length() > 4096 ||
            deserializeJson(selection, encoded.c_str(), encoded.length()) || !selection.is<JsonArray>() ||
            selection.as<JsonArray>().size() == 0 || selection.as<JsonArray>().size() > 16) {
            ctx->sendError(request, 400, "IO_GROUP_REQUIRED: channelIds must be a JSON array of 1..16 IDs");
            return;
        }
        for (JsonVariantConst value : selection.as<JsonArrayConst>()) {
            const JsonString selected = value.as<JsonString>();
            if (!value.is<const char*>() || selected.size() == 0 || selected.size() > 40 ||
                strlen(selected.c_str()) != selected.size()) {
                ctx->sendError(request, 400, "IO_GROUP_INVALID: invalid channel ID"); return;
            }
            channelIds.emplace_back(selected.c_str());
        }
    }
    if (group ? pm.enableIoExpanderGroupAndSave(id, channelIds, errorMsg)
              : pm.enablePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral enabled");
    } else {
        ctx->sendError(request, mutationErrorStatus(errorMsg),
                       errorMsg.isEmpty() ? "Failed to enable peripheral" : errorMsg);
    }
}

void PeripheralRouteHandler::handleDisablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    if (config.type == PeripheralType::MODBUS_DEVICE) {
        ctx->sendError(request, 422,
                       userConfigurablePeripheralTypeError(config.type));
        return;
    }
    String errorMsg;
    if (pm.disablePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral disabled");
    } else {
        ctx->sendError(request, mutationErrorStatus(errorMsg),
                       errorMsg.isEmpty() ? "Failed to disable peripheral" : errorMsg);
    }
}

void PeripheralRouteHandler::handleGetPeripheralStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (ctx->getParamValue(request, "groupPreview", "0") == "1") {
        JsonDocument preview;
        String error;
        if (!buildMcpGroupPreview(pm, id, preview, error)) {
            ctx->sendError(request, error.startsWith("IO_GROUP_BUSY") ? 503 : 422, error); return;
        }
        // The snapshot lock has been released before allocating/sending HTTP.
        HandlerUtils::sendJsonStream(request, preview);
        return;
    }
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    EncoderReading encoderReading;
    const bool hasEncoderReading = config.type == PeripheralType::ENCODER &&
                                   pm.getEncoderReading(id, encoderReading);
    PeripheralRuntimeState runtimeState;
    const bool hasRuntimeState = pm.getRuntimeStateSnapshot(id, runtimeState);

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    FastBee::setFixedJsonField(data, "id", id);
    FastBee::setFixedJsonField(data, "enabled", config.enabled);
    if (config.type == PeripheralType::PCA9685_PWM &&
        !pm.getPca9685Status(id, data["pca9685"].to<JsonObject>())) {
        ctx->sendError(request, 503, "PCA_STATUS_BUSY"); return;
    }
    FastBee::setFixedJsonField(data, "status", hasRuntimeState ? static_cast<int>(runtimeState.status) : 0);
    FastBee::setFixedJsonField(data, "groupActivation", config.type == PeripheralType::IO_EXPANDER &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0);
    FastBee::setFixedJsonField(data, "memorySuspended", hasRuntimeState && runtimeState.memorySuspended);
    FastBee::setFixedJsonField(data, "controlOutputStatusAvailable", hasRuntimeState);
    if (hasRuntimeState) {
        FastBee::setFixedJsonField(data, "controlOutputClaimed", runtimeState.controlOutputClaimed);
        FastBee::setFixedJsonField(data, "controlOutputFault", runtimeState.controlOutputFault);
        FastBee::setFixedJsonField(data, "controlOutputRetiring", runtimeState.controlOutputRetiring);
        FastBee::setFixedJsonField(data, "controlOutputRetirementAttempts", runtimeState.controlOutputRetirementAttempts);
        FastBee::setFixedJsonField(data, "controlOutputRetirementError", runtimeState.controlOutputRetirementFailed
            ? "CONTROL_OUTPUT_RETIRE_UNCONFIRMED" : "");
    }

    if (hasRuntimeState) {
        FastBee::setFixedJsonField(data, "initTime", runtimeState.initTime);
        FastBee::setFixedJsonField(data, "lastActivity", runtimeState.lastActivity);
        FastBee::setFixedJsonField(data, "errorCount", runtimeState.errorCount);
        if (!runtimeState.lastError.isEmpty()) {
            FastBee::setFixedJsonField(data, "lastError", runtimeState.lastError);
        }
        if (config.type == PeripheralType::PWM_SERVO) {
            FastBee::setFixedJsonField(data, "angle", runtimeState.state.servo.currentAngle);
            FastBee::setFixedJsonField(data, "pulseUs", runtimeState.state.servo.currentPulseUs);
        } else if (config.type == PeripheralType::UART) {
            FastBee::setFixedJsonField(data, "framing", uartFramingName(config));
            addUartRuntime(data, runtimeState, config.enabled);
        }
    }
    if (hasEncoderReading) addEncoderReading(data, encoderReading);

    if (config.type == PeripheralType::SDIO) {
        const SDCardStatus status = SDCardManager::getInstance().getStatus();
        FastBee::writeSdCardStatusJson(data, status, id);
    } else if (config.type == PeripheralType::DS1302_RTC) {
#if FASTBEE_ENABLE_DS1302
        FastBee::RtcAccess rtcAccess(id);
        FastBee::setFixedJsonField(data, "initialized", rtcAccess.driver() != nullptr);
        if (rtcAccess.driver()) FastBee::setFixedJsonField(data, "running", rtcAccess.driver()->isRunning());
        else FastBee::setFixedJsonField(data, "driverError", rtcAccess.error());
#else
        FastBee::setFixedJsonField(data, "initialized", false);
#endif
    } else if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        FastBee::setFixedJsonField(data, "initialized", LCD1602Driver::getInstance().isInitialized());
        FastBee::setFixedJsonField(data, "cols", LCD1602Driver::getInstance().getCols());
        FastBee::setFixedJsonField(data, "rows", LCD1602Driver::getInstance().getRows());
        FastBee::setFixedJsonField(data, "backlight", LCD1602Driver::getInstance().isBacklightOn());
#else
        FastBee::setFixedJsonField(data, "initialized", false);
#endif
    } else if (config.type == PeripheralType::RFID_MFRC522) {
#if FASTBEE_ENABLE_RFID
        RFIDDriver::Status rfidStatus;
        String rfidStatusError;
        const bool available = RFIDDriver::getInstance().getStatus(rfidStatus, rfidStatusError);
        FastBee::setFixedJsonField(data, "statusAvailable", available);
        if (available) {
            FastBee::setFixedJsonField(data, "initialized", rfidStatus.initialized);
            FastBee::setFixedJsonField(data, "cardPresent", rfidStatus.cardPresent);
            FastBee::setFixedJsonField(data, "uid", rfidStatus.uid);
            FastBee::setFixedJsonField(data, "cardType", rfidStatus.cardType);
            if (!rfidStatus.lastError.isEmpty()) FastBee::setFixedJsonField(data, "driverError", rfidStatus.lastError);
        } else {
            FastBee::setFixedJsonField(data, "driverError", rfidStatusError);
        }
#else
        FastBee::setFixedJsonField(data, "initialized", false);
#endif
    } else if (config.type == PeripheralType::SENSOR_GENERIC &&
               config.params.sensor.driver[0] != '\0') {
        FastBee::setFixedJsonField(data, "driver", config.params.sensor.driver);
        FastBee::setFixedJsonField(data, "address", config.params.sensor.address);
        FastBee::setFixedJsonField(data, "mode", config.params.sensor.mode);
        SensorReading sample;
        bool stale = true;
        String sampleError;
        const bool sampled = FastBee::SensorDriverCache::instance().getSnapshot(id, sample, stale, sampleError);
        FastBee::setFixedJsonField(data, "sampled", sampled);
        FastBee::setFixedJsonField(data, "stale", stale);
        FastBee::setFixedJsonField(data, "timestamp", sampled ? sample.timestamp : 0);
        if (!sampleError.isEmpty()) FastBee::setFixedJsonField(data, "driverError", sampleError);
        if (sampled) {
            const auto* metadata = FastBee::DriverRegistry::getInstance().getMetadata(config.params.sensor.driver);
            JsonArray channels = data["channels"].to<JsonArray>();
            for (uint8_t i = 0; metadata && i < sample.channelCount && i < 4; ++i) {
                const auto* descriptor = metadata->getChannel(i, config.params.sensor.mode, config.params.sensor.scanMask);
                if (!descriptor) break;
                JsonObject channel = channels.add<JsonObject>();
                FastBee::setFixedJsonField(channel, "name", descriptor->name);
                FastBee::setFixedJsonField(channel, "unit", descriptor->unit);
                FastBee::setFixedJsonField(channel, "value", sample.values[i]);
                FastBee::setFixedJsonField(channel, "valid", !stale);
                if (sample.channelMetadata) {
                    FastBee::setFixedJsonField(channel, "timestamp", sample.channelTimestamps[i]);
                    FastBee::setFixedJsonField(channel, "quality", !stale ? "good" : "scan_invalid");
                    FastBee::setFixedJsonField(channel, "error", sensorChannelErrorName(sample.channelErrors[i]));
                }
            }
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleValidatePins(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String typeStr = ctx->getParamValue(request, "type", "");
    String pinsStr = ctx->getParamValue(request, "pins", "");
    String excludeId = ctx->getParamValue(request, "excludeId", "");

    if (typeStr.isEmpty() || pinsStr.isEmpty()) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing type or pins\"}");
        return;
    }

    for (size_t i = 0; i < typeStr.length(); ++i) {
        if (typeStr[i] < '0' || typeStr[i] > '9') {
            ctx->sendError(request, 400, "INVALID_PERIPHERAL_TYPE: type must be numeric");
            return;
        }
    }
    PeripheralType type = static_cast<PeripheralType>(typeStr.toInt());
    const char* typeError = userConfigurablePeripheralTypeError(type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }
    PeripheralManager& pm = PeripheralManager::getInstance();

    uint8_t parsedPins[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    uint8_t parsedPinCount = 0;
    String parseError;
    if (!parsePeripheralPins(pinsStr, parsedPins, parsedPinCount, parseError) || parsedPinCount == 0) {
        ctx->sendError(request, 400,
                       parseError.isEmpty() ? "INVALID_PIN_LIST: 至少需要一个引脚" : parseError);
        return;
    }

    String items = "";
    bool allValid = true;
    for (uint8_t pinIdx = 0; pinIdx < parsedPinCount; ++pinIdx) {
        const uint8_t pin = parsedPins[pinIdx];
        String pinError;
        String warning;
        bool pinOk = pm.validatePinForType(pin, type, pinError);
        if (!pinOk) allValid = false;

        const String conflictInfo = pm.getPinConflictInfo(pin, excludeId);
        if (!conflictInfo.isEmpty()) {
            if (conflictInfo.startsWith("警告:")) {
                warning = conflictInfo;
            } else {
                pinError = conflictInfo;
                pinOk = false;
                allValid = false;
            }
        }

        if (pinIdx > 0) items += ",";
        items += F("{\"pin\":");
        items += String(pin);
        items += F(",\"valid\":");
        items += pinOk ? F("true") : F("false");
        if (!pinError.isEmpty()) {
            items += F(",\"error\":\"");
            items += escapePeriphJsonString(pinError);
            items += F("\"");
        }
        if (!warning.isEmpty()) {
            items += F(",\"warning\":\"");
            items += escapePeriphJsonString(warning);
            items += F("\"");
        }
        items += F("}");
    }

    String response;
    response.reserve(64 + items.length());
    response += F("{\"success\":true,\"allValid\":");
    response += allValid ? F("true") : F("false");
    response += F(",\"pins\":[");
    response += items;
    response += F("]}");
    request->send(200, "application/json", response);
}

void PeripheralRouteHandler::handleWritePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    uint32_t writeConfigVersion = 0;
    if (!pm.getPeripheralSnapshot(id, config, &writeConfigVersion)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    bool success = false;
    String message;
    const char* uartWriteError = nullptr;

    if (config.type == PeripheralType::PCA9685_PWM) {
        const String command = ctx->getParamValue(request, "action", "");
        JsonDocument response;
        JsonObject data = response["data"].to<JsonObject>();
        String error;
        if (command == "prepare") {
            success = pm.preparePca9685(id, data["review"].to<JsonObject>(), error);
        } else if (command == "start") {
            success = pm.startPca9685(id, ctx->getParamValue(request, "token", ""),
                ctx->getParamValue(request, "powerCycleConfirmed", "") == "true", error);
        } else {
            uint32_t channel = 0;
            double value = 0;
            if (command == "duty" || command == "pulse" || command == "angle") {
                if (ctx->getParamValue(request, "channel", "").isEmpty()) {
                    ctx->sendError(request, 422, "PCA_CHANNEL_REQUIRED"); return;
                }
                if (!readIoRequestInteger(ctx, request, "channel", 0, 0, 15, channel, error)) {
                    ctx->sendError(request, 422, error); return;
                }
                const String text = ctx->getParamValue(request, "value", "");
                char* end = nullptr;
                value = strtod(text.c_str(), &end);
                if (text.isEmpty() || end != text.c_str() + text.length() || !std::isfinite(value)) {
                    ctx->sendError(request, 422, "PCA_VALUE_INVALID"); return;
                }
            }
            FastBee::OrdinaryControlContext control;
            FastBee::OrdinaryControlResult controlResult;
            const bool emergency = command == "disable" || command == "stop";
            if (!emergency && !FastBee::beginHttpPcaOutput(*ctx, request, id, writeConfigVersion, control)) return;
            success = pm.commandPca9685(id, command, channel, value, error,
                emergency ? nullptr : &control, &controlResult);
            if (!emergency) pm.finishOrdinaryControl(control, false);
            FastBee::writeOrdinaryControlJson(data["control"].to<JsonObject>(), controlResult);
            FastBee::setFixedJsonField(data, "configEpoch", writeConfigVersion);
        }
        response["success"] = success;
        if (!success) response["error"] = error;
        FastBee::setFixedJsonField(data, "id", id); FastBee::setFixedJsonField(data, "physicalOutputVerified", false);
        FastBee::setFixedJsonField(data, "statusAvailable", pm.getPca9685Status(id, data["status"].to<JsonObject>()));
        if (response.overflowed()) { ctx->sendError(request, 503, "PCA_RESPONSE_UNCONFIRMED"); return; }
        HandlerUtils::sendJsonStream(request, response);
        return;
    }

    if (config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] != '\0' &&
        FastBee::stringEqualsIgnoreCaseLiteral(ctx->getParamValue(request, "action", ""), "release")) {
        success = pm.releaseSensorCache(id);
        message = success ? "Sensor driver cache released" : "SENSOR_CONFIG_BUSY: retry release";
    } else if (config.type == PeripheralType::SDIO) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "SD_NOT_READY: enable the SD peripheral first");
            return;
        }
        FastBee::SdAccess storage(id, writeConfigVersion);
        if (!storage.driver()) { ctx->sendError(request, 409, storage.error()); return; }
        const String action = ctx->getParamValue(request, "action", "");
        String errorMsg;
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "mount")) {
            success = SDCardManager::getInstance().mount(errorMsg, id);
            message = success ? "SD card mounted" : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "recover")) {
            success = storage.driver()->recover(id, errorMsg);
            message = success ? "SD remounted; prior file operation was not replayed" : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "unmount")) {
            success = SDCardManager::getInstance().unmount(id, &errorMsg, false);
            message = success ? "SD card unmounted" : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "write") ||
                   FastBee::stringEqualsIgnoreCaseLiteral(action, "write-file")) {
            const String path = ctx->getParamValue(request, "path", "");
            const String content = ctx->getParamValue(request, "content", "");
            if (content.isEmpty()) {
                ctx->sendError(request, 400, "SD_WRITE_SIZE_INVALID: content is empty");
                return;
            }
            if (content.length() > SDCardManager::MAX_FILE_BYTES) {
                ctx->sendError(request, 413, "SD_WRITE_SIZE_INVALID: maximum 16384 bytes");
                return;
            }
            const bool append = ctx->getParamBool(request, "append", false);
            success = SDCardManager::getInstance().writeFile(
                path, reinterpret_cast<const uint8_t*>(content.c_str()),
                content.length(), append, errorMsg, id);
            message = success ? String("SD file written: ") +
                                    String(content.length()) + " bytes"
                              : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "delete") ||
                   FastBee::stringEqualsIgnoreCaseLiteral(action, "delete-file")) {
            const String path = ctx->getParamValue(request, "path", "");
            const String confirmation = ctx->getParamValue(request, "confirm", "");
            if (confirmation != "DELETE") {
                ctx->sendError(request, 400,
                               "SD_DELETE_CONFIRMATION_REQUIRED: use confirm=DELETE");
                return;
            }
            success = SDCardManager::getInstance().removeFile(path, errorMsg, id);
            message = success ? "SD path deleted" : errorMsg;
        } else {
            ctx->sendError(
                request, 400,
                "SD_ACTION_INVALID: expected mount, recover, unmount, write-file or delete-file");
            return;
        }
    } else if (config.type == PeripheralType::DS1302_RTC) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "DS1302_NOT_READY: enable the RTC first");
            return;
        }
#if FASTBEE_ENABLE_DS1302
        FastBee::RtcAccess rtcAccess(id);
        if (!rtcAccess.driver()) {
            ctx->sendError(request, 409, rtcAccess.error());
            return;
        }
        DS1302Driver& rtc = *rtcAccess.driver();
        const String action = ctx->getParamValue(request, "action", "");
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "setDateTime") ||
            FastBee::stringEqualsIgnoreCaseLiteral(action, "set-datetime")) {
            const int year = ctx->getParamInt(request, "year", -1);
            const int month = ctx->getParamInt(request, "month", -1);
            const int day = ctx->getParamInt(request, "day", -1);
            const int hour = ctx->getParamInt(request, "hour", -1);
            const int minute = ctx->getParamInt(request, "minute", -1);
            const int second = ctx->getParamInt(request, "second", -1);
            int dayOfWeek = ctx->getParamInt(request, "dayOfWeek", 0);
            if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
                day < 1 || day > daysInMonth(year, month) || hour < 0 ||
                hour > 23 || minute < 0 || minute > 59 || second < 0 ||
                second > 59 || dayOfWeek < 0 || dayOfWeek > 7) {
                ctx->sendError(request, 400, "DS1302_DATETIME_INVALID");
                return;
            }
            if (dayOfWeek == 0) {
                dayOfWeek = calculateDayOfWeek(year, month, day);
            }
            DS1302DateTime value = {
                static_cast<uint8_t>(year - 2000), static_cast<uint8_t>(month),
                static_cast<uint8_t>(day), static_cast<uint8_t>(hour),
                static_cast<uint8_t>(minute), static_cast<uint8_t>(second),
                static_cast<uint8_t>(dayOfWeek)
            };
            success = rtc.setDateTime(value);
            message = success ? "DS1302 date/time updated" : "DS1302_WRITE_FAILED";
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "setRunning") ||
                   FastBee::stringEqualsIgnoreCaseLiteral(action, "set-running")) {
            if (!request->hasParam("running", true)) {
                ctx->sendError(request, 400, "DS1302_RUNNING_REQUIRED");
                return;
            }
            const bool running = ctx->getParamBool(request, "running", true);
            if (!rtc.readDateTime().isValid()) {
                ctx->sendError(request, 502, "DS1302_INVALID_DATA: set a valid date/time first");
                return;
            }
            rtc.setRunning(running);
            success = rtc.isRunning() == running;
            message = success ? (running ? "DS1302 clock started"
                                         : "DS1302 clock stopped")
                              : "DS1302_RUNNING_VERIFY_FAILED";
        } else {
            ctx->sendError(
                request, 400,
                "DS1302_ACTION_INVALID: expected set-datetime or set-running");
            return;
        }
#else
        ctx->sendError(request, 501, "DS1302_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::LCD1602) {
        if (!config.enabled) {
            ctx->sendError(request, 409,
                           "LCD1602_NOT_READY: enable the display first");
            return;
        }
#if FASTBEE_ENABLE_LCD1602
        LCD1602Driver& lcd = LCD1602Driver::getInstance();
        if (!lcd.isInitialized()) {
            ctx->sendError(request, 409, "LCD1602_NOT_INITIALIZED");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "");
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "print")) {
            const String content = ctx->getParamValue(request, "text", "");
            const int row = ctx->getParamInt(request, "row", 0);
            const int col = ctx->getParamInt(request, "col", 0);
            if (content.isEmpty() || content.length() > LCD1602_API_MAX_BYTES ||
                row < 0 || row >= lcd.getRows() || col < 0 ||
                col >= lcd.getCols()) {
                ctx->sendError(
                    request, 400,
                    "LCD1602_PRINT_INVALID: check text, row and column");
                return;
            }
            success = lcd.printAt(static_cast<uint8_t>(col), static_cast<uint8_t>(row), content);
            message = success ? "LCD1602 text printed" : "LCD1602_PRINT_FAILED";
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "clear")) {
            success = lcd.clear();
            message = success ? "LCD1602 cleared" : "LCD1602_CLEAR_UNCONFIRMED";
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "backlight")) {
            if (!request->hasParam("enabled", true)) {
                ctx->sendError(request, 400,
                               "LCD1602_BACKLIGHT_VALUE_REQUIRED");
                return;
            }
            success = lcd.setBacklight(ctx->getParamBool(request, "enabled", true));
            message = success ? "LCD1602 backlight updated" : "LCD1602_BACKLIGHT_UNCONFIRMED";
        } else {
            ctx->sendError(
                request, 400,
                "LCD1602_ACTION_INVALID: expected print, clear or backlight");
            return;
        }
#else
        ctx->sendError(request, 501, "LCD1602_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "RFID_NOT_READY: enable the reader first");
            return;
        }
#if FASTBEE_ENABLE_RFID
        RFIDDriver& reader = RFIDDriver::getInstance();
        if (!reader.isInitialized()) {
            ctx->sendError(request, 409, "RFID_NOT_INITIALIZED");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "");
        const int blockValue = ctx->getParamInt(request, "block", -1);
        String keyText = ctx->getParamValue(request, "key", "");
        uint8_t key[6] = {0};
        size_t keyLength = 0;
        String errorMsg;
        const bool keyParsed =
            parseStrictHex(keyText, key, sizeof(key), keyLength, errorMsg);
        keyText = "";
        if (blockValue < 1 || blockValue > 62) {
            memset(key, 0, sizeof(key));
            ctx->sendError(
                request, 400,
                "RFID_BLOCK_INVALID: block must be between 1 and 62");
            return;
        }
        if (!keyParsed || keyLength != sizeof(key)) {
            memset(key, 0, sizeof(key));
            ctx->sendError(
                request, 400,
                keyLength != sizeof(key)
                    ? "RFID_KEY_INVALID: exactly six bytes are required"
                    : errorMsg);
            return;
        }

        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "readBlock") ||
            FastBee::stringEqualsIgnoreCaseLiteral(action, "read-block")) {
            uint8_t blockData[16] = {0};
            success = reader.readBlock(static_cast<uint8_t>(blockValue), key,
                                       blockData, errorMsg);
            memset(key, 0, sizeof(key));
            if (!success) {
                ctx->sendError(request, 409, errorMsg);
                return;
            }
            JsonDocument response;
            response["success"] = true;
            JsonObject payload = response["data"].to<JsonObject>();
            FastBee::setFixedJsonField(payload, "id", id);
            FastBee::setFixedJsonField(payload, "block", blockValue);
            FastBee::setFixedJsonField(payload, "hex", bytesToHex(blockData, sizeof(blockData)));
            FastBee::setFixedJsonField(payload, "uid", reader.getLastUID());
            FastBee::setFixedJsonField(payload, "cardType", reader.getLastCardType());
            memset(blockData, 0, sizeof(blockData));
            HandlerUtils::sendJsonStream(request, response);
            return;
        }

        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "writeBlock") ||
            FastBee::stringEqualsIgnoreCaseLiteral(action, "write-block")) {
            const String confirmation =
                ctx->getParamValue(request, "confirm", "");
            if (confirmation != "WRITE_BLOCK") {
                memset(key, 0, sizeof(key));
                ctx->sendError(
                    request, 400,
                    "RFID_WRITE_CONFIRMATION_REQUIRED: use confirm=WRITE_BLOCK");
                return;
            }
            const String dataText = ctx->getParamValue(request, "data", "");
            uint8_t blockData[16] = {0};
            size_t dataLength = 0;
            const bool dataParsed = parseStrictHex(
                dataText, blockData, sizeof(blockData), dataLength, errorMsg);
            if (!dataParsed || dataLength != sizeof(blockData)) {
                memset(key, 0, sizeof(key));
                memset(blockData, 0, sizeof(blockData));
                ctx->sendError(
                    request, 400,
                    dataLength != sizeof(blockData)
                        ? "RFID_DATA_INVALID: exactly 16 bytes are required"
                        : errorMsg);
                return;
            }
            success = reader.writeBlockVerified(
                static_cast<uint8_t>(blockValue), key, blockData, errorMsg);
            memset(key, 0, sizeof(key));
            memset(blockData, 0, sizeof(blockData));
            message = success ? "RFID block written and verified" : errorMsg;
        } else {
            memset(key, 0, sizeof(key));
            ctx->sendError(
                request, 400,
                "RFID_ACTION_INVALID: expected read-block or write-block");
            return;
        }
#else
        ctx->sendError(request, 501, "RFID_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::ENCODER) {
        String action = ctx->getParamValue(request, "action", "");
        const bool resetRequested = ctx->getParamBool(request, "reset", false) ||
                                    FastBee::stringEqualsIgnoreCaseLiteral(action, "reset");
        if (!resetRequested) {
            ctx->sendError(request, 400, "ENCODER_READ_ONLY: use action=reset or reset=1");
            return;
        }
        success = pm.resetEncoder(id);
        message = success ? "Encoder reset" : "Encoder reset failed";
    } else if (config.type == PeripheralType::PWM_SERVO) {
        FastBee::handleHttpServoOutput(*ctx, request, id, writeConfigVersion);
        return;
    } else if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
               !config.isExpanderChannel() && !config.params.gpio.passive) {
        FastBee::handleHttpNativeDigital(*ctx, request, id, writeConfigVersion);
        return;
    } else if (!config.isModbusPeripheral() && !config.isExpanderChannel() &&
               (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
                config.type == PeripheralType::GPIO_ANALOG_OUTPUT || config.type == PeripheralType::DAC ||
                (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive))) {
        FastBee::handleHttpNativeScalar(*ctx, request, id, writeConfigVersion);
        return;
    } else if (config.isGPIOPeripheral()) {
        if (request->hasParam("state", true)) {
            int stateValue = ctx->getParamInt(request, "state", 0);
            GPIOState state = (stateValue == 1) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            success = pm.writePin(id, state);
            message = success ? "GPIO state set" : "GPIO write failed";
        } else if (request->hasParam("toggle", true)) {
            success = pm.togglePin(id);
            message = success ? "GPIO toggled" : "GPIO toggle failed";
        } else if (request->hasParam("pwm", true) &&
                 (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
                  config.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
                  config.type == PeripheralType::PWM_SERVO)) {
            uint32_t dutyCycle = ctx->getParamInt(request, "pwm", 0);
            success = pm.writePWM(id, dutyCycle);
            message = success ? "PWM duty set" : "PWM write failed";
        } else {
            ctx->sendError(request, 400, "Missing parameter (state/toggle/pwm)");
            return;
        }
    } else if (config.type == PeripheralType::DAC) {
        if (request->hasParam("value", true)) {
            const String text = ctx->getParamValue(request, "value", "");
            uint8_t value = 0;
            if (!FastBee::parseDacActionValue(text.c_str(), text.length(), value)) {
                ctx->sendError(request, 400, "DAC value must be an integer from 0 to 255"); return;
            }
            success = FastBee::writeDacAtVersion(pm, id, writeConfigVersion, value);
            message = success ? "DAC write accepted (SDK only)" : "DAC write unconfirmed or configuration changed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    } else if (config.type == PeripheralType::RF_MODULE) {
        FastBee::handleHttpRfOutput(*ctx, request, id, writeConfigVersion);
        return;
    } else if (config.isCommunicationPeripheral()) {
        const auto writeCommunication = [&](const uint8_t* bytes, size_t length) {
            if (config.type != PeripheralType::UART) return pm.writeData(id, bytes, length);
            RecursiveMutexGuard lock(pm.getMutex(), 0);
            PeripheralConfig current;
            PeripheralRuntimeState runtime;
            uint32_t currentVersion = 0;
            if (!lock.isLocked()) uartWriteError = "UART_BUSY: no bytes submitted";
            else if (!pm.getPeripheralSnapshot(id, current, &currentVersion) ||
                     currentVersion != writeConfigVersion || current.type != PeripheralType::UART)
                uartWriteError = "UART_CONFIG_CHANGED: no bytes submitted";
            else if (!current.enabled || !pm.getRuntimeStateSnapshot(id, runtime) ||
                     runtime.memorySuspended || !runtime.state.comm.isConnected)
                uartWriteError = "UART_NOT_READY: no bytes submitted";
            else return pm.writeData(id, bytes, length);
            return false;
        };
        if (request->hasParam("data", true)) {
            String dataStr = ctx->getParamValue(request, "data", "");
            if (!dataStr.isEmpty()) {
                if (request->hasParam("hex", true) && ctx->getParamInt(request, "hex", 0) == 1) {
                    uint8_t bytes[UART_API_MAX_BYTES];
                    size_t byteCount = 0;
                    String parseError;
                    if (!parseStrictHex(dataStr, bytes, sizeof(bytes), byteCount, parseError)) {
                        ctx->sendError(request,
                            parseError.startsWith("PAYLOAD_TOO_LARGE") ? 413 : 400,
                            parseError);
                        return;
                    }
                    success = writeCommunication(bytes, byteCount);
                    message = success ? String("Sent ") + String(byteCount) + " bytes (HEX)" : "Send failed";
                } else {
                    if (config.type == PeripheralType::UART &&
                        dataStr.length() > UART_API_MAX_BYTES) {
                        ctx->sendError(request, 413,
                            String("PAYLOAD_TOO_LARGE: maximum ") +
                            String(UART_API_MAX_BYTES) + " bytes");
                        return;
                    }
                    success = writeCommunication((const uint8_t*)dataStr.c_str(), dataStr.length());
                    message = success ? String("Sent ") + String(dataStr.length()) + " bytes" : "Send failed";
                }
            } else {
                ctx->sendError(request, 400, "Data is empty");
                return;
            }
        } else {
            ctx->sendError(request, 400, "Missing data parameter");
            return;
        }
    } else {
        if (request->hasParam("value", true)) {
            int value = ctx->getParamInt(request, "value", 0);
            uint8_t bytes[4];
            bytes[0] = value & 0xFF;
            bytes[1] = (value >> 8) & 0xFF;
            bytes[2] = (value >> 16) & 0xFF;
            bytes[3] = (value >> 24) & 0xFF;

            size_t len = 1;
            if (value > 0xFF) len = 2;
            if (value > 0xFFFF) len = 4;

            success = pm.writeData(id, bytes, len);
            message = success ? "Data written" : "Data write failed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    }

    if (config.type == PeripheralType::UART) {
        message = uartWriteError ? uartWriteError : success ? "UART_LOCAL_ACCEPTED: bytes accepted by local driver; peer not verified"
                          : "UART_WRITE_UNCONFIRMED: bytes may have been partially sent; do not retry blindly";
    }
    if (success) {
        ctx->sendSuccess(request, message);
    } else {
        ctx->sendError(request, 400, message);
    }
}
