/**
 * @description Peripheral configuration persistence implementation
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/PeripheralManager.h"
#include "core/RtcBootSeedConfig.h"
#include "core/AsyncExecTypes.h"
#include "core/SensorConfigSupport.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterContract.h"
#include "utils/FileUtils.h"
#include "systems/LoggerSystem.h"
#include "PeripheralManagerConfigHelpers.h"
#include "core/PeripheralResourcePlanner.h"
#include "utils/CheckedText.h"
#include "network/FixedJsonField.h"
#include <cmath>
#include <cstring>

using namespace FastBee::PeripheralManagerConfigDetail;
// ========== 持久化 ==========

namespace {
void serializePersistedPeripheral(JsonObject obj, const PeripheralConfig& config) {
    FastBee::setFixedJsonField(obj, "id", config.id);
    FastBee::setFixedJsonField(obj, "name", config.name);
    FastBee::setFixedJsonField(obj, "type", static_cast<int>(config.type));
    FastBee::setFixedJsonField(obj, "enabled", config.enabled);

    // 引脚配置
    JsonArray pins = obj["pins"].to<JsonArray>();
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255) {
            pins.add(config.pins[i]);
        }
    }

    // 类型特定参数
    JsonObject params = obj["params"].to<JsonObject>();
    if (config.type == PeripheralType::PCA9685_PWM) FastBee::serializePca9685Config(params, config);
    if (config.isGPIOPeripheral() && config.isExpanderChannel()) {
        FastBee::setFixedJsonField(params, "ioSource", config.params.gpio.ioSource);
        FastBee::setFixedJsonField(params, "providerId", config.params.gpio.providerId);
        FastBee::setFixedJsonField(params, "channel", config.params.gpio.channel);
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        FastBee::setFixedJsonField(params, "driver", config.params.ioExpander.driver);
        FastBee::setFixedJsonField(params, "busPeripheralId", config.params.ioExpander.busPeripheralId);
        FastBee::setFixedJsonField(params, "resetPeripheralId", config.params.ioExpander.resetPeripheralId);
        FastBee::setFixedJsonField(params, "address", config.params.ioExpander.address);
        FastBee::setFixedJsonField(params, "sampleInterval", config.params.ioExpander.sampleInterval);
    }

    if (config.type == PeripheralType::UART) {
        FastBee::setFixedJsonField(params, "baudRate", config.params.uart.baudRate);
        FastBee::setFixedJsonField(params, "dataBits", config.params.uart.dataBits);
        FastBee::setFixedJsonField(params, "stopBits", config.params.uart.stopBits);
        FastBee::setFixedJsonField(params, "parity", config.params.uart.parity);
    }
    else if (config.type == PeripheralType::ENCODER) {
        FastBee::setFixedJsonField(params, "resolution", config.params.encoder.resolution);
        FastBee::setFixedJsonField(params, "useInterrupt", config.params.encoder.useInterrupt);
    }
    else if (config.type == PeripheralType::I2C) {
        FastBee::setFixedJsonField(params, "frequency", config.params.i2c.frequency);
        FastBee::setFixedJsonField(params, "address", config.params.i2c.address);
        FastBee::setFixedJsonField(params, "isMaster", config.params.i2c.isMaster);
    }
    else if (config.type == PeripheralType::SPI) {
        FastBee::setFixedJsonField(params, "frequency", config.params.spi.frequency);
        FastBee::setFixedJsonField(params, "mode", config.params.spi.mode);
        FastBee::setFixedJsonField(params, "msbFirst", config.params.spi.msbFirst);
    }
    else if (config.type == PeripheralType::PWM_SERVO) {
        FastBee::setFixedJsonField(params, "frequency", config.params.servo.frequency);
        FastBee::setFixedJsonField(params, "minPulse", config.params.servo.minPulse);
        FastBee::setFixedJsonField(params, "maxPulse", config.params.servo.maxPulse);
        FastBee::setFixedJsonField(params, "minAngle", config.params.servo.minAngle);
        FastBee::setFixedJsonField(params, "maxAngle", config.params.servo.maxAngle);
        FastBee::setFixedJsonField(params, "initialAngle", config.params.servo.initialAngle);
        FastBee::setFixedJsonField(params, "pwmChannel", config.params.servo.pwmChannel);
        FastBee::setFixedJsonField(params, "pwmResolution", config.params.servo.pwmResolution);
        FastBee::setFixedJsonField(params, "inverted", config.params.servo.inverted);
        FastBee::setFixedJsonField(params, "releaseAfterMove", config.params.servo.releaseAfterMove);
    }
    else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
        FastBee::setFixedJsonField(params, "initialState", static_cast<int>(config.params.gpio.initialState));
        FastBee::setFixedJsonField(params, "pwmChannel", config.params.gpio.pwmChannel);
        FastBee::setFixedJsonField(params, "pwmFrequency", config.params.gpio.pwmFrequency);
        FastBee::setFixedJsonField(params, "pwmResolution", config.params.gpio.pwmResolution);
        FastBee::setFixedJsonField(params, "defaultDuty", config.params.gpio.defaultDuty);
        if (config.params.gpio.passive) params["passive"] = true;
        // Logical PCF8574 outputs also use invertLogic to map the
        // configured startup state to the physical channel level.
        if (config.isExpanderChannel() && config.params.gpio.invertLogic) {
            FastBee::setFixedJsonField(params, "invertLogic", true);
        }
        // AO 模拟量输出模式（非默认值时才写入，保持配置精简）
        if (config.params.gpio.aoMode) {
            FastBee::setFixedJsonField(params, "aoMode", true);
            FastBee::setFixedJsonField(params, "aoMinPct", config.params.gpio.aoMinPct);
            FastBee::setFixedJsonField(params, "aoMaxPct", config.params.gpio.aoMaxPct);
        }
        // DI 参数只在非默认值时写入，保持旧配置文件紧凑。
        if (config.type == PeripheralType::GPIO_DIGITAL_INPUT ||
            config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
            config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
            config.type == PeripheralType::GPIO_INTERRUPT_RISING ||
            config.type == PeripheralType::GPIO_INTERRUPT_FALLING ||
            config.type == PeripheralType::GPIO_INTERRUPT_CHANGE) {
            if (config.params.gpio.inputMode != static_cast<uint8_t>(DigitalInputMode::AUTO)) {
                FastBee::setFixedJsonField(params, "inputMode", config.params.gpio.inputMode);
            }
            if (config.params.gpio.invertLogic) params["invertLogic"] = true;
            if (config.params.gpio.debounceMs > 0) params["debounceMs"] = config.params.gpio.debounceMs;
            if (config.params.gpio.pairedInputId[0]) {
                FastBee::setFixedJsonField(params, "pairedInputId", config.params.gpio.pairedInputId);
                FastBee::setFixedJsonField(params, "pairDebounceMs", config.params.gpio.pairDebounceMs);
            }
            if (config.params.gpio.reportMode != static_cast<uint8_t>(DigitalReportMode::ON_CHANGE)) {
                FastBee::setFixedJsonField(params, "reportMode", config.params.gpio.reportMode);
            }
        }
    }
    else if (isAdcInputType(config.type)) {
        FastBee::setFixedJsonField(params, "attenuation", config.params.adc.attenuation);
        FastBee::setFixedJsonField(params, "resolution", config.params.adc.resolution);
        FastBee::setFixedJsonField(params, "sampleRate", config.params.adc.sampleRate);
        // 多采样滤波（非默认值时才写入，向后兼容旧配置）
        if (config.params.adc.sampleCount > 1) params["sampleCount"] = config.params.adc.sampleCount;
        if (config.params.adc.filterMode > 0) params["filterMode"] = config.params.adc.filterMode;
        const bool hasAnalogCalibration =
            config.params.adc.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW) ||
            config.params.adc.conversionEnabled ||
            config.params.adc.clampEnabled ||
            config.params.adc.calibratedVoltage ||
            config.params.adc.inputMin != 0.0f || config.params.adc.inputMax != 0.0f ||
            config.params.adc.engineeringMin != 0.0f || config.params.adc.engineeringMax != 100.0f ||
            config.params.adc.engineeringOffset != 0.0f || config.params.adc.decimalPlaces != 2 ||
            config.params.adc.vRef != 0.0f || config.params.adc.voltageRatio != 0.0f ||
            config.params.adc.shuntResistance != 0.0f || config.params.adc.inputGain != 0.0f ||
            config.params.adc.inputOffset != 0.0f || config.params.adc.rawMin != 0 ||
            config.params.adc.rawMax != 0;
        if (hasAnalogCalibration) {
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
    else if (config.type == PeripheralType::DAC) {
        FastBee::setFixedJsonField(params, "channel", config.params.dac.channel);
    }
    else if (config.type == PeripheralType::STEPPER_MOTOR) {
        FastBee::setFixedJsonField(params, "stepsPerRevolution", config.params.stepper.stepsPerRevolution);
        FastBee::setFixedJsonField(params, "speed", config.params.stepper.speed);
    }
    else if (config.type == PeripheralType::SDIO) {
        FastBee::setFixedJsonField(params, "interface", config.params.sdcard.interface);
        FastBee::setFixedJsonField(params, "frequency", config.params.sdcard.frequency);
    }
    else if (config.type == PeripheralType::DS1302_RTC) {
        FastBee::setFixedJsonField(params, "use24Hour", config.params.ds1302.use24Hour);
        FastBee::setFixedJsonField(params, "trickleCharge", config.params.ds1302.trickleCharge);
        FastBee::serializeRtcBootSeedConfig(params, config.params.ds1302.bootSeed,
                                           config.params.ds1302.utcOffsetMinutes);
    }
    else if (config.type == PeripheralType::LCD1602) {
        FastBee::setFixedJsonField(params, "cols", config.params.lcd1602.cols);
        FastBee::setFixedJsonField(params, "rows", config.params.lcd1602.rows);
        FastBee::setFixedJsonField(params, "address", config.params.lcd1602.address);
        FastBee::setFixedJsonField(params, "backlight", config.params.lcd1602.backlight);
    }
    else if (config.type == PeripheralType::RFID_MFRC522) {
        FastBee::setFixedJsonField(params, "frequency", config.params.rfid.frequency);
    }
    else if (config.type == PeripheralType::LCD) {
        FastBee::setFixedJsonField(params, "width", config.params.lcd.width);
        FastBee::setFixedJsonField(params, "height", config.params.lcd.height);
        FastBee::setFixedJsonField(params, "interface", config.params.lcd.interface);
        FastBee::setFixedJsonField(params, "contrast", config.params.lcd.contrast);
    }
    else if (config.type == PeripheralType::SCREEN_DISPLAY) {
        const auto& display = config.params.display;
        FastBee::setFixedJsonField(params, "backend", display.backend);
        FastBee::setFixedJsonField(params, "driver", display.driver);
        FastBee::setFixedJsonField(params, "touchBackend", display.touchBackend);
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
        FastBee::setFixedJsonField(params, "rgbInitProfile", display.rgbInitProfile);
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
        FastBee::setFixedJsonField(params, "controlBackend", display.controlBackend);
        FastBee::setFixedJsonField(params, "controlAddress", display.controlAddress);
        FastBee::setFixedJsonField(params, "controlResetMask", display.controlResetMask);
        FastBee::setFixedJsonField(params, "controlBacklightMask", display.controlBacklightMask);
        FastBee::setFixedJsonField(params, "controlEnableMask", display.controlEnableMask);
        FastBee::setFixedJsonField(params, "controlBuzzerMask", display.controlBuzzerMask);
        FastBee::setFixedJsonField(params, "touchBuzzerDurationMs", display.touchBuzzerDurationMs);
        FastBee::setFixedJsonField(params, "controlInactiveMask", display.controlInactiveMask);
        FastBee::setFixedJsonField(params, "controlTouchIntMask", display.controlTouchIntMask);
        JsonObject namedPins = params["pins"].to<JsonObject>();
        FastBee::setFixedJsonField(namedPins, "mosi", display.mosi);
        FastBee::setFixedJsonField(namedPins, "sclk", display.sclk);
        FastBee::setFixedJsonField(namedPins, "cs", display.cs);
        FastBee::setFixedJsonField(namedPins, "dc", display.dc);
        FastBee::setFixedJsonField(namedPins, "rst", display.rst);
        FastBee::setFixedJsonField(namedPins, "backlight", display.backlight);
        FastBee::setFixedJsonField(namedPins, "touchSda", display.touchSda);
        FastBee::setFixedJsonField(namedPins, "touchScl", display.touchScl);
        FastBee::setFixedJsonField(namedPins, "touchInt", display.touchInt);
        FastBee::setFixedJsonField(namedPins, "touchRst", display.touchRst);
        JsonArray rgbPins = namedPins["rgbDataPins"].to<JsonArray>();
        for (uint8_t i = 0; i < 16; ++i) rgbPins.add(display.rgbDataPins[i]);
        FastBee::setFixedJsonField(namedPins, "rgbDe", display.rgbDe);
        FastBee::setFixedJsonField(namedPins, "rgbVsync", display.rgbVsync);
        FastBee::setFixedJsonField(namedPins, "rgbHsync", display.rgbHsync);
        FastBee::setFixedJsonField(namedPins, "rgbPclk", display.rgbPclk);
        FastBee::setFixedJsonField(namedPins, "controlSda", display.controlSda);
        FastBee::setFixedJsonField(namedPins, "controlScl", display.controlScl);
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        FastBee::setFixedJsonField(params, "brightness", config.params.segment.brightness);
    }
    else if (config.type == PeripheralType::NEO_PIXEL) {
        FastBee::setFixedJsonField(params, "count", clampNeoPixelCount(config.params.neopixel.count));
        FastBee::setFixedJsonField(params, "brightness", config.params.neopixel.brightness);
    }
    else if (config.type == PeripheralType::RF_MODULE) {
        FastBee::setFixedJsonField(params, "mode", config.params.rf.mode);
        FastBee::setFixedJsonField(params, "pulseWidth", clampRfPulseWidth(config.params.rf.pulseWidth));
        FastBee::setFixedJsonField(params, "repeat", clampRfRepeat(config.params.rf.repeat));
        FastBee::setFixedJsonField(params, "bitLength", clampRfBitLength(config.params.rf.bitLength));
        FastBee::setFixedJsonField(params, "activeHigh", config.params.rf.activeHigh);
    }
    else if (config.type == PeripheralType::RADAR_SENSOR) {
        FastBee::setFixedJsonField(params, "mode", config.params.radar.mode);
        FastBee::setFixedJsonField(params, "activeHigh", config.params.radar.activeHigh);
        FastBee::setFixedJsonField(params, "debounceMs", config.params.radar.debounceMs);
        FastBee::setFixedJsonField(params, "holdMs", config.params.radar.holdMs);
    }
    else if (config.type == PeripheralType::SENSOR_GENERIC) {
        FastBee::serializeSensorConfig(params, config);
    }
}

bool parsePersistedPeripheral(JsonObject obj, PeripheralConfig& config, String& parseError) {
    config.id = obj["id"] | "";
    config.name = obj["name"] | "";
    config.type = static_cast<PeripheralType>(obj["type"] | 0);
    config.enabled = obj["enabled"] | false;

    if (config.type == PeripheralType::RESERVED_46) {
        LOG_WARNINGF("Peripheral Manager: Skipping removed peripheral type 46 for '%s'", config.id.c_str());
        return false;
    }
    if (config.type == PeripheralType::UART) {
        initializeUartDefaults(config);
    } else if (config.type == PeripheralType::ENCODER) {
        initializeEncoderDefaults(config);
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = STEPPER_DEFAULT_STEPS_PER_REV;
        config.params.stepper.speed = STEPPER_DEFAULT_RPM;
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        config.params.neopixel.count = NEOPIXEL_DEFAULT_COUNT;
        config.params.neopixel.brightness = NEOPIXEL_DEFAULT_BRIGHTNESS;
    } else if (config.type == PeripheralType::RF_MODULE) {
        config.params.rf.mode = RF_MODE_TX;
        config.params.rf.pulseWidth = RF_DEFAULT_PULSE_WIDTH_US;
        config.params.rf.repeat = RF_DEFAULT_REPEAT;
        config.params.rf.bitLength = RF_DEFAULT_BIT_LENGTH;
        config.params.rf.activeHigh = true;
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        config.params.radar.mode = RADAR_MODE_DIGITAL;
        config.params.radar.activeHigh = true;
        config.params.radar.debounceMs = RADAR_DEFAULT_DEBOUNCE_MS;
        config.params.radar.holdMs = RADAR_DEFAULT_HOLD_MS;
    } else if (config.type == PeripheralType::SENSOR_GENERIC) {
        initializeSensorDefaults(config);
    } else if (config.type == PeripheralType::SDIO) {
        initializeSdCardDefaults(config);
    } else if (config.type == PeripheralType::DS1302_RTC) {
        initializeDs1302Defaults(config);
    } else if (config.type == PeripheralType::LCD1602) {
        initializeLcd1602Defaults(config);
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        initializeRfidDefaults(config);
    } else if (config.type == PeripheralType::SCREEN_DISPLAY) {
        initializeDisplayDefaults(config);
    } else if (isAdcInputType(config.type)) {
        initializeAdcDefaults(config);
    } else if (config.type == PeripheralType::PWM_SERVO) {
        initializeServoDefaults(config);
    }

    // 引脚配置
    JsonArray pins = obj["pins"].as<JsonArray>();
    config.pinCount = 0;
    for (int i = 0; i < 8 && i < pins.size(); i++) {
        config.pins[i] = pins[i] | 255;
        if (config.pins[i] != 255) {
            config.pinCount++;
        }
    }

    // 类型特定参数
    JsonObject params = obj["params"].as<JsonObject>();
    if (!params.isNull()) {
        if (config.type == PeripheralType::PCA9685_PWM &&
            !FastBee::parsePca9685Config(JsonObjectConst(params), config, parseError)) {
            if (parseError.isEmpty()) parseError = "PCA_CONFIG_INVALID";
        }
        if (config.isGPIOPeripheral()) {
            config.params.gpio.ioSource = params["ioSource"] | 0;
            strlcpy(config.params.gpio.providerId, params["providerId"] | "",
                    sizeof(config.params.gpio.providerId));
            config.params.gpio.channel = params["channel"] | 0;
            if ((!params["ioSource"].isNull() &&
                 (!params["ioSource"].is<uint8_t>() || params["ioSource"].as<unsigned>() > 1)) ||
                (config.params.gpio.ioSource == 1 &&
                 (!params["channel"].is<uint8_t>() || params["channel"].as<unsigned>() > 15 ||
                  strlen(params["providerId"] | "") >= sizeof(config.params.gpio.providerId)))) {
                parseError = "IO_CHANNEL_INVALID: invalid source, reference or channel";
            }
        }
        if (config.type == PeripheralType::IO_EXPANDER) {
            strlcpy(config.params.ioExpander.driver, params["driver"] | "PCF8574",
                    sizeof(config.params.ioExpander.driver));
            strlcpy(config.params.ioExpander.busPeripheralId, params["busPeripheralId"] | "",
                    sizeof(config.params.ioExpander.busPeripheralId));
            strlcpy(config.params.ioExpander.resetPeripheralId, params["resetPeripheralId"] | "",
                    sizeof(config.params.ioExpander.resetPeripheralId));
            config.params.ioExpander.address = params["address"] | static_cast<uint8_t>(FastBee::H1Parameter::ioAddress.defaultValue);
            config.params.ioExpander.sampleInterval = params["sampleInterval"] | static_cast<uint16_t>(FastBee::H1Parameter::ioInterval.defaultValue);
            if (strlen(params["driver"] | "PCF8574") >= sizeof(config.params.ioExpander.driver) ||
                strlen(params["busPeripheralId"] | "") >= sizeof(config.params.ioExpander.busPeripheralId) ||
                (!params["resetPeripheralId"].isNull() &&
                 (!params["resetPeripheralId"].is<const char*>() ||
                  strlen(params["resetPeripheralId"] | "") >= sizeof(config.params.ioExpander.resetPeripheralId))) ||
                (!params["address"].isNull() && !params["address"].is<uint8_t>()) ||
                (!params["sampleInterval"].isNull() && !params["sampleInterval"].is<uint16_t>())) {
                parseError = "IO_PROVIDER_INVALID: oversized reference or invalid numeric value";
            }
        }
        if (config.type == PeripheralType::UART) {
            config.params.uart.baudRate = params["baudRate"] | UART_DEFAULT_BAUD_RATE;
            config.params.uart.dataBits = params["dataBits"] | UART_DEFAULT_DATA_BITS;
            config.params.uart.stopBits = params["stopBits"] | UART_DEFAULT_STOP_BITS;
            config.params.uart.parity = params["parity"] | UART_DEFAULT_PARITY;
        }
        else if (config.type == PeripheralType::ENCODER) {
            config.params.encoder.resolution = params["resolution"] | ENCODER_DEFAULT_RESOLUTION;
            config.params.encoder.useInterrupt = params["useInterrupt"] | true;
        }
        else if (config.type == PeripheralType::I2C) {
            config.params.i2c.frequency = params["frequency"] | 100000;
            config.params.i2c.address = params["address"] | 0;
            config.params.i2c.isMaster = params["isMaster"] | true;
        }
        else if (config.type == PeripheralType::SPI) {
            config.params.spi.frequency = params["frequency"] | 1000000;
            config.params.spi.mode = params["mode"] | 0;
            config.params.spi.msbFirst = params["msbFirst"] | true;
        }
        else if (config.type == PeripheralType::PWM_SERVO) {
            config.params.servo.frequency = params["frequency"] | SERVO_DEFAULT_FREQUENCY_HZ;
            config.params.servo.minPulse = params["minPulse"] | SERVO_DEFAULT_MIN_PULSE_US;
            config.params.servo.maxPulse = params["maxPulse"] | SERVO_DEFAULT_MAX_PULSE_US;
            config.params.servo.minAngle = params["minAngle"] | SERVO_DEFAULT_MIN_ANGLE;
            config.params.servo.maxAngle = params["maxAngle"] | SERVO_DEFAULT_MAX_ANGLE;
            config.params.servo.initialAngle = params["initialAngle"] | SERVO_DEFAULT_INITIAL_ANGLE;
            config.params.servo.pwmChannel = params["pwmChannel"] | SERVO_DEFAULT_PWM_CHANNEL;
            config.params.servo.pwmResolution = params["pwmResolution"] | SERVO_DEFAULT_PWM_RESOLUTION;
            config.params.servo.inverted = params["inverted"] | false;
            config.params.servo.releaseAfterMove = params["releaseAfterMove"] | false;
        }
        else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
            config.params.gpio.initialState = static_cast<GPIOState>(params["initialState"] | 0);
            config.params.gpio.pwmChannel = params["pwmChannel"] | 0;
            config.params.gpio.pwmFrequency = params["pwmFrequency"] | 1000;
            config.params.gpio.pwmResolution = params["pwmResolution"] | 8;
            config.params.gpio.defaultDuty = params["defaultDuty"] | 0;
            config.params.gpio.passive = params["passive"] | false;
            // AO 模拟量输出模式（旧配置无此字段时默认关闭，保持原行为）
            config.params.gpio.aoMode = params["aoMode"] | false;
            config.params.gpio.aoMinPct = params["aoMinPct"] | 0;
            config.params.gpio.aoMaxPct = params["aoMaxPct"] | 100;
            config.params.gpio.inputMode = params["inputMode"] | static_cast<uint8_t>(DigitalInputMode::AUTO);
            if (config.params.gpio.inputMode > static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)) {
                config.params.gpio.inputMode = static_cast<uint8_t>(DigitalInputMode::AUTO);
            }
            config.params.gpio.invertLogic = params["invertLogic"] | false;
            const String peerId = params["pairedInputId"] | "";
            if (peerId.length() >= sizeof(config.params.gpio.pairedInputId)) {
                LOGGER.warning("[Peripheral] Invalid paired DI ID length; skipping peripheral");
                return false;
            }
            peerId.toCharArray(config.params.gpio.pairedInputId, sizeof(config.params.gpio.pairedInputId));
            const int pairMs = params["pairDebounceMs"] | 0;
            if (pairMs != 0 && (pairMs < 20 || pairMs > 5000)) {
                LOGGER.warning("[Peripheral] Invalid paired DI stable time; skipping peripheral");
                return false;
            }
            config.params.gpio.pairDebounceMs = static_cast<uint16_t>(pairMs);
            int debounceMs = params["debounceMs"] | 0;
            if (debounceMs < 0) debounceMs = 0;
            if (debounceMs > 5000) debounceMs = 5000;
            config.params.gpio.debounceMs = static_cast<uint16_t>(debounceMs);
            config.params.gpio.reportMode = params["reportMode"] | static_cast<uint8_t>(DigitalReportMode::ON_CHANGE);
            if (config.params.gpio.reportMode > static_cast<uint8_t>(DigitalReportMode::EVENT_ONLY)) {
                config.params.gpio.reportMode = static_cast<uint8_t>(DigitalReportMode::ON_CHANGE);
            }
        }
        else if (isAdcInputType(config.type)) {
            config.params.adc.attenuation = params["attenuation"] | 3;
            config.params.adc.resolution = params["resolution"] | 12;
            config.params.adc.sampleRate = params["sampleRate"] | 0;
            // 多采样滤波（旧配置无此字段时默认单次采样，保持原行为）
            config.params.adc.sampleCount = normalizeAdcSampleCount(params["sampleCount"] | 1);
            config.params.adc.filterMode = normalizeAdcFilterMode(params["filterMode"] | 0);
            config.params.adc.signalMode = params["signalMode"] | static_cast<uint8_t>(AnalogSignalMode::RAW);
            if (config.params.adc.signalMode > static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR)) {
                config.params.adc.signalMode = static_cast<uint8_t>(AnalogSignalMode::RAW);
            }
            config.params.adc.conversionEnabled = params["conversionEnabled"] | false;
            config.params.adc.calibratedVoltage = params["calibratedVoltage"] | false;
            config.params.adc.clampEnabled = params["clampEnabled"] | false;
            int decimals = params["decimalPlaces"] | 2;
            if (decimals < 0) decimals = 0;
            if (decimals > 6) decimals = 6;
            config.params.adc.decimalPlaces = static_cast<uint8_t>(decimals);
            config.params.adc.inputMin = params["inputMin"] | 0.0f;
            config.params.adc.inputMax = params["inputMax"] | 0.0f;
            config.params.adc.engineeringMin = params["engineeringMin"] | 0.0f;
            config.params.adc.engineeringMax = params["engineeringMax"] | 100.0f;
            config.params.adc.engineeringOffset = params["engineeringOffset"] | 0.0f;
            config.params.adc.vRef = params["vRef"] | 0.0f;
            config.params.adc.voltageRatio = params["voltageRatio"] | 0.0f;
            config.params.adc.shuntResistance = params["shuntResistance"] | 0.0f;
            config.params.adc.inputGain = params["inputGain"] | 0.0f;
            config.params.adc.inputOffset = params["inputOffset"] | 0.0f;
            config.params.adc.rawMin = params["rawMin"] | 0;
            config.params.adc.rawMax = params["rawMax"] | 0;
        }
        else if (config.type == PeripheralType::DAC) {
            config.params.dac.channel = params["channel"] | 1;
        }
        else if (config.type == PeripheralType::STEPPER_MOTOR) {
            config.params.stepper.stepsPerRevolution = params["stepsPerRevolution"] | STEPPER_DEFAULT_STEPS_PER_REV;
            config.params.stepper.speed = clampStepperSpeed(params["speed"] | STEPPER_DEFAULT_RPM);
        }
        else if (config.type == PeripheralType::SDIO) {
            config.params.sdcard.interface = params["interface"] | SDCARD_INTERFACE_SPI;
            config.params.sdcard.frequency = params["frequency"] | SDCARD_DEFAULT_FREQUENCY;
        }
        else if (config.type == PeripheralType::DS1302_RTC) {
            config.params.ds1302.use24Hour = params["use24Hour"] | true;
            config.params.ds1302.trickleCharge = params["trickleCharge"] | 0;
            if (!FastBee::parseRtcBootSeedConfig(JsonObjectConst(params),
                config.params.ds1302.bootSeed, config.params.ds1302.utcOffsetMinutes, parseError)) return false;
        }
        else if (config.type == PeripheralType::LCD1602) {
            config.params.lcd1602.cols = params["cols"] | LCD1602_DEFAULT_COLS;
            config.params.lcd1602.rows = params["rows"] | LCD1602_DEFAULT_ROWS;
            config.params.lcd1602.address = params["address"] | LCD1602_DEFAULT_ADDRESS;
            config.params.lcd1602.backlight = params["backlight"] | true;
        }
        else if (config.type == PeripheralType::RFID_MFRC522) {
            config.params.rfid.frequency = params["frequency"] | RFID_DEFAULT_FREQUENCY;
        }
        else if (config.type == PeripheralType::LCD) {
            // 支持扩展宽度/高度（用 int 读取避免 uint8_t 默认值裁剪）
            int w = params["width"]  | 128;
            int h = params["height"] | 64;
            int iface = params["interface"] | 2; // 0=Parallel, 1=SPI, 2=I2C
            int contrast = params["contrast"] | 255; // 对比度，默认拉满
            config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
            config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
            config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
            config.params.lcd.contrast  = (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
        }
        else if (config.type == PeripheralType::SCREEN_DISPLAY) {
            const char* backend = params["backend"] | "spi_tft";
            const char* driver = params["driver"] | "st7789";
            const char* touchBackend = params["touchBackend"] | "none";
            const char* controlBackend = params["controlBackend"] | "none";
            const char* rgbProfile = params["rgbInitProfile"] | "st7701_type1";
            if (strlen(backend) >= sizeof(config.params.display.backend) ||
                strlen(driver) >= sizeof(config.params.display.driver) ||
                strlen(touchBackend) >= sizeof(config.params.display.touchBackend) ||
                strlen(controlBackend) >= sizeof(config.params.display.controlBackend) ||
                strlen(rgbProfile) >= sizeof(config.params.display.rgbInitProfile)) {
                parseError = "DISPLAY_PARAMS_INVALID: backend identifier too long";
            } else {
                strlcpy(config.params.display.backend, backend,
                        sizeof(config.params.display.backend));
                strlcpy(config.params.display.driver, driver,
                        sizeof(config.params.display.driver));
                strlcpy(config.params.display.touchBackend, touchBackend,
                        sizeof(config.params.display.touchBackend));
                strlcpy(config.params.display.controlBackend, controlBackend,
                        sizeof(config.params.display.controlBackend));
                strlcpy(config.params.display.rgbInitProfile, rgbProfile,
                        sizeof(config.params.display.rgbInitProfile));
            }
            config.params.display.width = params["width"] | DISPLAY_DEFAULT_WIDTH;
            config.params.display.height = params["height"] | DISPLAY_DEFAULT_HEIGHT;
            config.params.display.rotation = params["rotation"] | 0;
            config.params.display.colorDepth = params["colorDepth"] | DISPLAY_DEFAULT_COLOR_DEPTH;
            config.params.display.bufferLines = params["bufferLines"] | DISPLAY_DEFAULT_BUFFER_LINES;
            config.params.display.brightness = params["brightness"] | DISPLAY_DEFAULT_BRIGHTNESS;
            config.params.display.backlightChannel = params["backlightChannel"] | 0;
            config.params.display.backlightActiveHigh = params["backlightActiveHigh"] | true;
            config.params.display.spiFrequency = params["spiFrequency"] | DISPLAY_DEFAULT_SPI_FREQUENCY;
            config.params.display.i2cFrequency = params["i2cFrequency"] | DISPLAY_DEFAULT_I2C_FREQUENCY;
            auto readDisplayExtra = [&](JsonVariant value, int32_t fallback,
                                        int32_t minimum, int32_t maximum) -> int32_t {
                if (value.isNull()) return fallback;
                if (!value.is<int32_t>() || value.as<int32_t>() < minimum ||
                    value.as<int32_t>() > maximum) {
                    parseError = "DISPLAY_PARAMS_INVALID: invalid RGB/control parameter";
                    return fallback;
                }
                return value.as<int32_t>();
            };
#define LOAD_DISPLAY_EXTRA(field, minimum, maximum) \
            config.params.display.field = static_cast<decltype(config.params.display.field)>( \
                readDisplayExtra(params[#field], config.params.display.field, minimum, maximum))
            LOAD_DISPLAY_EXTRA(rgbFrequency, 1000000, 16000000);
            LOAD_DISPLAY_EXTRA(hsyncFrontPorch, 0, 1023);
            LOAD_DISPLAY_EXTRA(hsyncPulseWidth, 1, 1023);
            LOAD_DISPLAY_EXTRA(hsyncBackPorch, 0, 1023);
            LOAD_DISPLAY_EXTRA(vsyncFrontPorch, 0, 1023);
            LOAD_DISPLAY_EXTRA(vsyncPulseWidth, 1, 1023);
            LOAD_DISPLAY_EXTRA(vsyncBackPorch, 0, 1023);
            LOAD_DISPLAY_EXTRA(controlAddress, 0, 127);
            LOAD_DISPLAY_EXTRA(controlResetMask, 0, 255);
            LOAD_DISPLAY_EXTRA(controlBacklightMask, 0, 255);
            LOAD_DISPLAY_EXTRA(controlEnableMask, 0, 255);
            LOAD_DISPLAY_EXTRA(controlBuzzerMask, 0, 255);
            LOAD_DISPLAY_EXTRA(touchBuzzerDurationMs, 20, 150);
            LOAD_DISPLAY_EXTRA(controlInactiveMask, 0, 255);
            LOAD_DISPLAY_EXTRA(controlTouchIntMask, 0, 255);
#undef LOAD_DISPLAY_EXTRA
            if (!config.params.display.controlMasksDisjoint()) {
                parseError = "DISPLAY_CONTROL_INVALID: control masks must be disjoint";
            }
            config.params.display.hsyncIdleLow = params["hsyncIdleLow"] | false;
            config.params.display.vsyncIdleLow = params["vsyncIdleLow"] | false;
            config.params.display.pclkActiveNeg = params["pclkActiveNeg"] | false;
            JsonObject namedPins = params["pins"].as<JsonObject>();
            if (namedPins.isNull()) {
                parseError = "DISPLAY_PINS_INVALID: params.pins object required";
            } else {
                config.params.display.mosi = namedPins["mosi"] | -1;
                config.params.display.sclk = namedPins["sclk"] | -1;
                config.params.display.cs = namedPins["cs"] | -1;
                config.params.display.dc = namedPins["dc"] | -1;
                config.params.display.rst = namedPins["rst"] | -1;
                config.params.display.backlight = namedPins["backlight"] | -1;
                config.params.display.touchSda = namedPins["touchSda"] | -1;
                config.params.display.touchScl = namedPins["touchScl"] | -1;
                config.params.display.touchInt = namedPins["touchInt"] | -1;
                config.params.display.touchRst = namedPins["touchRst"] | -1;
#define LOAD_DISPLAY_PIN(field) \
                config.params.display.field = static_cast<int8_t>( \
                    readDisplayExtra(namedPins[#field], -1, -1, 127))
                LOAD_DISPLAY_PIN(rgbDe);
                LOAD_DISPLAY_PIN(rgbVsync);
                LOAD_DISPLAY_PIN(rgbHsync);
                LOAD_DISPLAY_PIN(rgbPclk);
                LOAD_DISPLAY_PIN(controlSda);
                LOAD_DISPLAY_PIN(controlScl);
#undef LOAD_DISPLAY_PIN
                JsonArray rgbPins = namedPins["rgbDataPins"].as<JsonArray>();
                if (!namedPins["rgbDataPins"].isNull() && (rgbPins.isNull() || rgbPins.size() != 16)) {
                    parseError = "DISPLAY_PINS_INVALID: rgbDataPins requires 16 entries";
                }
                for (uint8_t i = 0; i < 16; ++i) {
                    config.params.display.rgbDataPins[i] = static_cast<int8_t>(
                        readDisplayExtra(rgbPins[i], -1, -1, 127));
                }
            }
            if (!config.params.display.controlTouchSyncValid()) {
                parseError = "DISPLAY_CONTROL_INVALID: invalid touch INT reset control";
            }
            config.pinCount = 0;
        }
        else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
            int b = params["brightness"] | 2;
            if (b < 0) b = 0;
            if (b > 7) b = 7;
            config.params.segment.brightness = (uint8_t)b;
        }
        else if (config.type == PeripheralType::NEO_PIXEL) {
            int count = params["count"] | NEOPIXEL_DEFAULT_COUNT;
            int brightness = params["brightness"] | NEOPIXEL_DEFAULT_BRIGHTNESS;
            config.params.neopixel.count = clampNeoPixelCount(count < 0 ? 0 : (uint16_t)count);
            config.params.neopixel.brightness = clampNeoPixelBrightness(brightness < 0 ? 0 : (uint16_t)brightness);
        }
        else if (config.type == PeripheralType::RF_MODULE) {
            int mode = params["mode"] | RF_MODE_TX;
            config.params.rf.mode = (mode == RF_MODE_RX) ? RF_MODE_RX : RF_MODE_TX;
            config.params.rf.pulseWidth = clampRfPulseWidth(params["pulseWidth"] | RF_DEFAULT_PULSE_WIDTH_US);
            config.params.rf.repeat = clampRfRepeat(params["repeat"] | RF_DEFAULT_REPEAT);
            config.params.rf.bitLength = clampRfBitLength(params["bitLength"] | RF_DEFAULT_BIT_LENGTH);
            config.params.rf.activeHigh = params["activeHigh"] | true;
        }
        else if (config.type == PeripheralType::RADAR_SENSOR) {
            config.params.radar.mode = RADAR_MODE_DIGITAL;
            config.params.radar.activeHigh = params["activeHigh"] | true;
            config.params.radar.debounceMs = params["debounceMs"] | RADAR_DEFAULT_DEBOUNCE_MS;
            config.params.radar.holdMs = params["holdMs"] | RADAR_DEFAULT_HOLD_MS;
        }
        else if (config.type == PeripheralType::SENSOR_GENERIC) {
            FastBee::parseSensorConfig(params, config, parseError);
        }
    }
    return true;
}
// Supplied fields must survive the actual boot parser and serializer. This
// rejects unknown fields, implicit numeric narrowing/clamping and NUL suffixes;
// omitted optional defaults may be supplied by the existing boot parser.
bool peripheralImportFieldsMatch(JsonVariantConst supplied, JsonVariantConst normalized, unsigned depth = 0) {
    if (depth > 8 || supplied.isNull() || normalized.isNull()) return false;
    if (supplied.is<JsonObjectConst>()) {
        if (!normalized.is<JsonObjectConst>()) return false;
        for (JsonPairConst field : supplied.as<JsonObjectConst>()) {
            if (!peripheralImportFieldsMatch(field.value(), normalized[field.key()], depth + 1)) return false;
        }
        return true;
    }
    if (supplied.is<JsonArrayConst>()) {
        if (!normalized.is<JsonArrayConst>() || supplied.size() != normalized.size()) return false;
        for (size_t i = 0; i < supplied.size(); ++i) {
            if (!peripheralImportFieldsMatch(supplied[i], normalized[i], depth + 1)) return false;
        }
        return true;
    }
    if (supplied.is<const char*>()) {
        if (!normalized.is<const char*>()) return false;
        const JsonString text = supplied.as<JsonString>(), copy = normalized.as<JsonString>();
        return text.size() == copy.size() && !memchr(text.c_str(), '\0', text.size()) &&
               memcmp(text.c_str(), copy.c_str(), text.size()) == 0;
    }
    if (supplied.is<bool>() || normalized.is<bool>()) {
        return supplied.is<bool>() && normalized.is<bool>() && supplied.as<bool>() == normalized.as<bool>();
    }
    if (normalized.is<int64_t>() || normalized.is<uint64_t>()) return supplied == normalized;
    // ADC calibration values are stored as float; preserve that representable
    // value while still rejecting strings, nonfinite data and integer wrap.
    return supplied.is<double>() && normalized.is<double>() &&
        std::isfinite(supplied.as<double>()) && std::isfinite(normalized.as<double>()) &&
        supplied.as<float>() == normalized.as<float>();
}

bool importedDigitalInput(const PeripheralConfig& config) {
    return config.type == PeripheralType::GPIO_DIGITAL_INPUT ||
        config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
        config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
        config.type == PeripheralType::GPIO_INTERRUPT_RISING ||
        config.type == PeripheralType::GPIO_INTERRUPT_FALLING ||
        config.type == PeripheralType::GPIO_INTERRUPT_CHANGE;
}

bool validateImportedPeripheralRelations(const std::vector<PeripheralConfig>& candidates, String& error) {
    size_t providers = 0;
    for (const auto& config : candidates) {
        if (config.type == PeripheralType::IO_EXPANDER && ++providers > 4) {
            error = "CONFIG_IMPORT_PROVIDER_CAPACITY"; return false;
        }
        const bool mcp = config.type == PeripheralType::IO_EXPANDER &&
            strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0;
        // Import must not bypass the provider/channel activation review.
        if (mcp && config.enabled) { error = "CONFIG_IMPORT_MCP_SAVE_DISABLED_FIRST"; return false; }
        for (const auto& other : candidates) {
            if (&config == &other) continue;
            if (config.id == other.id) { error = "CONFIG_IMPORT_DUPLICATE_ID"; return false; }
            const bool singleton = config.type == PeripheralType::SDIO || config.type == PeripheralType::DS1302_RTC ||
                config.type == PeripheralType::LCD1602 || config.type == PeripheralType::LCD ||
                config.type == PeripheralType::RFID_MFRC522 ||
                config.type == PeripheralType::PCA9685_PWM;
            if (config.type == other.type && (singleton ||
                (config.type == PeripheralType::SCREEN_DISPLAY && config.enabled && other.enabled))) {
                error = "CONFIG_IMPORT_SINGLETON_CONFLICT"; return false;
            }
            if (config.isExpanderChannel() && other.isExpanderChannel() &&
                strcmp(config.params.gpio.providerId, other.params.gpio.providerId) == 0 &&
                config.params.gpio.channel == other.params.gpio.channel) {
                error = "CONFIG_IMPORT_CHANNEL_CONFLICT"; return false;
            }
        }
        if (importedDigitalInput(config) && config.params.gpio.pairedInputId[0]) {
            bool found = false;
            for (const auto& peer : candidates) {
                if (peer.id == config.params.gpio.pairedInputId && peer.id != config.id &&
                    importedDigitalInput(peer)) found = true;
            }
            if (!found) { error = "CONFIG_IMPORT_DI_REFERENCE"; return false; }
        }
    }
    return true;
}
} // namespace

bool PeripheralManager::configurationImportPending() const {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    return !lock.isLocked() || _configurationImportPending;
}

bool PeripheralManager::prepareConfigurationImport(JsonDocument& document,
        std::vector<PeripheralConfig>& candidates, const FastBee::PreflightContext& context,
        FastBee::PreflightResult& result, String& error) {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    candidates.clear(); result.reset(); error = "";
    if (!lock.isLocked() || _configurationImportPending || !_configurationReviewVersion) {
        error = "CONFIG_IMPORT_BUSY_OR_PENDING_RESTART"; return false;
    }
    if (document.overflowed() || !document.is<JsonObject>() || document.size() != 1 ||
        !document["peripherals"].is<JsonArray>() || measureJson(document) > 24 * 1024) {
        error = "CONFIG_IMPORT_SCHEMA_OR_SIZE"; return false;
    }
    JsonArray imported = document["peripherals"].as<JsonArray>();
    size_t externalCount = 0;
    for (const auto& item : peripherals) if (item.second.isModbusPeripheral()) ++externalCount;
    if (imported.size() + externalCount > FastBee::ResourceProfile::MAX_PERIPHERALS) {
        error = "CONFIG_IMPORT_PROFILE_CAPACITY"; return false;
    }
    candidates.reserve(imported.size() + externalCount);
    for (JsonVariant entry : imported) {
        if (!entry.is<JsonObject>() || !entry["id"].is<const char*>() || !entry["name"].is<const char*>() ||
            !entry["type"].is<uint8_t>() || !entry["enabled"].is<bool>() ||
            !entry["pins"].is<JsonArray>() || entry["pins"].size() > 8 || !entry["params"].is<JsonObject>()) {
            error = "CONFIG_IMPORT_ENTRY_SCHEMA"; return false;
        }
        PeripheralConfig candidate;
        if (!parsePersistedPeripheral(entry.as<JsonObject>(), candidate, error) || !error.isEmpty() ||
            !isPeripheralTypeUserConfigurable(candidate.type) ||
            !validateConfig(candidate, error, false)) {
            error = "CONFIG_IMPORT_ENTRY_INVALID"; return false;
        }
        JsonDocument normalized;
        serializePersistedPeripheral(normalized.to<JsonObject>(), candidate);
        if (normalized.overflowed() || !peripheralImportFieldsMatch(entry.as<JsonVariantConst>(), normalized.as<JsonVariantConst>())) {
            error = "CONFIG_IMPORT_FIELDS_NOT_PRESERVED"; return false;
        }
        candidates.push_back(std::move(candidate));
    }
    // Protocol-owned Modbus peripherals remain in the target graph but are not
    // serialized into this file. Never let an import take over their IDs.
    for (const auto& item : peripherals) {
        if (!item.second.isModbusPeripheral()) continue;
        candidates.push_back(item.second);
        if (candidates.back().id != item.second.id || candidates.back().name != item.second.name) {
            error = "CONFIG_IMPORT_ALLOCATION"; return false;
        }
    }
    if (!validateImportedPeripheralRelations(candidates, error)) return false;
    const auto& profile = FastBee::getCompiledBoardProfile();
    FastBee::PeripheralResourcePlanner planner(profile, context, result, candidates.data(), candidates.size());
    for (const auto& config : candidates) {
        planner.addPeripheral(config, config.enabled ? FastBee::ClaimState::ACTIVE : FastBee::ClaimState::DORMANT);
    }
    planner.addEthernet(context.ethernet);
    planner.addCellular(context.cellular);
    if (result.claimsTruncated || result.issuesTruncated) {
        error = "CONFIG_IMPORT_PREFLIGHT_CAPACITY"; return false;
    }
    for (size_t index = 0; index < result.issueCount; ++index) {
        if (result.issues[index].severity == FastBee::PreflightSeverity::ERROR) {
            error = "CONFIG_IMPORT_RESOURCE_CONFLICT"; return false;
        }
    }
    return true; // Confirmation warnings are returned for explicit review.
}

bool PeripheralManager::commitConfigurationImport(JsonDocument& document, uint64_t expectedVersion, String& error) {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    if (!lock.isLocked() || _configurationImportPending || !expectedVersion ||
        expectedVersion != _configurationReviewVersion) {
        error = "CONFIG_IMPORT_REVISION_CONFLICT"; return false;
    }
    // Called only after prepareConfigurationImport under the same owner.
    if (document.overflowed() || measureJson(document) > 24 * 1024) {
        error = "CONFIG_IMPORT_SCHEMA_OR_SIZE"; return false;
    }
    const size_t expectedSize = measureJson(document);
    File output = FileUtils::openAtomicWrite(PERIPHERAL_CONFIG_FILE);
    if (!output) { error = "CONFIG_IMPORT_STORAGE"; return false; }
    const size_t written = serializeJson(document, output);
    output.flush(); output.close();
    const String temporary = String(PERIPHERAL_CONFIG_FILE) + ".tmp";
    bool valid = written == expectedSize;
    {
        File input = LittleFS.open(temporary, "r");
        FastBeeJsonDocLarge verified;
        valid = valid && input && input.size() == expectedSize && !deserializeJson(verified, input) &&
            verified.as<JsonVariantConst>() == document.as<JsonVariantConst>();
        if (input) input.close();
    }
    if (!valid) {
        LittleFS.remove(temporary); error = "CONFIG_IMPORT_VERIFY"; return false;
    }
    if (!FileUtils::commitAtomicWrite(temporary, PERIPHERAL_CONFIG_FILE)) {
        error = "CONFIG_IMPORT_STORAGE"; return false;
    }
    _configurationImportPending = true;
    error = "";
    return true;
}

bool PeripheralManager::saveConfiguration() {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked() || _configurationImportPending) return false;
    FastBeeJsonDocLarge doc;
    JsonArray periphs = doc["peripherals"].to<JsonArray>();

    for (const auto& pair : peripherals) {
        const PeripheralConfig& config = pair.second;

        // Modbus 外设由 protocol.json 管理，不保存到 peripherals.json
        if (config.isModbusPeripheral()) continue;

        JsonObject obj = periphs.add<JsonObject>();
        serializePersistedPeripheral(obj, config);
    }

    if (doc.overflowed()) {
        LOG_ERROR("Peripheral Manager: Configuration JSON overflowed; refusing partial save");
        return false;
    }

    const size_t expected = measureJson(doc);
    File out = FileUtils::openAtomicWrite(PERIPHERAL_CONFIG_FILE);
    if (!out) return false;
    const size_t written = serializeJson(doc, out);
    out.flush();
    out.close();

    const String tmpPath = String(PERIPHERAL_CONFIG_FILE) + ".tmp";
    if (written != expected) {
        LittleFS.remove(tmpPath);
        LOG_ERRORF("Peripheral Manager: Configuration write truncated (%u/%u)",
                   static_cast<unsigned>(written), static_cast<unsigned>(expected));
        return false;
    }
    if (!FileUtils::commitAtomicWrite(tmpPath, PERIPHERAL_CONFIG_FILE)) {
        LOG_ERROR("Peripheral Manager: Failed to commit configuration atomically");
        return false;
    }

    LOG_INFO("Peripheral Manager: Configuration saved successfully");
    return true;
}

bool PeripheralManager::loadConfiguration() {
    if (_configurationImportPending) return false; // Apply only after an actual restart.
    if (!LittleFS.exists(PERIPHERAL_CONFIG_FILE)) {
        LOG_INFO("Peripheral Manager: No configuration file found");
        return true;
    }

    // 检查文件大小，避免一次性读取过大文件
    File file = LittleFS.open(PERIPHERAL_CONFIG_FILE, "r");
    if (!file) {
        LOG_ERROR("Peripheral Manager: Failed to open config file for reading");
        return false;
    }

    size_t fileSize = file.size();
    LOG_DEBUGF("Peripheral Manager: Config file size: %d bytes", fileSize);

    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < fileSize + 4096) {
        LOG_ERRORF("Peripheral Manager: Insufficient memory (free: %d, needed: %d)",
                   freeHeap, fileSize + 4096);
        file.close();
        return false;
    }

    // 使用流式解析，避免一次性读取整个文件
    FastBeeJsonDocLarge doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        LOG_ERRORF("Peripheral Manager: Failed to parse config - %s", error.c_str());
        return false;
    }

    JsonArray periphs = doc["peripherals"].as<JsonArray>();
    if (periphs.isNull()) {
        LOG_WARNING("Peripheral Manager: No peripherals found in configuration");
        return true;
    }

    RecursiveMutexGuard configurationLock(_mutex, pdMS_TO_TICKS(50));
    if (!configurationLock.isLocked()) return false;
    if (controlOutputIsClaimed()) return false;
    // Reload must never erase a live/retiring lease or orphan a checked timer.
    if (!actionTickers.empty()) return false;
    for (const auto& runtime : runtimeStates)
        if (ordinaryDigitalClaimedLocked(runtime.first)) return false;
    invalidateSensorConfigurationLocked();
    peripherals.clear();
    runtimeStates.clear();
    pinToPeripheral.clear();

    int loadedCount = 0;
    for (JsonObject obj : periphs) {
        if (peripherals.size() >= FastBee::ResourceProfile::MAX_PERIPHERALS) {
            LOG_WARNINGF("Peripheral Manager: profile %s allows max %u peripherals, skipping remaining config entries",
                         FastBee::ResourceProfile::NAME,
                         static_cast<unsigned int>(FastBee::ResourceProfile::MAX_PERIPHERALS));
            break;
        }

        PeripheralConfig config;
        String parseError;

        if (!parsePersistedPeripheral(obj, config, parseError)) continue;

        if (!config.id.isEmpty() && config.type != PeripheralType::UNCONFIGURED) {
            String errorMsg = parseError;
            bool valid = parseError.isEmpty() && validateConfig(config, errorMsg);
            if (!valid) {
                LOG_WARNINGF("Peripheral Manager: Invalid config '%s' loaded with error status: %s",
                             config.id.c_str(),
                             errorMsg.c_str());
            }

            peripherals[config.id] = config;

            PeripheralRuntimeState state;
            state.id = config.id;
            if (valid) {
                state.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
            } else {
                state.status = PeripheralStatus::PERIPHERAL_ERROR;
                state.lastError = "CONFIG_INVALID: " + errorMsg;
            }
            runtimeStates[config.id] = state;

            updatePinMapping(config.id, config);
            loadedCount++;
        }
    }

    // References are validated against the complete set, independently of JSON
    // order. Invalid imports remain visible and never drive hardware.
    for (const auto& item : peripherals) {
        auto state = runtimeStates.find(item.first);
        if (state == runtimeStates.end() || state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
        String error;
        if (!validateIoDependencies(item.second, item.first, error)) {
            state->second.status = PeripheralStatus::PERIPHERAL_ERROR;
            const bool unavailableParent = error.startsWith("IO_PROVIDER_UNAVAILABLE") ||
                                           error.startsWith("I2C_REFERENCE_INVALID");
            state->second.lastError = unavailableParent ? error : "CONFIG_INVALID: " + error;
        }
    }

    // 加固：加载完成后基于真实数据重建引脚映射缓存，确保一致性
    // （保险套：如果配置文件历史遗留同引脚多外设，updatePinMapping 会覆盖后者，
    //   由 checkPinConflict 基于真实数据的逻辑兼容此情况）
    rebuildPinMapping();

    LOG_INFOF("Peripheral Manager: Loaded %d peripheral configurations", loadedCount);
    return true;
}
