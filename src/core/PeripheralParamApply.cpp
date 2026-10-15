#include "core/PeripheralParamApply.h"
#include "core/ChipConfig.h"
#include "core/H1ParameterContract.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/SensorConfigSupport.h"
#include "core/RtcBootSeedConfig.h"
#include "core/PeripheralRouteMapper.h"
#include "PeripheralManagerConfigHelpers.h"

namespace FastBee {
namespace {
using FastBee::PeripheralManagerConfigDetail::initializeAdcDefaults;
using FastBee::PeripheralManagerConfigDetail::isAdcInputType;
using FastBee::PeripheralManagerConfigDetail::normalizeAdcFilterMode;
using FastBee::PeripheralManagerConfigDetail::normalizeAdcSampleCount;
} // namespace

bool applyServoParams(const PeripheralFormFields& form, PeripheralConfig& config,
                      bool resetDefaults, String& errorMsg) {
    if (resetDefaults) initializeServoDefaults(config);
    const auto& current = config.params.servo;
    const int frequency = form.intValue("frequency", current.frequency);
    const int minPulse = form.intValue("minPulse", current.minPulse);
    const int maxPulse = form.intValue("maxPulse", current.maxPulse);
    const int minAngle = form.intValue("minAngle", current.minAngle);
    const int maxAngle = form.intValue("maxAngle", current.maxAngle);
    const int initialAngle = form.intValue("initialAngle", current.initialAngle);

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
    config.params.servo.inverted = form.boolValue("inverted", current.inverted);
    config.params.servo.releaseAfterMove = form.boolValue(
        "releaseAfterMove", current.releaseAfterMove);
    return true;
}

bool applyP1PeripheralParams(const PeripheralFormFields& form,
                             PeripheralConfig& config, bool resetDefaults,
                             String& errorMsg) {
    if (config.type == PeripheralType::SDIO) {
        if (resetDefaults) initializeSdCardDefaults(config);
        config.params.sdcard.interface = static_cast<uint8_t>(
            form.intValue("interface", config.params.sdcard.interface));
        const int frequency = form.intValue(
            "frequency", static_cast<int>(config.params.sdcard.frequency));
        if (config.params.sdcard.interface != SDCARD_INTERFACE_SPI ||
            frequency < 100000 || frequency > 40000000) {
            errorMsg = "INVALID_SD_CONFIG: 仅支持 100kHz-40MHz 的 SDSPI";
            return false;
        }
        config.params.sdcard.frequency = static_cast<uint32_t>(frequency);
    } else if (config.type == PeripheralType::DS1302_RTC) {
        if (resetDefaults) initializeDs1302Defaults(config);
        if (!FastBee::parseRtcBootSeedForm(
            form.has("bootSeed"),
            form.value("bootSeed", ""),
            form.has("utcOffsetMinutes"),
            form.value("utcOffsetMinutes", ""),
            config.params.ds1302.bootSeed, config.params.ds1302.utcOffsetMinutes, errorMsg)) return false;
        config.params.ds1302.use24Hour = form.boolValue(
            "use24Hour", config.params.ds1302.use24Hour);
        const int charge = form.intValue(
            "trickleCharge", config.params.ds1302.trickleCharge);
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
        const int cols = form.intValue("cols", config.params.lcd1602.cols);
        const int rows = form.intValue("rows", config.params.lcd1602.rows);
        const int address = form.intValue("address", config.params.lcd1602.address);
        if (!((cols == 16 && rows == 2) || (cols == 20 && rows == 4)) ||
            (address != 0 && !((address >= 0x20 && address <= 0x27) ||
                               (address >= 0x38 && address <= 0x3F)))) {
            errorMsg = "INVALID_LCD1602_CONFIG: 尺寸或 I2C 地址无效";
            return false;
        }
        config.params.lcd1602.cols = static_cast<uint8_t>(cols);
        config.params.lcd1602.rows = static_cast<uint8_t>(rows);
        config.params.lcd1602.address = static_cast<uint8_t>(address);
        config.params.lcd1602.backlight = form.boolValue(
            "backlight", config.params.lcd1602.backlight);
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
            if (form.has(key)) {
                sensorParams[key] = form.value(key, "");
            }
        }
        if (sensorParams.overflowed()) { errorMsg = "SENSOR_PARAMS_MEMORY_UNAVAILABLE"; return false; }
        if (!FastBee::parseSensorConfig(sensorParams.as<JsonObjectConst>(), config, errorMsg) ||
            !FastBee::validateSensorConfig(config, errorMsg)) return false;
    }
    return true;
}

float getRequestFloat(const PeripheralFormFields& form, const char* name,
                      float fallback) {
    String raw = form.value(name, "");
    if (raw.isEmpty()) return fallback;
    float value = raw.toFloat();
    return (isnan(value) || isinf(value)) ? fallback : value;
}

int clampParamInt(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

void applyDigitalInputParams(const PeripheralFormFields& form, PeripheralConfig& config) {
    int inputMode = form.intValue("inputMode", config.params.gpio.inputMode);
    if (inputMode < static_cast<int>(DigitalInputMode::AUTO) ||
        inputMode > static_cast<int>(DigitalInputMode::PULL_DOWN)) {
        inputMode = static_cast<int>(DigitalInputMode::AUTO);
    }
    config.params.gpio.inputMode = static_cast<uint8_t>(inputMode);
    config.params.gpio.invertLogic = form.boolValue("invertLogic",
                                                        config.params.gpio.invertLogic);
    int debounceMs = form.intValue("debounceMs", config.params.gpio.debounceMs);
    if (debounceMs < 0) debounceMs = 0;
    if (debounceMs > 5000) debounceMs = 5000;
    config.params.gpio.debounceMs = static_cast<uint16_t>(debounceMs);
    int reportMode = form.intValue("reportMode", config.params.gpio.reportMode);
    if (reportMode < static_cast<int>(DigitalReportMode::ON_CHANGE) ||
        reportMode > static_cast<int>(DigitalReportMode::EVENT_ONLY)) {
        reportMode = static_cast<int>(DigitalReportMode::ON_CHANGE);
    }
    config.params.gpio.reportMode = static_cast<uint8_t>(reportMode);
    const String peerId = form.value("pairedInputId", config.params.gpio.pairedInputId);
    if (peerId.length() >= sizeof(config.params.gpio.pairedInputId)) {
        // Keep invalid input detectable by validateConfig; never silently target
        // a different peripheral by truncating the requested ID.
        memset(config.params.gpio.pairedInputId, '?', sizeof(config.params.gpio.pairedInputId));
    } else {
        peerId.toCharArray(config.params.gpio.pairedInputId, sizeof(config.params.gpio.pairedInputId));
    }
    const int pairMs = form.intValue("pairDebounceMs", config.params.gpio.pairDebounceMs);
    config.params.gpio.pairDebounceMs = (pairMs == 0 || (pairMs >= 20 && pairMs <= 5000))
        ? static_cast<uint16_t>(pairMs) : UINT16_MAX;
}

// GPIO_ANALOG_INPUT is a legacy AI spelling, but it uses the same ADC
// parameter union as the dedicated ADC type. Keep it out of the generic GPIO
// branch so its resolution/filter settings survive API round trips.
void applyAnalogInputParams(const PeripheralFormFields& form, PeripheralConfig& config,
                            bool resetDefaults) {
    if (resetDefaults) initializeAdcDefaults(config);
    auto& adc = config.params.adc;
    const int attenuationDefault = resetDefaults ? 3 : adc.attenuation;
    const int resolutionDefault = (resetDefaults || adc.resolution < 9 || adc.resolution > 12)
        ? 12 : adc.resolution;
    adc.attenuation = static_cast<uint8_t>(clampParamInt(
        form.intValue("attenuation", attenuationDefault), 0, 3));
    adc.resolution = static_cast<uint8_t>(clampParamInt(
        form.intValue("resolution", resolutionDefault), 9, 12));
    adc.sampleRate = static_cast<uint16_t>(clampParamInt(
        form.intValue("sampleRate", adc.sampleRate), 0, 65535));
    adc.sampleCount = normalizeAdcSampleCount(
        form.intValue("sampleCount", adc.sampleCount ? adc.sampleCount : 1));
    adc.filterMode = normalizeAdcFilterMode(
        form.intValue("filterMode", adc.filterMode));

    int signalMode = clampParamInt(form.intValue(
        "signalMode", adc.signalMode),
        static_cast<int>(AnalogSignalMode::RAW),
        static_cast<int>(AnalogSignalMode::CUSTOM_LINEAR));
    adc.signalMode = static_cast<uint8_t>(signalMode);
    adc.conversionEnabled = form.boolValue("conversionEnabled", adc.conversionEnabled);
    adc.clampEnabled = form.boolValue("clampEnabled", adc.clampEnabled);
    adc.calibratedVoltage = form.boolValue("calibratedVoltage", adc.calibratedVoltage);

    int decimalPlaces = form.intValue("decimalPlaces", adc.decimalPlaces);
    if (decimalPlaces < 0) decimalPlaces = 0;
    if (decimalPlaces > 6) decimalPlaces = 6;
    adc.decimalPlaces = static_cast<uint8_t>(decimalPlaces);

    adc.inputMin = getRequestFloat(form, "inputMin", adc.inputMin);
    adc.inputMax = getRequestFloat(form, "inputMax", adc.inputMax);
    adc.engineeringMin = getRequestFloat(form, "engineeringMin", adc.engineeringMin);
    adc.engineeringMax = getRequestFloat(form, "engineeringMax",
                                         adc.engineeringMax == 0.0f ? 100.0f : adc.engineeringMax);
    adc.engineeringOffset = getRequestFloat(form, "engineeringOffset", adc.engineeringOffset);
    adc.vRef = getRequestFloat(form, "vRef", adc.vRef);
    adc.voltageRatio = getRequestFloat(form, "voltageRatio", adc.voltageRatio);
    adc.shuntResistance = getRequestFloat(form, "shuntResistance", adc.shuntResistance);
    adc.inputGain = getRequestFloat(form, "inputGain", adc.inputGain);
    adc.inputOffset = getRequestFloat(form, "inputOffset", adc.inputOffset);

    adc.rawMin = static_cast<uint16_t>(clampParamInt(
        form.intValue("rawMin", adc.rawMin), 0, 65535));
    adc.rawMax = static_cast<uint16_t>(clampParamInt(
        form.intValue("rawMax", adc.rawMax), 0, 65535));
}

bool readDisplayRequestInteger(const PeripheralFormFields& form,
                               const char* name, int32_t fallback,
                               int32_t minimum, int32_t maximum,
                               int32_t& value, String& error) {
    value = fallback;
    if (!form.has(name)) {
        if (value >= minimum && value <= maximum) return true;
        error = String("DISPLAY_PARAMS_INVALID: invalid ") + name;
        return false;
    }

    String text = form.value(name, "");
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

bool applyDisplayRequestParameters(const PeripheralFormFields& form,
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
    String backend = form.value(
        "displayBackend", params.backend);
    String driver = form.value(
        "displayDriver", params.driver);
    String touch = form.value(
        "touchBackend", params.touchBackend);
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
    if (!readDisplayRequestInteger(form, "width", params.width,
            rgb ? 480 : 128, rgb ? 480 : 320, width, error) ||
        !readDisplayRequestInteger(form, "height", params.height,
            rgb ? 480 : 128, rgb ? 480 : 320, height, error) ||
        !readDisplayRequestInteger(form, "rotation", params.rotation,
            0, 3, rotation, error) ||
        !readDisplayRequestInteger(form, "colorDepth", params.colorDepth,
            16, 16, colorDepth, error) ||
        !readDisplayRequestInteger(form, "bufferLines", params.bufferLines,
            1, 64, bufferLines, error) ||
        !readDisplayRequestInteger(form, "brightness", params.brightness,
            0, 100, brightness, error) ||
        !readDisplayRequestInteger(form, "backlightChannel", params.backlightChannel,
            0, CHIP_MAX_PWM_CH > 0 ? CHIP_MAX_PWM_CH - 1 : 0, backlightChannel, error) ||
        !readDisplayRequestInteger(form, "spiFrequency",
            static_cast<int32_t>(params.spiFrequency), 1000000, 80000000,
            spiFrequency, error) ||
        !readDisplayRequestInteger(form, "i2cFrequency",
            static_cast<int32_t>(params.i2cFrequency), 100000, 400000,
            i2cFrequency, error) ||
        !readDisplayRequestInteger(form, "mosi", params.mosi,
            -1, 127, mosi, error) ||
        !readDisplayRequestInteger(form, "sclk", params.sclk,
            -1, 127, sclk, error) ||
        !readDisplayRequestInteger(form, "cs", params.cs,
            -1, 127, cs, error) ||
        !readDisplayRequestInteger(form, "dc", params.dc,
            -1, 127, dc, error) ||
        !readDisplayRequestInteger(form, "rst", params.rst,
            -1, 127, rst, error) ||
        !readDisplayRequestInteger(form, "backlight", params.backlight,
            -1, 127, backlight, error) ||
        !readDisplayRequestInteger(form, "touchSda", params.touchSda,
            -1, 127, touchSda, error) ||
        !readDisplayRequestInteger(form, "touchScl", params.touchScl,
            -1, 127, touchScl, error) ||
        !readDisplayRequestInteger(form, "touchInt", params.touchInt,
            -1, 127, touchInt, error) ||
        !readDisplayRequestInteger(form, "touchRst", params.touchRst,
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
    params.backlightActiveHigh = form.boolValue(
        "backlightActiveHigh", params.backlightActiveHigh);
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
    String control = form.value("controlBackend", params.controlBackend);
    String profile = form.value("rgbInitProfile", params.rgbInitProfile);
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
    if (!readDisplayRequestInteger(form, #field, params.field, minimum, maximum, extra, error)) return false; \
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
    params.hsyncIdleLow = form.boolValue("hsyncIdleLow", params.hsyncIdleLow);
    params.vsyncIdleLow = form.boolValue("vsyncIdleLow", params.vsyncIdleLow);
    params.pclkActiveNeg = form.boolValue("pclkActiveNeg", params.pclkActiveNeg);
    for (uint8_t i = 0; i < 16; ++i) {
        const String field = String("rgbData") + String(i);
        if (!readDisplayRequestInteger(form, field.c_str(), params.rgbDataPins[i],
                                       -1, 127, extra, error)) return false;
        params.rgbDataPins[i] = static_cast<int8_t>(extra);
    }
    config.params.display = params;
    return true;
}

bool readIoFormInteger(const PeripheralFormFields& form,
                       const char* name, uint32_t fallback, uint32_t minimum,
                       uint32_t maximum, uint32_t& value, String& error,
                       bool allowHex) {
    value = fallback;
    if (!form.has(name)) {
        if (value >= minimum && value <= maximum) return true;
        error = String("IO_CONFIG_INVALID: invalid ") + name;
        return false;
    }
    String text = form.value(name, "");
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

bool applyIoPeripheralRequestParameters(const PeripheralFormFields& form,
                                        PeripheralConfig& config,
                                        bool resetDefaults, String& error) {
    if (config.type == PeripheralType::PCA9685_PWM) {
        const String encoded = form.value("pca9685", "");
        JsonDocument parameters;
        if (encoded.isEmpty() || encoded.length() > 2048 ||
            deserializeJson(parameters, encoded.c_str(), encoded.length()) || !parameters.is<JsonObject>()) {
            error = "PCA_CONFIG_INVALID"; return false;
        }
        return FastBee::parsePca9685Config(parameters.as<JsonObjectConst>(), config, error);
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        auto params = config.params.ioExpander;
        String driver = form.value(
            form.has("driver") ? "driver" : "driverName",
            !resetDefaults && params.driver[0] ? params.driver : "PCF8574");
        driver.trim();
        driver.toUpperCase();
        if (driver != "PCF8574" && driver != "MCP23017") {
            error = "IO_PROVIDER_INVALID: unsupported driver"; return false;
        }
        String reference = form.value("busPeripheralId",
            resetDefaults ? "" : params.busPeripheralId);
        reference.trim();
        if (!FastBee::H1Parameter::acceptsReference(FastBee::H1Parameter::ioBus,
                reference.c_str(), reference.length())) {
            error = "IO_PROVIDER_INVALID: bus reference is too long"; return false;
        }
        String reset = form.value("resetPeripheralId",
            resetDefaults ? "" : params.resetPeripheralId);
        reset.trim();
        if (driver == "MCP23017" ? !FastBee::H1Parameter::acceptsReference(
                FastBee::H1Parameter::mcpReset, reset.c_str(), reset.length()) : !reset.isEmpty()) {
            error = "RESET_REFERENCE_INVALID: MCP requires a dedicated native output reference"; return false;
        }
        uint32_t address = 0, interval = 0;
        if (!readIoFormInteger(form,
                form.has("address") ? "address" : "i2cAddress",
                !resetDefaults && params.address ? params.address : FastBee::H1Parameter::ioAddress.defaultValue,
                FastBee::H1Parameter::ioAddress.minimum, FastBee::H1Parameter::ioAddress.maximum, address, error, true) ||
            !readIoFormInteger(form, "sampleInterval",
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
    } else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
        auto params = config.params.gpio;
        String source = form.value("ioSource",
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
            String provider = form.value("providerId",
                resetDefaults ? "" : params.providerId);
            provider.trim();
            if (provider.isEmpty() || provider.length() >= sizeof(params.providerId)) {
                error = "IO_CHANNEL_INVALID: providerId is required and must fit 40 characters";
                return false;
            }
            uint32_t channel = 0, initial = 0, inputMode = 0;
            if (!readIoFormInteger(form, "channel", resetDefaults ? 0 : params.channel,
                    0, 15, channel, error) ||
                !readIoFormInteger(form, "initialState", static_cast<uint32_t>(params.initialState),
                    0, 1, initial, error) ||
                !readIoFormInteger(form, "inputMode", params.inputMode,
                    0, 0, inputMode, error)) return false;
            String inverted = form.value("invertLogic", params.invertLogic ? "true" : "false");
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

bool applyPeripheralRequestParameters(const PeripheralFormFields& form,
                                      PeripheralConfig& config,
                                      bool resetDefaults,
                                      String& errorMsg) {
    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        if (!applyDisplayRequestParameters(form, config,
                                           resetDefaults, errorMsg)) {
            return false;
        }
    } else if (config.type == PeripheralType::UART) {
        if (resetDefaults) initializeUartDefaults(config);
        config.params.uart.baudRate = form.intValue(
            "baudRate", config.params.uart.baudRate ?
                config.params.uart.baudRate : UART_DEFAULT_BAUD_RATE);
        config.params.uart.dataBits = form.intValue(
            "dataBits", config.params.uart.dataBits ?
                config.params.uart.dataBits : UART_DEFAULT_DATA_BITS);
        config.params.uart.stopBits = form.intValue(
            "stopBits", config.params.uart.stopBits ?
                config.params.uart.stopBits : UART_DEFAULT_STOP_BITS);
        config.params.uart.parity = form.intValue(
            "parity", config.params.uart.parity);
    } else if (config.type == PeripheralType::ENCODER) {
        if (resetDefaults) initializeEncoderDefaults(config);
        const int resolution = form.intValue(
            "resolution", config.params.encoder.resolution ?
                config.params.encoder.resolution : ENCODER_DEFAULT_RESOLUTION);
        if (resolution < 1 || resolution > UINT16_MAX) {
            errorMsg =
                "INVALID_ENCODER_RESOLUTION: expected 1-65535 pulses per revolution";
            return false;
        }
        config.params.encoder.resolution = static_cast<uint16_t>(resolution);
        config.params.encoder.useInterrupt = form.boolValue(
            "useInterrupt",
            resetDefaults ? true : config.params.encoder.useInterrupt);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = form.intValue(
            "frequency", resetDefaults || !config.params.i2c.frequency
                ? 100000 : config.params.i2c.frequency);
        config.params.i2c.address = form.intValue(
            "address", config.params.i2c.address);
        config.params.i2c.isMaster = form.boolValue(
            "isMaster", resetDefaults ? true : config.params.i2c.isMaster);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = form.intValue(
            "frequency", resetDefaults || !config.params.spi.frequency
                ? 1000000 : config.params.spi.frequency);
        config.params.spi.mode = form.intValue(
            "mode", config.params.spi.mode);
        config.params.spi.msbFirst = form.boolValue(
            "msbFirst", resetDefaults ? true : config.params.spi.msbFirst);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (!applyServoParams(form, config, resetDefaults, errorMsg)) {
            return false;
        }
    } else if (config.isGPIOPeripheral() &&
               !isAdcInputType(config.type)) {
        config.params.gpio.initialState = static_cast<GPIOState>(form.intValue(
            "initialState", resetDefaults ? 0 :
                static_cast<int>(config.params.gpio.initialState)));
        config.params.gpio.pwmChannel = form.intValue(
            "pwmChannel", resetDefaults ? 0 : config.params.gpio.pwmChannel);
        config.params.gpio.pwmFrequency = form.intValue(
            "pwmFrequency", resetDefaults ? 1000 :
                config.params.gpio.pwmFrequency);
        config.params.gpio.pwmResolution = form.intValue(
            "pwmResolution", resetDefaults ? 8 :
                config.params.gpio.pwmResolution);
        config.params.gpio.defaultDuty = form.intValue(
            "defaultDuty", resetDefaults ? 0 :
                config.params.gpio.defaultDuty);
        config.params.gpio.passive = form.boolValue(
            "passive", resetDefaults ? false : config.params.gpio.passive);
        if (FastBee::isDigitalInputPeripheral(config.type)) {
            applyDigitalInputParams(form, config);
        }
    } else if (isAdcInputType(config.type)) {
        applyAnalogInputParams(form, config, resetDefaults);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = form.intValue(
            "channel", resetDefaults ? 1 : config.params.dac.channel);
        config.params.dac.defaultValue = form.intValue(
            "defaultValue", resetDefaults ? 0 :
                config.params.dac.defaultValue);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = form.intValue(
            "stepsPerRevolution",
            resetDefaults || !config.params.stepper.stepsPerRevolution
                ? 2048 : config.params.stepper.stepsPerRevolution);
        config.params.stepper.speed = form.intValue(
            "speed", resetDefaults || !config.params.stepper.speed
                ? 8 : config.params.stepper.speed);
    } else if (config.type == PeripheralType::LCD) {
        const int width = form.intValue(
            "width", resetDefaults || !config.params.lcd.width
                ? 128 : config.params.lcd.width);
        const int height = form.intValue(
            "height", resetDefaults || !config.params.lcd.height
                ? 64 : config.params.lcd.height);
        const int interfaceValue = form.intValue(
            "interface", resetDefaults ? 2 : config.params.lcd.interface);
        const int contrast = form.intValue(
            "contrast", resetDefaults || !config.params.lcd.contrast
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
        int count = form.intValue(
            "count", resetDefaults || !config.params.neopixel.count
                ? 1 : config.params.neopixel.count);
        int brightness = form.intValue(
            "brightness", resetDefaults ? 64 :
                config.params.neopixel.brightness);
        count = clampParamInt(count, 1, 64);
        brightness = clampParamInt(brightness, 0, 255);
        config.params.neopixel.count = static_cast<uint16_t>(count);
        config.params.neopixel.brightness = static_cast<uint8_t>(brightness);
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = form.intValue(
            "mode", resetDefaults ? 0 : config.params.rf.mode);
        int pulseWidth = form.intValue(
            "pulseWidth", resetDefaults || !config.params.rf.pulseWidth
                ? 350 : config.params.rf.pulseWidth);
        int repeat = form.intValue(
            "repeat", resetDefaults || !config.params.rf.repeat
                ? 8 : config.params.rf.repeat);
        int bitLength = form.intValue(
            "bitLength", resetDefaults || !config.params.rf.bitLength
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
        config.params.rf.activeHigh = form.boolValue(
            "activeHigh", resetDefaults ? true : config.params.rf.activeHigh);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = form.intValue(
            "debounceMs", resetDefaults || !config.params.radar.debounceMs
                ? 50 : config.params.radar.debounceMs);
        int holdMs = form.intValue(
            "holdMs", resetDefaults || !config.params.radar.holdMs
                ? 2000 : config.params.radar.holdMs);
        debounceMs = clampParamInt(debounceMs, 0, 5000);
        holdMs = clampParamInt(holdMs, 0, 60000);
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = form.boolValue(
            "activeHigh", resetDefaults ? true :
                config.params.radar.activeHigh);
        config.params.radar.debounceMs = static_cast<uint16_t>(debounceMs);
        config.params.radar.holdMs = static_cast<uint16_t>(holdMs);
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        if (!applyP1PeripheralParams(form, config, resetDefaults,
                                     errorMsg)) {
            return false;
        }
    } else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int brightness = form.intValue(
            "brightness", resetDefaults ? 2 :
                config.params.segment.brightness);
        config.params.segment.brightness = static_cast<uint8_t>(
            clampParamInt(brightness, 0, 7));
    }
    return applyIoPeripheralRequestParameters(form, config, resetDefaults, errorMsg);
}

// Legacy per-type parameter reader for the add mutation route. Keeps the
// original defaults and validation on top of the shared apply* family; the
// route layer only maps the returned error to its HTTP response.
bool applyAddTypeParams(const PeripheralFormFields& form, PeripheralConfig& config,
                        String& errorMsg) {
    if (config.type == PeripheralType::UART) {
        initializeUartDefaults(config);
        config.params.uart.baudRate = form.intValue("baudRate", UART_DEFAULT_BAUD_RATE);
        config.params.uart.dataBits = form.intValue("dataBits", UART_DEFAULT_DATA_BITS);
        config.params.uart.stopBits = form.intValue("stopBits", UART_DEFAULT_STOP_BITS);
        config.params.uart.parity = form.intValue("parity", UART_DEFAULT_PARITY);
    } else if (config.type == PeripheralType::ENCODER) {
        initializeEncoderDefaults(config);
        const int resolution = form.intValue("resolution", ENCODER_DEFAULT_RESOLUTION);
        if (resolution < 1 || resolution > UINT16_MAX) {
            errorMsg =
                "INVALID_ENCODER_RESOLUTION: expected 1-65535 pulses per revolution";
            return false;
        }
        config.params.encoder.resolution = static_cast<uint16_t>(resolution);
        config.params.encoder.useInterrupt = form.boolValue("useInterrupt", true);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = form.intValue("frequency", 100000);
        config.params.i2c.address = form.intValue("address", 0);
        config.params.i2c.isMaster = form.boolValue("isMaster", true);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = form.intValue("frequency", 1000000);
        config.params.spi.mode = form.intValue("mode", 0);
        config.params.spi.msbFirst = form.boolValue("msbFirst", true);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (!applyServoParams(form, config, true, errorMsg)) {
            return false;
        }
        // Creation never energizes a servo. Test and enable are explicit steps.
        config.enabled = false;
    } else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
        config.params.gpio.initialState = static_cast<GPIOState>(form.intValue("initialState", 0));
        config.params.gpio.pwmChannel = form.intValue("pwmChannel", 0);
        config.params.gpio.pwmFrequency = form.intValue("pwmFrequency", 1000);
        config.params.gpio.pwmResolution = form.intValue("pwmResolution", 8);
        config.params.gpio.defaultDuty = form.intValue("defaultDuty", 0);
        config.params.gpio.passive = form.boolValue("passive", false);
        if (isDigitalInputPeripheral(config.type)) {
            applyDigitalInputParams(form, config);
        }
    } else if (isAdcInputType(config.type)) {
        applyAnalogInputParams(form, config, true);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = form.intValue("channel", 1);
        config.params.dac.defaultValue = form.intValue("defaultValue", 0);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = form.intValue("stepsPerRevolution", 2048);
        config.params.stepper.speed = form.intValue("speed", 8);
    } else if (config.type == PeripheralType::LCD) {
        int w = form.intValue("width", 128);
        int h = form.intValue("height", 64);
        int iface = form.intValue("interface", 2);
        int contrast = form.intValue("contrast", 255);
        config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
        config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
        config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
        config.params.lcd.contrast  = (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = form.intValue("count", 1);
        int brightness = form.intValue("brightness", 64);
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        config.params.neopixel.count = (uint16_t)count;
        config.params.neopixel.brightness = (uint8_t)brightness;
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = form.intValue("mode", 0);
        int pulseWidth = form.intValue("pulseWidth", 350);
        int repeat = form.intValue("repeat", 8);
        int bitLength = form.intValue("bitLength", 24);
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
        config.params.rf.activeHigh = form.boolValue("activeHigh", true);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = form.intValue("debounceMs", 50);
        int holdMs = form.intValue("holdMs", 2000);
        if (debounceMs < 0) debounceMs = 0;
        if (debounceMs > 5000) debounceMs = 5000;
        if (holdMs < 0) holdMs = 0;
        if (holdMs > 60000) holdMs = 60000;
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = form.boolValue("activeHigh", true);
        config.params.radar.debounceMs = (uint16_t)debounceMs;
        config.params.radar.holdMs = (uint16_t)holdMs;
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        if (!applyP1PeripheralParams(form, config, true, errorMsg)) {
            return false;
        }
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int b = form.intValue("brightness", 2);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        config.params.segment.brightness = (uint8_t)b;
    }
    return true;
}

// Legacy per-type parameter reader for the update mutation route; existing
// values stay the fallback when a field is absent.
bool applyUpdateTypeParams(const PeripheralFormFields& form, PeripheralConfig& config,
                           String& errorMsg) {
    if (config.type == PeripheralType::UART) {
        config.params.uart.baudRate = form.intValue("baudRate", config.params.uart.baudRate);
        config.params.uart.dataBits = form.intValue("dataBits", config.params.uart.dataBits);
        config.params.uart.stopBits = form.intValue("stopBits", config.params.uart.stopBits);
        config.params.uart.parity = form.intValue("parity", config.params.uart.parity);
    } else if (config.type == PeripheralType::ENCODER) {
        const int resolution = form.intValue("resolution", config.params.encoder.resolution ?
                config.params.encoder.resolution : ENCODER_DEFAULT_RESOLUTION);
        if (resolution < 1 || resolution > UINT16_MAX) {
            errorMsg =
                "INVALID_ENCODER_RESOLUTION: expected 1-65535 pulses per revolution";
            return false;
        }
        config.params.encoder.resolution = static_cast<uint16_t>(resolution);
        config.params.encoder.useInterrupt = form.boolValue("useInterrupt", config.params.encoder.useInterrupt);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = form.intValue("frequency", config.params.i2c.frequency);
        config.params.i2c.address = form.intValue("address", config.params.i2c.address);
        config.params.i2c.isMaster = form.boolValue("isMaster", config.params.i2c.isMaster);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = form.intValue("frequency", config.params.spi.frequency);
        config.params.spi.mode = form.intValue("mode", config.params.spi.mode);
        config.params.spi.msbFirst = form.boolValue("msbFirst", config.params.spi.msbFirst);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        if (!applyServoParams(form, config, false, errorMsg)) {
            return false;
        }
    } else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
        config.params.gpio.initialState = static_cast<GPIOState>(form.intValue("initialState", static_cast<int>(config.params.gpio.initialState)));
        config.params.gpio.pwmChannel = form.intValue("pwmChannel", config.params.gpio.pwmChannel);
        config.params.gpio.pwmFrequency = form.intValue("pwmFrequency", config.params.gpio.pwmFrequency);
        config.params.gpio.pwmResolution = form.intValue("pwmResolution", config.params.gpio.pwmResolution);
        config.params.gpio.defaultDuty = form.intValue("defaultDuty", config.params.gpio.defaultDuty);
        config.params.gpio.passive = form.boolValue("passive", config.params.gpio.passive);
        if (isDigitalInputPeripheral(config.type)) {
            applyDigitalInputParams(form, config);
        }
    } else if (isAdcInputType(config.type)) {
        if (config.params.adc.resolution < 9 || config.params.adc.resolution > 12) {
            applyAnalogInputParams(form, config, true);
        } else {
            if (config.params.adc.engineeringMax == 0.0f) {
                config.params.adc.engineeringMax = 100.0f;
            }
            applyAnalogInputParams(form, config);
        }
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = form.intValue("channel", config.params.dac.channel);
        config.params.dac.defaultValue = form.intValue("defaultValue", config.params.dac.defaultValue);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = form.intValue("stepsPerRevolution", config.params.stepper.stepsPerRevolution ? config.params.stepper.stepsPerRevolution : 2048);
        config.params.stepper.speed = form.intValue("speed", config.params.stepper.speed ? config.params.stepper.speed : 8);
    } else if (config.type == PeripheralType::LCD) {
        int w = form.intValue("width", config.params.lcd.width ? config.params.lcd.width : 128);
        int h = form.intValue("height", config.params.lcd.height ? config.params.lcd.height : 64);
        int iface = form.intValue("interface", config.params.lcd.interface);
        int contrast = form.intValue("contrast", config.params.lcd.contrast ? config.params.lcd.contrast : 255);
        config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
        config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
        config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
        config.params.lcd.contrast  = (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = form.intValue("count", config.params.neopixel.count ? config.params.neopixel.count : 1);
        int brightness = form.intValue("brightness", config.params.neopixel.brightness);
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        config.params.neopixel.count = (uint16_t)count;
        config.params.neopixel.brightness = (uint8_t)brightness;
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = form.intValue("mode", config.params.rf.mode);
        int pulseWidth = form.intValue("pulseWidth", config.params.rf.pulseWidth ? config.params.rf.pulseWidth : 350);
        int repeat = form.intValue("repeat", config.params.rf.repeat ? config.params.rf.repeat : 8);
        int bitLength = form.intValue("bitLength", config.params.rf.bitLength ? config.params.rf.bitLength : 24);
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
        config.params.rf.activeHigh = form.boolValue("activeHigh", config.params.rf.activeHigh);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = form.intValue("debounceMs", config.params.radar.debounceMs ? config.params.radar.debounceMs : 50);
        int holdMs = form.intValue("holdMs", config.params.radar.holdMs ? config.params.radar.holdMs : 2000);
        if (debounceMs < 0) debounceMs = 0;
        if (debounceMs > 5000) debounceMs = 5000;
        if (holdMs < 0) holdMs = 0;
        if (holdMs > 60000) holdMs = 60000;
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = form.boolValue("activeHigh", config.params.radar.activeHigh);
        config.params.radar.debounceMs = (uint16_t)debounceMs;
        config.params.radar.holdMs = (uint16_t)holdMs;
    } else if (config.type == PeripheralType::SENSOR_GENERIC ||
               config.type == PeripheralType::SDIO ||
               config.type == PeripheralType::DS1302_RTC ||
               config.type == PeripheralType::LCD1602 ||
               config.type == PeripheralType::RFID_MFRC522) {
        if (!applyP1PeripheralParams(form, config, false, errorMsg)) {
            return false;
        }
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int b = form.intValue("brightness", config.params.segment.brightness);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        config.params.segment.brightness = (uint8_t)b;
    }
    return true;
}
} // namespace FastBee
