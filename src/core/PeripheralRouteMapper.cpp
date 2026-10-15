#include "core/PeripheralRouteMapper.h"
#include "core/MemoryBudget.h"
#include "core/PeripheralTypes.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/SensorConfigSupport.h"
#include "core/RtcBootSeedConfig.h"
#include "core/RuntimeFeatureManager.h"
#include "network/FixedJsonField.h"

namespace FastBee {

namespace {
bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}
} // namespace

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
void addAnalogInputParams(JsonObject params, const PeripheralConfig& config, bool includeAdvanced) {
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

namespace {
int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}
} // namespace

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
void addPassiveBuzzerFlag(JsonVariant doc, const PeripheralConfig& config) {
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive) {
        doc["passive"] = true;
    }
}

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

bool isListMemoryCriticallyLow() {
    return FastBee::MemoryBudget::dramFree() < 4096 || FastBee::MemoryBudget::dramLargest() < 1536;
}

bool shouldForceCompactList() {
    return FastBee::MemoryBudget::dramFree() < 16384 || FastBee::MemoryBudget::dramLargest() < 8192;
}

} // namespace FastBee
