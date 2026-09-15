#include "./network/handlers/PeripheralRouteHandler.h"
#include "core/MemoryBudget.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/FeatureFlags.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/PeripheralManager.h"
#include "core/ResourceProfile.h"
#include "core/SensorDriverCache.h"
#include "peripherals/SDCardManager.h"
#if FASTBEE_ENABLE_DS1302
#include "peripherals/drivers/DS1302Driver.h"
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
#include <math.h>
#include <vector>
#include "systems/LoggerSystem.h"

namespace {
constexpr uint32_t SENSOR_DEFAULT_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t SENSOR_MIN_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t SENSOR_MAX_SAMPLE_INTERVAL_MS = 86400000UL;
constexpr size_t UART_API_MAX_BYTES = 128;
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

void addDigitalInputParams(JsonObject params, const PeripheralConfig& config) {
    params["inputMode"] = config.params.gpio.inputMode;
    params["invertLogic"] = config.params.gpio.invertLogic;
    params["debounceMs"] = config.params.gpio.debounceMs ? config.params.gpio.debounceMs : 50;
    params["reportMode"] = config.params.gpio.reportMode;
}

void addAnalogInputParams(JsonObject params, const PeripheralConfig& config, bool includeAdvanced = true) {
    params["attenuation"] = config.params.adc.attenuation;
    params["resolution"] = config.params.adc.resolution;
    if (includeAdvanced) params["sampleRate"] = config.params.adc.sampleRate;
    params["sampleCount"] = config.params.adc.sampleCount ? config.params.adc.sampleCount : 1;
    params["filterMode"] = config.params.adc.filterMode;
    if (includeAdvanced) {
        params["signalMode"] = config.params.adc.signalMode;
        params["conversionEnabled"] = config.params.adc.conversionEnabled;
        params["clampEnabled"] = config.params.adc.clampEnabled;
        params["decimalPlaces"] = config.params.adc.decimalPlaces;
        params["inputMin"] = config.params.adc.inputMin;
        params["inputMax"] = config.params.adc.inputMax;
        params["engineeringMin"] = config.params.adc.engineeringMin;
        params["engineeringMax"] = config.params.adc.engineeringMax;
        params["engineeringOffset"] = config.params.adc.engineeringOffset;
        params["vRef"] = config.params.adc.vRef;
        params["voltageRatio"] = config.params.adc.voltageRatio;
        params["shuntResistance"] = config.params.adc.shuntResistance;
        params["inputGain"] = config.params.adc.inputGain;
        params["inputOffset"] = config.params.adc.inputOffset;
        params["rawMin"] = config.params.adc.rawMin;
        params["rawMax"] = config.params.adc.rawMax;
    }
}

void addServoParams(JsonObject params, const PeripheralConfig& config) {
    params["frequency"] = config.params.servo.frequency;
    params["minPulse"] = config.params.servo.minPulse;
    params["maxPulse"] = config.params.servo.maxPulse;
    params["minAngle"] = config.params.servo.minAngle;
    params["maxAngle"] = config.params.servo.maxAngle;
    params["initialAngle"] = config.params.servo.initialAngle;
    params["inverted"] = config.params.servo.inverted;
    params["releaseAfterMove"] = config.params.servo.releaseAfterMove;
}

void addUartParams(JsonObject params, const PeripheralConfig& config) {
    params["baudRate"] = config.params.uart.baudRate;
    params["dataBits"] = config.params.uart.dataBits;
    params["stopBits"] = config.params.uart.stopBits;
    params["parity"] = config.params.uart.parity;
    params["framing"] = uartFramingName(config);
}

void addEncoderParams(JsonObject params, const PeripheralConfig& config) {
    params["resolution"] = config.params.encoder.resolution;
    params["useInterrupt"] = config.params.encoder.useInterrupt;
}

void addP1PeripheralParams(JsonObject params, const PeripheralConfig& config) {
    if (config.type == PeripheralType::SDIO) {
        params["interface"] = config.params.sdcard.interface;
        params["frequency"] = config.params.sdcard.frequency;
    } else if (config.type == PeripheralType::DS1302_RTC) {
        params["use24Hour"] = config.params.ds1302.use24Hour;
        params["trickleCharge"] = config.params.ds1302.trickleCharge;
    } else if (config.type == PeripheralType::LCD1602) {
        params["cols"] = config.params.lcd1602.cols;
        params["rows"] = config.params.lcd1602.rows;
        params["address"] = config.params.lcd1602.address;
        params["backlight"] = config.params.lcd1602.backlight;
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        params["frequency"] = config.params.rfid.frequency;
    } else if (config.type == PeripheralType::SENSOR_GENERIC) {
        params["sensorType"] = config.params.sensor.sensorType;
        params["sampleInterval"] = normalizeSensorSampleInterval(
            static_cast<int>(config.params.sensor.sampleInterval));
        if (config.params.sensor.driver[0] != '\0') {
            params["driver"] = config.params.sensor.driver;
            params["address"] = config.params.sensor.address;
            params["mode"] = config.params.sensor.mode;
            params["frequency"] = config.params.sensor.frequency;
        }
    }
}

void addUartRuntime(JsonObject target, const PeripheralRuntimeState& runtime) {
    target["bytesSent"] = runtime.state.comm.bytesSent;
    target["bytesReceived"] = runtime.state.comm.bytesReceived;
    target["connected"] = runtime.state.comm.isConnected;
    target["port"] = runtime.state.comm.port == 255
        ? -1 : static_cast<int>(runtime.state.comm.port);
    target["rxAvailable"] = runtime.state.comm.rxAvailable;
}

void addEncoderReading(JsonObject target, const EncoderReading& reading) {
    target["count"] = reading.count;
    target["delta"] = reading.delta;
    target["direction"] = reading.direction;
    target["frequencyHz"] = reading.frequencyHz;
    target["rpm"] = reading.rpm;
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
        int sensorType = ctx->getParamInt(request, "sensorType", config.params.sensor.sensorType);
        if (sensorType < 0) sensorType = 0;
        if (sensorType > 255) sensorType = 255;
        config.params.sensor.sensorType = static_cast<uint8_t>(sensorType);
        config.params.sensor.sampleInterval = normalizeSensorSampleInterval(
            ctx->getParamInt(request, "sampleInterval",
                             static_cast<int>(config.params.sensor.sampleInterval)));

        String driver = ctx->getParamValue(
            request, "driver", String(config.params.sensor.driver));
        driver.trim();
        driver.toUpperCase();
        if (driver.length() >= sizeof(config.params.sensor.driver)) {
            errorMsg = "INVALID_SENSOR_DRIVER: 驱动名过长";
            return false;
        }
        if (!driver.isEmpty() && driver != "BMP280" && driver != "MPU6050") {
            errorMsg = "INVALID_SENSOR_DRIVER: 仅支持 BMP280 或 MPU6050";
            return false;
        }
        strlcpy(config.params.sensor.driver, driver.c_str(),
                sizeof(config.params.sensor.driver));
        const int defaultAddress = driver == "MPU6050" ? 0x68 : 0x76;
        const int address = ctx->getParamInt(
            request, "address",
            config.params.sensor.address ? config.params.sensor.address : defaultAddress);
        const int mode = ctx->getParamInt(request, "mode", config.params.sensor.mode);
        const int frequency = ctx->getParamInt(
            request, "frequency",
            config.params.sensor.frequency ? config.params.sensor.frequency : 100000);
        if (!driver.isEmpty() && (address < 1 || address > 0x7F)) {
            errorMsg = "INVALID_SENSOR_ADDRESS: I2C 地址必须为 1-127";
            return false;
        }
        if (mode < 0 || mode > 1 || (frequency != 100000 && frequency != 400000)) {
            errorMsg = "INVALID_SENSOR_CONFIG: 模式或 I2C 频率无效";
            return false;
        }
        config.params.sensor.address = static_cast<uint8_t>(address);
        config.params.sensor.mode = static_cast<uint8_t>(mode);
        config.params.sensor.frequency = static_cast<uint32_t>(frequency);
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
bool isAnalogInputConfigType(PeripheralType type) {
    return type == PeripheralType::ADC || type == PeripheralType::GPIO_ANALOG_INPUT;
}

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

bool applyPeripheralRequestParameters(WebHandlerContext* ctx,
                                      AsyncWebServerRequest* request,
                                      PeripheralConfig& config,
                                      bool resetDefaults,
                                      String& errorMsg) {
    if (config.type == PeripheralType::UART) {
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
    return true;
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

void addPeripheralTypeDescriptorDetails(JsonObject typeObj,
                                        PeripheralType type) {
    JsonArray roles = typeObj["pinRoles"].to<JsonArray>();
    auto addRoles = [&roles](std::initializer_list<const char*> values) {
        for (const char* value : values) roles.add(value);
    };

    switch (type) {
        case PeripheralType::UART: addRoles({"RX", "TX"}); break;
        case PeripheralType::I2C: addRoles({"SDA", "SCL"}); break;
        case PeripheralType::SPI: addRoles({"MISO", "MOSI", "SCK", "CS"}); break;
        case PeripheralType::SDIO: addRoles({"SCK", "MOSI", "MISO", "CS"}); break;
        case PeripheralType::SENSOR_GENERIC:
            // SENSOR_GENERIC covers single-wire/DHT, HC-SR04 and explicit
            // local I2C drivers. A single fixed SDA/SCL descriptor caused Web
            // templates to assign the wrong roles to DHT and ultrasonic pins.
            addRoles({"DATA"});
            typeObj["minPinCount"] = 0;
            typeObj["maxPinCount"] = 2;
            {
                JsonArray variants = typeObj["pinRoleVariants"].to<JsonArray>();
                JsonObject singleWire = variants.add<JsonObject>();
                singleWire["key"] = "single-wire";
                singleWire["pinRoles"].to<JsonArray>().add("DATA");
                JsonObject ultrasonic = variants.add<JsonObject>();
                ultrasonic["key"] = "ultrasonic";
                ultrasonic["sensorType"] = 3;
                JsonArray ultrasonicRoles = ultrasonic["pinRoles"].to<JsonArray>();
                ultrasonicRoles.add("TRIG");
                ultrasonicRoles.add("ECHO");
                JsonObject i2cDriver = variants.add<JsonObject>();
                i2cDriver["key"] = "i2c-driver";
                JsonArray drivers = i2cDriver["drivers"].to<JsonArray>();
                drivers.add("BMP280");
                drivers.add("MPU6050");
                JsonArray i2cRoles = i2cDriver["pinRoles"].to<JsonArray>();
                i2cRoles.add("SDA");
                i2cRoles.add("SCL");
            }
            break;
        case PeripheralType::PWM_SERVO: addRoles({"PWM"}); break;
        case PeripheralType::STEPPER_MOTOR:
            addRoles({"IN1", "IN2", "IN3", "IN4"});
            break;
        case PeripheralType::ENCODER: addRoles({"A", "B"}); break;
        case PeripheralType::ONE_WIRE:
        case PeripheralType::NEO_PIXEL:
        case PeripheralType::RF_MODULE:
        case PeripheralType::IR_RECEIVER:
            addRoles({"DATA"});
            break;
        case PeripheralType::SEVEN_SEGMENT_TM1637:
            addRoles({"CLK", "DIO"});
            break;
        case PeripheralType::RADAR_SENSOR: addRoles({"OUT"}); break;
        case PeripheralType::DS1302_RTC:
            addRoles({"CE", "IO", "SCLK"});
            break;
        case PeripheralType::LCD1602: addRoles({"SDA", "SCL"}); break;
        case PeripheralType::RFID_MFRC522:
            addRoles({"SCK", "MOSI", "MISO", "SS", "RST"});
            break;
        default:
            if (getPeripheralPinCount(type) == 1) addRoles({"GPIO"});
            break;
    }

    JsonArray operations = typeObj["operations"].to<JsonArray>();
    auto addOperations = [&operations](std::initializer_list<const char*> values) {
        for (const char* value : values) operations.add(value);
    };
    switch (type) {
        case PeripheralType::UART: addOperations({"read", "write"}); break;
        case PeripheralType::I2C: addOperations({"scan"}); break;
        case PeripheralType::SDIO:
            typeObj["supportedInterfaces"].to<JsonArray>().add("sdspi");
            addOperations({"status", "mount", "unmount", "list", "read-file",
                           "write-file", "delete-file"});
            break;
        case PeripheralType::SENSOR_GENERIC:
            {
                JsonArray drivers = typeObj["drivers"].to<JsonArray>();
                drivers.add("BMP280");
                drivers.add("MPU6050");
            }
            addOperations({"scan", "probe", "read", "release"});
            break;
        case PeripheralType::DS1302_RTC:
            addOperations({"read", "set-datetime", "set-running"});
            break;
        case PeripheralType::LCD1602:
            addOperations({"status", "print", "clear", "backlight"});
            break;
        case PeripheralType::RFID_MFRC522:
            addOperations({"scan", "read-block", "write-block"});
            break;
        case PeripheralType::ENCODER:
            addOperations({"read", "reset"});
            break;
        case PeripheralType::PWM_SERVO:
            addOperations({"read", "set-angle"});
            break;
        default:
            break;
    }
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
#if CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C6
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
    return nullptr;
}

const char* boardProfileChip(const FastBee::BoardProfile& profile) {
    return profile.id == FastBee::BoardProfileId::ESP32_C6_DEVKITC_1
        ? "ESP32-C6" : "ESP32";
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

void addPinRiskCodes(JsonArray risks, uint32_t mask) {
    struct RiskName {
        uint32_t bit;
        const char* name;
    };
    static constexpr RiskName NAMES[] = {
        {FastBee::PIN_RISK_FLASH, "flash"},
        {FastBee::PIN_RISK_NOT_EXPOSED, "not_exposed"},
        {FastBee::PIN_RISK_STRAPPING, "strapping"},
        {FastBee::PIN_RISK_INPUT_ONLY, "input_only"},
        {FastBee::PIN_RISK_UART0_CONSOLE, "uart0_console"},
        {FastBee::PIN_RISK_USB_SERIAL_JTAG, "usb_serial_jtag"},
        {FastBee::PIN_RISK_JTAG, "jtag"},
        {FastBee::PIN_RISK_ONBOARD_RGB, "onboard_rgb"},
        {FastBee::PIN_RISK_LP_I2C, "lp_i2c"},
        {FastBee::PIN_RISK_ADC2_WIFI, "adc2_wifi"}
    };
    for (const RiskName& item : NAMES) {
        if (mask & item.bit) risks.add(item.name);
    }
}

void addBoardProfileJson(JsonObject target,
                         const FastBee::BoardProfile& profile,
                         bool active) {
    target["id"] = profile.key;
    target["profileCode"] = static_cast<uint8_t>(profile.id);
    target["key"] = profile.key;
    target["displayName"] = profile.displayName;
    target["version"] = profile.version;
    target["source"] = profile.source;
    target["chip"] = boardProfileChip(profile);
    target["active"] = active;
    target["hardwareVerified"] = false;
    target["pinCount"] = profile.pinCount;

    JsonObject limits = target["resources"].to<JsonObject>();
    limits["i2cBusCount"] = profile.resources.i2cBusCount;
    limits["spiBusCount"] = profile.resources.spiBusCount;
    limits["genericUartCount"] = profile.resources.genericUartCount;
    limits["firstGenericUartPort"] = profile.resources.firstGenericUartPort;
    limits["ledcChannelCount"] = profile.resources.ledcChannelCount;
    limits["ledcTimerCount"] = profile.resources.ledcTimerCount;
    limits["rmtChannelCount"] = profile.resources.rmtChannelCount;
    limits["adcUnitCount"] = profile.resources.adcUnitCount;
    limits["encoderSlotCount"] = profile.resources.encoderSlotCount;
    limits["pcntUnitCount"] = profile.resources.pcntUnitCount;
    limits["pcntChannelsPerUnit"] = profile.resources.pcntChannelsPerUnit;
    limits["sdmmcSupported"] = profile.resources.sdmmcSupported;

    JsonObject defaults = target["defaultPins"].to<JsonObject>();
    defaults["i2cSda"] = profile.defaults.i2cSda;
    defaults["i2cScl"] = profile.defaults.i2cScl;
    defaults["spiMiso"] = profile.defaults.spiMiso;
    defaults["spiMosi"] = profile.defaults.spiMosi;
    defaults["spiSck"] = profile.defaults.spiSck;
    defaults["spiCs"] = profile.defaults.spiCs;
    defaults["uart0Rx"] = profile.defaults.uart0Rx;
    defaults["uart0Tx"] = profile.defaults.uart0Tx;
    if (profile.defaults.onboardRgb == FastBee::BOARD_PIN_NONE) {
        defaults["onboardRgb"] = nullptr;
    } else {
        defaults["onboardRgb"] = profile.defaults.onboardRgb;
    }

    JsonArray pins = target["pins"].to<JsonArray>();
    for (size_t i = 0; i < profile.pinCount; ++i) {
        const FastBee::BoardPinProfile& pin = profile.pins[i];
        JsonObject pinJson = pins.add<JsonObject>();
        pinJson["gpio"] = pin.gpio;
        pinJson["policy"] = FastBee::pinPolicyCode(pin.policy);
        JsonObject capabilities = pinJson["capabilities"].to<JsonObject>();
        capabilities["input"] = (pin.capabilities & FastBee::PIN_CAP_INPUT) != 0;
        capabilities["output"] = (pin.capabilities & FastBee::PIN_CAP_OUTPUT) != 0;
        capabilities["pullUp"] = (pin.capabilities & FastBee::PIN_CAP_PULL_UP) != 0;
        capabilities["pullDown"] = (pin.capabilities & FastBee::PIN_CAP_PULL_DOWN) != 0;
        capabilities["interrupt"] = (pin.capabilities & FastBee::PIN_CAP_INTERRUPT) != 0;
        capabilities["adc"] = (pin.capabilities & FastBee::PIN_CAP_ADC) != 0;
        capabilities["dac"] = (pin.capabilities & FastBee::PIN_CAP_DAC) != 0;
        capabilities["touch"] = (pin.capabilities & FastBee::PIN_CAP_TOUCH) != 0;
        addPinRiskCodes(pinJson["risks"].to<JsonArray>(), pin.risks);
        if (pin.adcUnit != FastBee::BOARD_PIN_NONE) {
            JsonObject adc = pinJson["adc"].to<JsonObject>();
            adc["unit"] = pin.adcUnit;
            adc["channel"] = pin.adcChannel;
        }
    }
}

FastBee::PreflightContext makePreflightContext(
        const FastBee::BoardProfile& profile, uint32_t acknowledgements) {
    FastBee::PreflightContext context;
    context.wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
    context.uart0ConsoleActive = true;
    context.usbSerialJtagActive =
        profile.id == FastBee::BoardProfileId::ESP32_C6_DEVKITC_1;
    context.jtagActive = false;
    context.acknowledgements = acknowledgements & PREFLIGHT_ACK_MASK;
    return context;
}

bool sameResourceIdentity(const FastBee::ResourceClaim& left,
                          const FastBee::ResourceClaim& right) {
    if (left.kind != right.kind) return false;
    switch (left.kind) {
        case FastBee::ResourceKind::GPIO:
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
        row["id"] = FastBee::resourceKindName(kind);
        row["label"] = resourceLabel(kind);
        row["used"] = used;
        row["configured"] = configured;
        row["capacity"] = capacity;
        row["available"] = used < capacity ? capacity - used : 0;
        row["status"] = capacity == 0 ? "unavailable"
            : (used > capacity ? "overcommitted"
                : (used == capacity ? "full" : "available"));
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
        item["code"] = FastBee::preflightIssueCodeName(issue.code);
        item["severity"] = FastBee::preflightSeverityName(issue.severity);
        item["requiresConfirmation"] = issue.severity ==
            FastBee::PreflightSeverity::WARNING_REQUIRES_CONFIRMATION;
        item["resourceKind"] = FastBee::resourceKindName(issue.resourceKind);
        item["unit"] = issue.unit;
        item["index"] = issue.index;
        if (issue.ownerId) item["ownerId"] = issue.ownerId;
        if (issue.conflictingOwnerId) {
            item["conflictingOwnerId"] = issue.conflictingOwnerId;
        }
        item["message"] = issue.message ? issue.message : "";
    }
}

void addCandidateClaims(JsonArray claims,
                         const FastBee::PreflightResult& result,
                         const char* candidateId) {
    for (size_t i = 0; i < result.claimCount; ++i) {
        const FastBee::ResourceClaim& claim = result.claims[i];
        if (!sameText(claim.ownerId, candidateId)) continue;
        JsonObject item = claims.add<JsonObject>();
        item["resourceKind"] = FastBee::resourceKindName(claim.kind);
        item["state"] = claimStateName(claim.state);
        item["shareMode"] = shareModeName(claim.shareMode);
        item["role"] = pinRoleName(claim.pinRole);
        item["unit"] = claim.unit;
        item["index"] = claim.index;
        item["ownerId"] = claim.ownerId ? claim.ownerId : "";
        item["parameterA"] = claim.parameterA;
        item["parameterB"] = claim.parameterB;
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
    if (value.equalsIgnoreCase("add")) {
        operation = FastBee::PreflightOperation::ADD;
        return true;
    }
    if (value.equalsIgnoreCase("update")) {
        operation = FastBee::PreflightOperation::UPDATE;
        return true;
    }
    if (value.equalsIgnoreCase("enable")) {
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

    if (!isPeripheralTypeImplemented(candidate.type)) {
        errorMsg =
            "UNSUPPORTED_PERIPHERAL_TYPE: current firmware does not implement this type";
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
    data["operation"] = preflightOperationName(operation);
    data["profileId"] = profile.key;
    data["allowed"] = allowed;
    data["requiresConfirmation"] = result.requiresConfirmation;
    data["requiredAcknowledgements"] = requiredAcknowledgements(result);
    data["claimsTruncated"] = result.claimsTruncated;
    data["issuesTruncated"] = result.issuesTruncated;
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

void PeripheralRouteHandler::handleGetBoardProfiles(
        AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    const FastBee::BoardProfile* active = compiledBoardProfileSupported()
        ? &FastBee::getCompiledBoardProfile() : nullptr;
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["compiledChip"] = CHIP_NAME;
    data["activeProfileSupported"] = active != nullptr;
    data["activeProfileId"] = active ? active->key : "";
    data["selectionBasis"] = "compiled-target-policy";
    JsonArray profiles = data["profiles"].to<JsonArray>();

    const FastBee::BoardProfile& pz = FastBee::getPuzhongPzEsp32Profile();
    const FastBee::BoardProfile& c6 = FastBee::getEsp32C6DevKitC1Profile();
    addBoardProfileJson(profiles.add<JsonObject>(), pz,
                        active && active->id == pz.id);
    addBoardProfileJson(profiles.add<JsonObject>(), c6,
                        active && active->id == c6.id);
    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleGetPeripheralResources(
        AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    const String requestedProfile =
        ctx->getParamValue(request, "profileId", "");
    const FastBee::BoardProfile* profile =
        resolveBoardProfile(requestedProfile);
    if (!profile) {
        ctx->sendError(request, 422,
                       "UNSUPPORTED_BOARD_PROFILE: unknown or unavailable profile");
        return;
    }

    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }

    const std::vector<PeripheralConfig> existing =
        PeripheralManager::getInstance().getAllPeripherals();
    FastBee::PeripheralResourcePlanner planner(
        *profile, makePreflightContext(*profile, FastBee::ACK_NONE),
        preflightResult);
    for (const PeripheralConfig& config : existing) {
        planner.addPeripheral(
            config, config.enabled ? FastBee::ClaimState::ACTIVE
                                   : FastBee::ClaimState::DORMANT);
    }

    const FastBee::BoardProfile* active = compiledBoardProfileSupported()
        ? &FastBee::getCompiledBoardProfile() : nullptr;
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["profileId"] = profile->key;
    data["profileName"] = profile->displayName;
    data["profileVersion"] = profile->version;
    data["activeProfileId"] = active ? active->key : "";
    data["comparison"] = active == nullptr || profile->id != active->id;
    data["claimsTruncated"] = preflightResult.claimsTruncated;
    data["issuesTruncated"] = preflightResult.issuesTruncated;
    addResourceRows(data["resources"].to<JsonArray>(), *profile,
                    preflightResult);
    addPreflightIssues(data["diagnostics"].to<JsonArray>(),
                       preflightResult);
    HandlerUtils::sendJsonStream(request, doc);
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
    const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
    FastBee::preflightPeripheralChange(
        profile, existing.data(), existing.size(), candidate, operation,
        excludeId, makePreflightContext(profile, acknowledgements),
        preflightResult);

    JsonDocument doc;
    const bool allowed = !hasHardPreflightError(preflightResult);
    addPreflightResponse(doc, profile, preflightResult, candidate, operation,
                         true, allowed);
    HandlerUtils::sendJsonStream(request, doc);
}

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
        doc["id"] = config.id;
        doc["name"] = config.name;
        doc["type"] = static_cast<int>(config.type);
        doc["typeName"] = getPeripheralTypeName(config.type);
        doc["category"] = getCategoryName(getPeripheralCategory(config.type));
        doc["enabled"] = config.enabled;
        doc["implemented"] = isPeripheralTypeImplemented(config.type);
        addPassiveBuzzerFlag(doc, config);

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
                addUartRuntime(doc.as<JsonObject>(), runtimeState);
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
        itemDoc["id"] = config.id;
        itemDoc["name"] = config.name;
        itemDoc["type"] = static_cast<int>(config.type);
        itemDoc["typeName"] = getPeripheralTypeName(config.type);
        itemDoc["category"] = getCategoryName(getPeripheralCategory(config.type));
        itemDoc["enabled"] = config.enabled;
        itemDoc["implemented"] = isPeripheralTypeImplemented(config.type);
        addPassiveBuzzerFlag(itemDoc, config);

        if (!compact) {
            JsonArray pins = itemDoc["pins"].to<JsonArray>();
            for (int j = 0; j < config.pinCount && j < 8; j++) {
                if (config.pins[j] != 255) {
                    pins.add(config.pins[j]);
                }
            }

            if (config.type == PeripheralType::UART) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addUartParams(params, config);
            } else if (config.type == PeripheralType::ENCODER) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addEncoderParams(params, config);
            } else if (isAnalogInputPeripheral(config.type)) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addAnalogInputParams(params, config, false);
            } else if (isDigitalInputPeripheral(config.type)) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addDigitalInputParams(params, config);
            } else if (config.type == PeripheralType::PWM_SERVO) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addServoParams(params, config);
            } else if (config.type == PeripheralType::SENSOR_GENERIC ||
                       config.type == PeripheralType::SDIO ||
                       config.type == PeripheralType::DS1302_RTC ||
                       config.type == PeripheralType::LCD1602 ||
                       config.type == PeripheralType::RFID_MFRC522) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addP1PeripheralParams(params, config);
            }

            PeripheralRuntimeState runtimeState;
            if (pm.getRuntimeStateSnapshot(config.id, runtimeState)) {
                itemDoc["status"] = static_cast<int>(runtimeState.status);
                itemDoc["memorySuspended"] = runtimeState.memorySuspended;
                if (config.type == PeripheralType::UART) {
                    addUartRuntime(itemDoc.as<JsonObject>(), runtimeState);
                }
                if (config.type == PeripheralType::ENCODER) {
                    JsonObject encoder = itemDoc["reading"].to<JsonObject>();
                    encoder["count"] = runtimeState.state.encoder.count;
                    encoder["delta"] = runtimeState.state.encoder.delta;
                    encoder["direction"] = runtimeState.state.encoder.direction;
                    encoder["frequencyHz"] = runtimeState.state.encoder.frequencyHz;
                    encoder["rpm"] = runtimeState.state.encoder.rpm;
                }
            } else {
                itemDoc["status"] = 0;
                itemDoc["memorySuspended"] = false;
            }
        }

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

void PeripheralRouteHandler::handleGetPeripheralTypes(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    doc["success"] = true;
    JsonObject profile = doc["profile"].to<JsonObject>();
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(PeripheralManager::getInstance().getPeripheralCount());
    profile["name"] = FastBee::ResourceProfile::NAME;
    profile["max"] = profileMax;
    profile["used"] = profileUsed;
    profile["remaining"] = std::max(0, profileMax - profileUsed);
    JsonObject data = doc["data"].to<JsonObject>();
    PeripheralManager& pm = PeripheralManager::getInstance();

    JsonObject hardware = doc["hardware"].to<JsonObject>();
    hardware["chip"] = CHIP_NAME;
    hardware["maxGpio"] = CHIP_MAX_GPIO;
    hardware["pwmChannels"] = CHIP_MAX_PWM_CH;
    hardware["hasDac"] = (bool)CHIP_HAS_DAC;
    hardware["hasTouch"] = (bool)CHIP_HAS_TOUCH;
    JsonArray inputPins = hardware["inputPins"].to<JsonArray>();
    JsonArray outputPins = hardware["outputPins"].to<JsonArray>();
    JsonArray adcPins = hardware["adcPins"].to<JsonArray>();
    JsonArray dacPins = hardware["dacPins"].to<JsonArray>();
    JsonArray touchPins = hardware["touchPins"].to<JsonArray>();
    JsonArray reservedPins = hardware["reservedPins"].to<JsonArray>();
    for (uint8_t pin = 0; pin <= CHIP_MAX_GPIO; ++pin) {
        String pinError;
        if (pm.isReservedPin(pin)) reservedPins.add(pin);
        if (pm.validatePinForType(pin, PeripheralType::GPIO_DIGITAL_INPUT, pinError)) inputPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::GPIO_DIGITAL_OUTPUT, pinError)) outputPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::ADC, pinError)) adcPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::DAC, pinError)) dacPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::GPIO_TOUCH, pinError)) touchPins.add(pin);
    }

    // 通信接口
    JsonArray commTypes = data["communication"].to<JsonArray>();
    for (int i = 1; i <= 5; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = commTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = isPeripheralTypeImplemented(type);
        addPeripheralTypeDescriptorDetails(typeObj, type);
    }

    // GPIO接口
    JsonArray gpioTypes = data["gpio"].to<JsonArray>();
    for (int i = 11; i <= 21; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = gpioTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = isPeripheralTypeImplemented(type);
        addPeripheralTypeDescriptorDetails(typeObj, type);
    }

    // 模拟信号
    JsonArray analogTypes = data["analog"].to<JsonArray>();
    for (int i = 26; i <= 27; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = analogTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = isPeripheralTypeImplemented(type);
        addPeripheralTypeDescriptorDetails(typeObj, type);
    }

    // 调试接口（已隐藏，JTAG/SWD未实现驱动）
    // JsonArray debugTypes = data["debug"].to<JsonArray>();
    // for (int i = 31; i <= 32; i++) {
    //     PeripheralType type = static_cast<PeripheralType>(i);
    //     JsonObject typeObj = debugTypes.add<JsonObject>();
    //     typeObj["value"] = i;
    //     typeObj["name"] = getPeripheralTypeName(type);
    //     typeObj["pinCount"] = getPeripheralPinCount(type);
    // }

    // 专用外设
    JsonArray specialTypes = data["special"].to<JsonArray>();
    for (int i = 36; i <= 45; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = specialTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = isPeripheralTypeImplemented(type);
        addPeripheralTypeDescriptorDetails(typeObj, type);
    }
    const int extraSpecialTypes[] = {47, 48, 49, 50, 51, 52, 53, 54, 60};
    for (int value : extraSpecialTypes) {
        PeripheralType type = static_cast<PeripheralType>(value);
        JsonObject typeObj = specialTypes.add<JsonObject>();
        typeObj["value"] = value;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = isPeripheralTypeImplemented(type);
        addPeripheralTypeDescriptorDetails(typeObj, type);
    }

    HandlerUtils::sendJsonStream(request, doc);
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

    data["id"] = config->id;
    data["name"] = config->name;
    data["type"] = static_cast<int>(config->type);
    data["typeName"] = getPeripheralTypeName(config->type);
    data["enabled"] = config->enabled;
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
        params["frequency"] = config->params.i2c.frequency;
        params["address"] = config->params.i2c.address;
        params["isMaster"] = config->params.i2c.isMaster;
    } else if (config->type == PeripheralType::SPI) {
        params["frequency"] = config->params.spi.frequency;
        params["mode"] = config->params.spi.mode;
        params["msbFirst"] = config->params.spi.msbFirst;
    } else if (config->type == PeripheralType::PWM_SERVO) {
        addServoParams(params, *config);
    } else if (config->isGPIOPeripheral() && !isAnalogInputConfigType(config->type)) {
        params["initialState"] = static_cast<int>(config->params.gpio.initialState);
        params["pwmChannel"] = config->params.gpio.pwmChannel;
        params["pwmFrequency"] = config->params.gpio.pwmFrequency;
        params["pwmResolution"] = config->params.gpio.pwmResolution;
        params["defaultDuty"] = config->params.gpio.defaultDuty;
        if (isDigitalInputPeripheral(config->type)) {
            addDigitalInputParams(params, *config);
        }
    } else if (isAnalogInputConfigType(config->type)) {
        addAnalogInputParams(params, *config);
    } else if (config->type == PeripheralType::DAC) {
        params["channel"] = config->params.dac.channel;
        params["defaultValue"] = config->params.dac.defaultValue;
    } else if (config->type == PeripheralType::STEPPER_MOTOR) {
        params["stepsPerRevolution"] = config->params.stepper.stepsPerRevolution;
        params["speed"] = config->params.stepper.speed;
    } else if (config->type == PeripheralType::LCD) {
        params["width"]     = config->params.lcd.width;
        params["height"]    = config->params.lcd.height;
        params["interface"] = config->params.lcd.interface;
    } else if (config->type == PeripheralType::NEO_PIXEL) {
        params["count"] = config->params.neopixel.count;
        params["brightness"] = config->params.neopixel.brightness;
    } else if (config->type == PeripheralType::RF_MODULE) {
        params["mode"] = config->params.rf.mode;
        params["pulseWidth"] = config->params.rf.pulseWidth;
        params["repeat"] = config->params.rf.repeat;
        params["bitLength"] = config->params.rf.bitLength;
        params["activeHigh"] = config->params.rf.activeHigh;
    } else if (config->type == PeripheralType::RADAR_SENSOR) {
        params["mode"] = config->params.radar.mode;
        params["activeHigh"] = config->params.radar.activeHigh;
        params["debounceMs"] = config->params.radar.debounceMs;
        params["holdMs"] = config->params.radar.holdMs;
    } else if (config->type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        params["brightness"] = config->params.segment.brightness;
    } else if (config->type == PeripheralType::SENSOR_GENERIC ||
               config->type == PeripheralType::SDIO ||
               config->type == PeripheralType::DS1302_RTC ||
               config->type == PeripheralType::LCD1602 ||
               config->type == PeripheralType::RFID_MFRC522) {
        addP1PeripheralParams(params, *config);
    }

    PeripheralRuntimeState runtimeSnapshot;
    const PeripheralRuntimeState* runtimeState =
        pm.getRuntimeStateSnapshot(id, runtimeSnapshot) ? &runtimeSnapshot : nullptr;
    if (runtimeState) {
        JsonObject status = data["status"].to<JsonObject>();
        status["state"] = static_cast<int>(runtimeState->status);
        status["initTime"] = runtimeState->initTime;
        status["lastActivity"] = runtimeState->lastActivity;
        status["errorCount"] = runtimeState->errorCount;
        status["memorySuspended"] = runtimeState->memorySuspended;
        if (config->type == PeripheralType::UART) {
            status["framing"] = uartFramingName(*config);
            addUartRuntime(status, *runtimeState);
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

    if (!isPeripheralTypeImplemented(config.type)) {
        ctx->sendError(request, 422, "UNSUPPORTED_PERIPHERAL_TYPE: 当前固件未实现该外设类型");
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
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ADD, "",
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK),
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

    if (!isPeripheralTypeImplemented(config.type)) {
        ctx->sendError(request, 422, "UNSUPPORTED_PERIPHERAL_TYPE: 当前固件未实现该外设类型");
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
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::UPDATE, id,
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK),
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
    String errorMsg;
    if (pm.removePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral deleted");
    } else {
        ctx->sendError(request, errorMsg.startsWith("PERSISTENCE_") ? 500 : 404,
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
    if (!isPeripheralTypeImplemented(config.type)) {
        ctx->sendError(request, 422,
                       "UNSUPPORTED_PERIPHERAL_TYPE: current firmware does not implement this type");
        return;
    }
    config.enabled = true;
    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ENABLE, id,
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::ENABLE)) {
            return;
        }
    }
    String errorMsg;
    if (pm.enablePeripheralAndSave(id, errorMsg)) {
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

    data["id"] = id;
    data["enabled"] = config.enabled;
    data["status"] = hasRuntimeState ? static_cast<int>(runtimeState.status) : 0;
    data["memorySuspended"] = hasRuntimeState && runtimeState.memorySuspended;

    if (hasRuntimeState) {
        data["initTime"] = runtimeState.initTime;
        data["lastActivity"] = runtimeState.lastActivity;
        data["errorCount"] = runtimeState.errorCount;
        if (!runtimeState.lastError.isEmpty()) {
            data["lastError"] = runtimeState.lastError;
        }
        if (config.type == PeripheralType::PWM_SERVO) {
            data["angle"] = runtimeState.state.servo.currentAngle;
            data["pulseUs"] = runtimeState.state.servo.currentPulseUs;
        } else if (config.type == PeripheralType::UART) {
            data["framing"] = uartFramingName(config);
            addUartRuntime(data, runtimeState);
        }
    }
    if (hasEncoderReading) addEncoderReading(data, encoderReading);

    if (config.type == PeripheralType::SDIO) {
        const SDCardStatus status = SDCardManager::getInstance().getStatus();
        data["configured"] = status.configured && status.ownerId == id;
        data["mounted"] = status.mounted && status.ownerId == id;
        data["cardType"] = status.cardType;
        data["cardBytes"] = status.cardBytes;
        data["totalBytes"] = status.totalBytes;
        data["usedBytes"] = status.usedBytes;
        if (!status.lastError.isEmpty()) data["driverError"] = status.lastError;
    } else if (config.type == PeripheralType::DS1302_RTC) {
#if FASTBEE_ENABLE_DS1302
        data["initialized"] = DS1302Driver::getInstance().isInitialized();
        data["running"] = DS1302Driver::getInstance().isRunning();
#else
        data["initialized"] = false;
#endif
    } else if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        data["initialized"] = LCD1602Driver::getInstance().isInitialized();
        data["cols"] = LCD1602Driver::getInstance().getCols();
        data["rows"] = LCD1602Driver::getInstance().getRows();
        data["backlight"] = LCD1602Driver::getInstance().isBacklightOn();
#else
        data["initialized"] = false;
#endif
    } else if (config.type == PeripheralType::RFID_MFRC522) {
#if FASTBEE_ENABLE_RFID
        data["initialized"] = RFIDDriver::getInstance().isInitialized();
        data["cardPresent"] = RFIDDriver::getInstance().isCardPresent();
        data["uid"] = RFIDDriver::getInstance().getLastUID();
        data["cardType"] = RFIDDriver::getInstance().getLastCardType();
        if (!RFIDDriver::getInstance().getLastError().isEmpty()) {
            data["driverError"] = RFIDDriver::getInstance().getLastError();
        }
#else
        data["initialized"] = false;
#endif
    } else if (config.type == PeripheralType::SENSOR_GENERIC &&
               config.params.sensor.driver[0] != '\0') {
        data["driver"] = config.params.sensor.driver;
        data["address"] = config.params.sensor.address;
        data["mode"] = config.params.sensor.mode;
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleReadPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

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

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    data["id"] = id;
    data["type"] = static_cast<int>(config.type);
    data["typeName"] = getPeripheralTypeName(config.type);

    const String requestedAction = ctx->getParamValue(request, "action", "");
    const bool localI2cSensor =
        config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] != '\0';

    if ((config.type == PeripheralType::I2C || localI2cSensor) &&
        requestedAction.equalsIgnoreCase("scan")) {
        if (!config.enabled || config.pinCount < 2) {
            ctx->sendError(request, 409,
                           "I2C_NOT_READY: enable an I2C peripheral with SDA/SCL first");
            return;
        }
        JsonArray addresses = data["addresses"].to<JsonArray>();
        for (uint8_t address = 1; address < 0x7F; ++address) {
            Wire.beginTransmission(address);
            if (Wire.endTransmission() == 0) addresses.add(address);
        }
        data["count"] = addresses.size();
        data["sda"] = config.pins[0];
        data["scl"] = config.pins[1];
    } else if (config.type == PeripheralType::SDIO) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "SD_NOT_READY: enable and mount the SD peripheral first");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "status");
        String errorMsg;
        if (action.equalsIgnoreCase("status")) {
            const SDCardStatus status = SDCardManager::getInstance().getStatus();
            data["mounted"] = status.mounted && status.ownerId == id;
            data["cardType"] = status.cardType;
            data["cardBytes"] = status.cardBytes;
            data["totalBytes"] = status.totalBytes;
            data["usedBytes"] = status.usedBytes;
        } else if (action.equalsIgnoreCase("list")) {
            std::vector<SDCardEntry> entries;
            const String path = ctx->getParamValue(request, "path", "/fastbee");
            if (!SDCardManager::getInstance().list(path, entries, errorMsg)) {
                ctx->sendError(request, 400, errorMsg);
                return;
            }
            JsonArray items = data["entries"].to<JsonArray>();
            for (const SDCardEntry& entry : entries) {
                JsonObject item = items.add<JsonObject>();
                item["path"] = entry.path;
                item["directory"] = entry.directory;
                item["size"] = entry.size;
            }
            data["path"] = path;
            data["count"] = entries.size();
            data["truncated"] = entries.size() >= SDCardManager::MAX_LIST_ENTRIES;
        } else if (action.equalsIgnoreCase("read")) {
            const String path = ctx->getParamValue(request, "path", "");
            int requestedMax = ctx->getParamInt(request, "maxBytes", 4096);
            if (requestedMax < 1) requestedMax = 1;
            if (requestedMax > 4096) requestedMax = 4096;
            String content;
            if (!SDCardManager::getInstance().readFile(
                    path, content, errorMsg, static_cast<size_t>(requestedMax))) {
                ctx->sendError(request, 400, errorMsg);
                return;
            }
            data["path"] = path;
            data["content"] = content;
            data["length"] = content.length();
        } else {
            ctx->sendError(request, 400, "SD_ACTION_INVALID: expected status, list or read");
            return;
        }
    } else if (config.type == PeripheralType::DS1302_RTC) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "DS1302_NOT_READY: enable the RTC first");
            return;
        }
#if FASTBEE_ENABLE_DS1302
        DS1302Driver& rtc = DS1302Driver::getInstance();
        if (!rtc.isInitialized()) {
            ctx->sendError(request, 409, "DS1302_NOT_INITIALIZED");
            return;
        }
        const DS1302DateTime dt = rtc.readDateTime();
        if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
            dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
            ctx->sendError(request, 502, "DS1302_INVALID_DATA: clock returned an invalid date/time");
            return;
        }
        data["year"] = 2000 + dt.year;
        data["month"] = dt.month;
        data["day"] = dt.day;
        data["hour"] = dt.hour;
        data["minute"] = dt.minute;
        data["second"] = dt.second;
        data["dayOfWeek"] = dt.dayOfWeek;
        data["formatted"] = dt.toString();
        data["running"] = rtc.isRunning();
        data["use24Hour"] = rtc.is24HourMode();
#else
        ctx->sendError(request, 501, "DS1302_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        data["initialized"] = LCD1602Driver::getInstance().isInitialized();
        data["cols"] = LCD1602Driver::getInstance().getCols();
        data["rows"] = LCD1602Driver::getInstance().getRows();
#else
        data["initialized"] = false;
#endif
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "RFID_NOT_READY: enable the reader first");
            return;
        }
#if FASTBEE_ENABLE_RFID
        String uid;
        String cardType;
        String errorMsg;
        if (!RFIDDriver::getInstance().scanNow(uid, cardType, errorMsg)) {
            ctx->sendError(request, 409, errorMsg);
            return;
        }
        data["uid"] = uid;
        data["cardType"] = cardType;
        data["cardPresent"] = true;
#else
        ctx->sendError(request, 501, "RFID_NOT_COMPILED");
        return;
#endif
    } else if (localI2cSensor) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "SENSOR_NOT_READY: enable the sensor first");
            return;
        }
        if (!requestedAction.isEmpty() &&
            !requestedAction.equalsIgnoreCase("read") &&
            !requestedAction.equalsIgnoreCase("probe")) {
            ctx->sendError(request, 400,
                           "SENSOR_ACTION_INVALID: expected scan, probe or read");
            return;
        }
        String channel = ctx->getParamValue(request, "channel", "");
        if (channel == "accel_x") channel = "accelX";
        else if (channel == "accel_y") channel = "accelY";
        else if (channel == "accel_z") channel = "accelZ";
        else if (channel == "gyro_x") channel = "gyroX";
        else if (channel == "gyro_y") channel = "gyroY";
        else if (channel == "gyro_z") channel = "gyroZ";
        if (channel.isEmpty()) {
            channel = String(config.params.sensor.driver).equalsIgnoreCase("MPU6050")
                ? (config.params.sensor.mode == 1 ? "gyroX" : "accelX")
                : "temperature";
        }
        JsonDocument driverDoc;
        driverDoc["addr"] = config.params.sensor.address;
        if (String(config.params.sensor.driver).equalsIgnoreCase("MPU6050") &&
            config.params.sensor.mode == 1) {
            driverDoc["mode"] = "gyro";
        }
        String driverParams;
        serializeJson(driverDoc, driverParams);
        float value = 0.0f;
        String unit;
        String errorMsg;
        if (!FastBee::SensorDriverCache::instance().readChannel(
                id, config.params.sensor.driver, config.getPrimaryPin(),
                driverParams.c_str(), channel.c_str(), value, unit, errorMsg)) {
            ctx->sendError(request, 502, errorMsg);
            return;
        }
        data["driver"] = config.params.sensor.driver;
        data["channel"] = channel;
        data["value"] = value;
        data["unit"] = unit;
        data["timestamp"] = millis();
        data["probed"] = requestedAction.equalsIgnoreCase("probe");
    } else if (config.type == PeripheralType::ENCODER) {
        EncoderReading reading;
        if (!pm.getEncoderReading(id, reading)) {
            ctx->sendError(request, 409, "ENCODER_NOT_READY: enable and initialize the encoder first");
            return;
        }
        addEncoderReading(data, reading);
        data["value"] = reading.count;
        data["unit"] = "count";
    } else if (config.type == PeripheralType::PWM_SERVO) {
        PeripheralRuntimeState runtimeState;
        if (pm.getRuntimeStateSnapshot(id, runtimeState)) {
            data["angle"] = runtimeState.state.servo.currentAngle;
            data["pulseUs"] = runtimeState.state.servo.currentPulseUs;
            data["value"] = runtimeState.state.servo.currentAngle;
        } else {
            data["angle"] = config.params.servo.initialAngle;
            data["pulseUs"] = servoAngleToPulseUs(
                config.params.servo.initialAngle,
                config.params.servo.minAngle, config.params.servo.maxAngle,
                config.params.servo.minPulse, config.params.servo.maxPulse,
                config.params.servo.inverted);
            data["value"] = config.params.servo.initialAngle;
        }
        data["unit"] = "deg";
    } else if (isAnalogInputConfigType(config.type)) {
        // AI 读取同时返回原始 ADC 计数和工程量，便于调试接线/量程；
        // conversionEnabled 关闭时 value 与 rawValue 保持一致，兼容旧客户端。
        const uint16_t rawValue = pm.readAnalog(id);
        data["rawValue"] = rawValue;
        data["conversionEnabled"] = config.params.adc.conversionEnabled;
        data["signalMode"] = config.params.adc.signalMode;
        // Reuse the same ADC sample for rawValue and engineering conversion;
        // taking a second sample can make the two values disagree and doubles
        // the cost of filtered reads (up to 16 analogRead calls).
        AnalogInputQuality quality = AnalogInputQuality::INVALID_CONFIG;
        const float converted = pm.convertAnalogRawValue(id, rawValue, &quality);
        data["quality"] = analogInputQualityName(quality);
        data["qualityCode"] = static_cast<uint8_t>(quality);
        if (isnan(converted) || isinf(converted)) {
            data["value"] = nullptr;
            data["valueValid"] = false;
        } else {
            data["value"] = converted;
            data["valueValid"] = true;
        }
    } else if (config.isGPIOPeripheral()) {
        GPIOState state = pm.readPin(id);
        data["state"] = static_cast<int>(state);
        data["stateName"] = (state == GPIOState::STATE_HIGH) ? "HIGH" : (state == GPIOState::STATE_LOW) ? "LOW" : "UNDEFINED";
    } else if (config.isCommunicationPeripheral()) {
        uint8_t buffer[UART_API_MAX_BYTES];
        size_t len = sizeof(buffer);

        if (pm.readData(id, buffer, len) && len > 0) {
            bool isText = true;
            for (size_t i = 0; i < len; i++) {
                const bool printableAscii = buffer[i] >= 32 && buffer[i] <= 126;
                if (!printableAscii && buffer[i] != '\n' && buffer[i] != '\r' && buffer[i] != '\t') {
                    isText = false;
                    break;
                }
            }

            if (isText) {
                data["text"] = String((char*)buffer, len);
            }

            String hex;
            hex.reserve(len * 3);
            for (size_t i = 0; i < len; i++) {
                char hexBuf[4];
                snprintf(hexBuf, sizeof(hexBuf), i + 1 < len ? "%02X " : "%02X", buffer[i]);
                hex += hexBuf;
            }
            data["hex"] = hex;
            data["length"] = len;
        } else {
            data["text"] = "";
            data["hex"] = "";
            data["length"] = 0;
        }
        if (config.type == PeripheralType::UART) {
            data["framing"] = uartFramingName(config);
            PeripheralRuntimeState runtimeState;
            if (pm.getRuntimeStateSnapshot(id, runtimeState)) {
                addUartRuntime(data, runtimeState);
            }
        }
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        bool detected = false;
        if (pm.readRadarState(id, detected)) {
            data["detected"] = detected;
            data["value"] = detected ? 1 : 0;
            data["stateName"] = detected ? "DETECTED" : "CLEAR";
        } else {
            data["detected"] = nullptr;
            data["value"] = nullptr;
        }
    } else if (config.type == PeripheralType::RF_MODULE) {
        bool level = false;
        if (pm.readRfLevel(id, level)) {
            data["level"] = level;
            data["value"] = level ? 1 : 0;
            data["stateName"] = level ? "ACTIVE" : "IDLE";
        } else {
            data["level"] = nullptr;
            data["value"] = nullptr;
        }
    } else if (config.type == PeripheralType::DAC) {
        data["channel"] = config.params.dac.channel;
        data["pin"] = config.getPrimaryPin();
    } else {
        uint8_t buffer[64];
        size_t len = sizeof(buffer);

        if (pm.readData(id, buffer, len) && len > 0) {
            data["length"] = len;
            if (len <= 4) {
                uint32_t value = 0;
                for (size_t i = 0; i < len; i++) {
                    value |= ((uint32_t)buffer[i]) << (i * 8);
                }
                data["value"] = value;
            }
        } else {
            data["value"] = nullptr;
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
    if (!isPeripheralTypeImplemented(type)) {
        ctx->sendError(request, 422, "UNSUPPORTED_PERIPHERAL_TYPE: 当前固件未实现该外设类型");
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
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    bool success = false;
    String message;

    if (config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] != '\0' &&
        ctx->getParamValue(request, "action", "").equalsIgnoreCase("release")) {
        FastBee::SensorDriverCache::instance().release(id);
        success = true;
        message = "Sensor driver cache released";
    } else if (config.type == PeripheralType::SDIO) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "SD_NOT_READY: enable the SD peripheral first");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "");
        String errorMsg;
        if (action.equalsIgnoreCase("mount")) {
            success = SDCardManager::getInstance().mount(errorMsg);
            message = success ? "SD card mounted" : errorMsg;
        } else if (action.equalsIgnoreCase("unmount")) {
            success = SDCardManager::getInstance().unmount(id, &errorMsg, false);
            message = success ? "SD card unmounted" : errorMsg;
        } else if (action.equalsIgnoreCase("write") ||
                   action.equalsIgnoreCase("write-file")) {
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
                content.length(), append, errorMsg);
            message = success ? String("SD file written: ") +
                                    String(content.length()) + " bytes"
                              : errorMsg;
        } else if (action.equalsIgnoreCase("delete") ||
                   action.equalsIgnoreCase("delete-file")) {
            const String path = ctx->getParamValue(request, "path", "");
            const String confirmation = ctx->getParamValue(request, "confirm", "");
            if (confirmation != "DELETE") {
                ctx->sendError(request, 400,
                               "SD_DELETE_CONFIRMATION_REQUIRED: use confirm=DELETE");
                return;
            }
            success = SDCardManager::getInstance().removeFile(path, errorMsg);
            message = success ? "SD path deleted" : errorMsg;
        } else {
            ctx->sendError(
                request, 400,
                "SD_ACTION_INVALID: expected mount, unmount, write-file or delete-file");
            return;
        }
    } else if (config.type == PeripheralType::DS1302_RTC) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "DS1302_NOT_READY: enable the RTC first");
            return;
        }
#if FASTBEE_ENABLE_DS1302
        DS1302Driver& rtc = DS1302Driver::getInstance();
        if (!rtc.isInitialized()) {
            ctx->sendError(request, 409, "DS1302_NOT_INITIALIZED");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "");
        if (action.equalsIgnoreCase("setDateTime") ||
            action.equalsIgnoreCase("set-datetime")) {
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
        } else if (action.equalsIgnoreCase("setRunning") ||
                   action.equalsIgnoreCase("set-running")) {
            if (!request->hasParam("running", true)) {
                ctx->sendError(request, 400, "DS1302_RUNNING_REQUIRED");
                return;
            }
            const bool running = ctx->getParamBool(request, "running", true);
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
        if (action.equalsIgnoreCase("print")) {
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
            lcd.setCursor(static_cast<uint8_t>(col), static_cast<uint8_t>(row));
            success = lcd.print(content);
            message = success ? "LCD1602 text printed" : "LCD1602_PRINT_FAILED";
        } else if (action.equalsIgnoreCase("clear")) {
            lcd.clear();
            success = true;
            message = "LCD1602 cleared";
        } else if (action.equalsIgnoreCase("backlight")) {
            if (!request->hasParam("enabled", true)) {
                ctx->sendError(request, 400,
                               "LCD1602_BACKLIGHT_VALUE_REQUIRED");
                return;
            }
            lcd.setBacklight(ctx->getParamBool(request, "enabled", true));
            success = true;
            message = "LCD1602 backlight updated";
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

        if (action.equalsIgnoreCase("readBlock") ||
            action.equalsIgnoreCase("read-block")) {
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
            payload["id"] = id;
            payload["block"] = blockValue;
            payload["hex"] = bytesToHex(blockData, sizeof(blockData));
            payload["uid"] = reader.getLastUID();
            payload["cardType"] = reader.getLastCardType();
            memset(blockData, 0, sizeof(blockData));
            HandlerUtils::sendJsonStream(request, response);
            return;
        }

        if (action.equalsIgnoreCase("writeBlock") ||
            action.equalsIgnoreCase("write-block")) {
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
                                    action.equalsIgnoreCase("reset");
        if (!resetRequested) {
            ctx->sendError(request, 400, "ENCODER_READ_ONLY: use action=reset or reset=1");
            return;
        }
        success = pm.resetEncoder(id);
        message = success ? "Encoder reset" : "Encoder reset failed";
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (request->hasParam("angle", true)) {
            const int angle = ctx->getParamInt(request, "angle", config.params.servo.initialAngle);
            success = pm.writeServoAngle(id, angle);
            message = success ? "Servo angle set" : "Servo angle write failed";
        } else if (request->hasParam("pwm", true)) {
            int rawDuty = ctx->getParamInt(request, "pwm", 0);
            if (rawDuty < 0) rawDuty = 0;
            success = pm.writePWM(id, static_cast<uint32_t>(rawDuty));
            message = success ? "Servo raw PWM duty set" : "Servo raw PWM write failed";
        } else {
            ctx->sendError(request, 400, "Missing parameter (angle/pwm)");
            return;
        }
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
            int value = ctx->getParamInt(request, "value", 0);
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            uint8_t dacValue = (uint8_t)value;
            success = pm.writeData(id, &dacValue, 1);
            message = success ? "DAC value set" : "DAC write failed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    } else if (config.type == PeripheralType::RF_MODULE) {
        String code = ctx->getParamValue(request, "code", "");
        if (code.isEmpty()) code = ctx->getParamValue(request, "data", "");
        if (code.isEmpty()) code = ctx->getParamValue(request, "value", "");
        if (code.isEmpty()) {
            ctx->sendError(request, 400, "Missing code parameter");
            return;
        }
        int bits = ctx->getParamInt(request, "bits", 0);
        int pulseWidth = ctx->getParamInt(request, "pulseWidth", 0);
        int repeat = ctx->getParamInt(request, "repeat", 0);
        if (bits < 0) bits = 0;
        if (bits > 32) bits = 32;
        if (pulseWidth < 0) pulseWidth = 0;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 0) repeat = 0;
        if (repeat > 20) repeat = 20;
        success = pm.sendRfCode(id, code, (uint8_t)bits, (uint16_t)pulseWidth, (uint8_t)repeat);
        message = success ? "RF code sent" : "RF send failed";
    } else if (config.isCommunicationPeripheral()) {
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
                    success = pm.writeData(id, bytes, byteCount);
                    message = success ? String("Sent ") + String(byteCount) + " bytes (HEX)" : "Send failed";
                } else {
                    if (config.type == PeripheralType::UART &&
                        dataStr.length() > UART_API_MAX_BYTES) {
                        ctx->sendError(request, 413,
                            String("PAYLOAD_TOO_LARGE: maximum ") +
                            String(UART_API_MAX_BYTES) + " bytes");
                        return;
                    }
                    success = pm.writeData(id, (const uint8_t*)dataStr.c_str(), dataStr.length());
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

    if (success) {
        ctx->sendSuccess(request, message);
    } else {
        ctx->sendError(request, 400, message);
    }
}
