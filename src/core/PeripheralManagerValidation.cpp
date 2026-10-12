// Peripheral configuration admission. Runtime I/O remains in PeripheralManager.cpp.
#include "core/PeripheralManager.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterContract.h"
#include "core/SensorConfigSupport.h"
#include "core/ResourceProfile.h"
#include "core/ChipConfig.h"
#include "PeripheralManagerConfigHelpers.h"
#include <algorithm>
#include <cstring>

using namespace FastBee::PeripheralManagerConfigDetail;

namespace {
bool isMcp23017(const PeripheralConfig& config) {
    return config.type == PeripheralType::IO_EXPANDER &&
        memchr(config.params.ioExpander.driver, '\0', sizeof(config.params.ioExpander.driver)) &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0;
}
}  // namespace

bool PeripheralManager::validateConfig(const PeripheralConfig& config, String& errorMsg, bool checkRunningGraph) const {
    // 基本字段验证
    if (config.id.isEmpty()) {
        errorMsg = "ID 不能为空";
        return false;
    }

    if (config.name.isEmpty()) {
        errorMsg = "名称不能为空";
        return false;
    }

    if (config.id.length() > FastBee::ResourceProfile::MAX_PERIPHERAL_ID_LEN) {
        errorMsg = "Peripheral ID too long (max " + String(FastBee::ResourceProfile::MAX_PERIPHERAL_ID_LEN) + ")";
        return false;
    }

    if (config.name.length() > FastBee::ResourceProfile::MAX_PERIPHERAL_NAME_LEN) {
        errorMsg = "Peripheral name too long (max " + String(FastBee::ResourceProfile::MAX_PERIPHERAL_NAME_LEN) + ")";
        return false;
    }

    if (config.type == PeripheralType::UNCONFIGURED) {
        errorMsg = "外设类型不能为空";
        return false;
    }

    if (config.type == PeripheralType::RESERVED_46) {
        errorMsg = "该外设类型已移除";
        return false;
    }

    if (!isPeripheralTypeImplemented(config.type)) {
        errorMsg = String("外设类型 ") + getPeripheralTypeName(config.type) + " 尚未实现硬件驱动";
        return false;
    }

    if (checkRunningGraph && (config.type == PeripheralType::SDIO ||
        config.type == PeripheralType::PCA9685_PWM ||
        config.type == PeripheralType::DS1302_RTC ||
        config.type == PeripheralType::LCD1602 || config.type == PeripheralType::LCD ||
        config.type == PeripheralType::RFID_MFRC522)) {
        for (const auto& pair : peripherals) {
            if (pair.first != config.id && pair.second.type == config.type) {
                errorMsg = String("SINGLETON_RESOURCE_BUSY: ") +
                           getPeripheralTypeName(config.type) +
                           " 当前固件仅允许一个实例";
                return false;
            }
        }
    }

    if (checkRunningGraph && config.type == PeripheralType::SCREEN_DISPLAY && config.enabled) {
        for (const auto& pair : peripherals) {
            if (pair.first != config.id && pair.second.type == PeripheralType::SCREEN_DISPLAY &&
                pair.second.enabled) {
                errorMsg = "SINGLETON_RESOURCE_BUSY: only one graphical display can be active";
                return false;
            }
        }
    }

    if (config.type == PeripheralType::PCA9685_PWM)
        return FastBee::validatePca9685Config(config, errorMsg);

    // Modbus 外设不需要引脚配置
    if (config.isModbusPeripheral()) {
        if (config.params.modbus.slaveAddress < 1 || config.params.modbus.slaveAddress > 247) {
            errorMsg = "Modbus 从站地址无效 (有效范围: 1-247)";
            return false;
        }
        return true;
    }

    // 设备事件虚拟外设：无需引脚/参数校验，仅用 id+name
    if (isDeviceEventType(config.type)) {
        return true;
    }

    if (isGpioDigitalInputType(config.type)) {
        const auto& gpio = config.params.gpio;
        if (!memchr(gpio.pairedInputId, '\0', sizeof(gpio.pairedInputId)) ||
            config.id == gpio.pairedInputId ||
            (gpio.pairDebounceMs != 0 && (gpio.pairDebounceMs < 20 || gpio.pairDebounceMs > 5000))) {
            errorMsg = "DI_PAIR_INVALID: choose another DI ID (max 40 chars), stable time 20..5000ms";
            return false;
        }
        // The peer can be loaded later. Missing/disabled/unreadable peers are
        // detected as a fault by the runtime monitor rather than ignored.
    }
    if (config.isGPIOPeripheral() && config.params.gpio.ioSource > 1) {
        errorMsg = "IO_SOURCE_INVALID";
        return false;
    }
    if (config.isExpanderChannel()) {
        if (config.pinCount != 0 || config.params.gpio.providerId[0] == '\0' ||
            !memchr(config.params.gpio.providerId, '\0', sizeof(config.params.gpio.providerId)) ||
            config.params.gpio.channel >= 16 ||
            (config.type != PeripheralType::GPIO_DIGITAL_INPUT &&
             config.type != PeripheralType::GPIO_DIGITAL_OUTPUT) ||
            config.params.gpio.passive || config.params.gpio.inputMode != 0 ||
            (config.params.gpio.initialState != GPIOState::STATE_LOW &&
             config.params.gpio.initialState != GPIOState::STATE_HIGH) ||
            config.params.gpio.debounceMs > 5000 || config.params.gpio.reportMode > 2) {
            errorMsg = "IO_CHANNEL_INVALID: plain digital input/output, no MCU pins or configurable pulls required";
            return false;
        }
        return true;
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        if (!memchr(config.params.ioExpander.driver, '\0', sizeof(config.params.ioExpander.driver)) ||
            (strcasecmp(config.params.ioExpander.driver, "PCF8574") != 0 && !isMcp23017(config)) ||
            !memchr(config.params.ioExpander.resetPeripheralId, '\0', sizeof(config.params.ioExpander.resetPeripheralId)) ||
            (isMcp23017(config) ? !config.params.ioExpander.resetPeripheralId[0] :
                                config.params.ioExpander.resetPeripheralId[0] != '\0') ||
            !FastBee::H1Parameter::accepts(FastBee::H1Parameter::ioAddress, config.params.ioExpander.address) ||
            !FastBee::H1Parameter::acceptsIoInterval(config.params.ioExpander.sampleInterval, true)) {
            errorMsg = "IO_PROVIDER_INVALID: PCF8574/MCP23017 address 0x20..0x27, interval 20..1000ms; MCP requires RESET reference";
            return false;
        }
        if (config.params.ioExpander.busPeripheralId[0]) {
            if (!memchr(config.params.ioExpander.busPeripheralId, '\0',
                        sizeof(config.params.ioExpander.busPeripheralId)) || config.pinCount != 0) {
                errorMsg = "I2C_REFERENCE_INVALID: choose bus reference or two pins";
                return false;
            }
            return true;
        }
        if (config.pinCount != 2 || config.pins[0] == config.pins[1]) {
            errorMsg = "I2C_PINS_INVALID: SDA/SCL required";
            return false;
        }
    }

    // Generic sensors may represent an external/logical data source and are
    // therefore valid without a local GPIO. When a pin is supplied, still run
    // the normal chip/reserved-pin checks below.
    if (config.type == PeripheralType::SENSOR_GENERIC) {
        if (!FastBee::validateSensorConfig(config, errorMsg)) return false;
        if (config.pinCount == 0) return true;
    }

    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        const auto& display = config.params.display;
        const bool rgb = memchr(display.backend, '\0', sizeof(display.backend)) &&
                         strcasecmp(display.backend, "rgb") == 0;
        if (config.pinCount != 0) {
            errorMsg = "DISPLAY_PINS_INVALID: use params.pins named display pins";
            return false;
        }
        if (!memchr(display.backend, '\0', sizeof(display.backend)) ||
            !memchr(display.driver, '\0', sizeof(display.driver)) ||
            !memchr(display.touchBackend, '\0', sizeof(display.touchBackend)) ||
            (!rgb && strcasecmp(display.backend, "spi_tft") != 0) ||
            (rgb ? strcasecmp(display.driver, "st7701") != 0 :
                (strcasecmp(display.driver, "st7789") != 0 &&
                 strcasecmp(display.driver, "ili9341") != 0)) ||
            (strcasecmp(display.touchBackend, "none") != 0 &&
             strcasecmp(display.touchBackend, "gt911") != 0)) {
            errorMsg = "DISPLAY_BACKEND_UNSUPPORTED: supported spi_tft with st7789/ili9341 and none/gt911 touch";
            return false;
        }
        if (display.width < 128 || display.width > (rgb ? 480 : 320) ||
            display.height < 128 || display.height > (rgb ? 480 : 320) ||
            display.rotation > 3 || display.colorDepth != 16 ||
            display.bufferLines < 1 || display.bufferLines > 64 ||
            display.brightness > 100 ||
            display.touchBuzzerDurationMs < 20 || display.touchBuzzerDurationMs > 150 ||
            (display.backlight >= 0 && display.backlightChannel >= CHIP_MAX_PWM_CH) ||
            display.spiFrequency < 1000000UL || display.spiFrequency > 80000000UL ||
            (display.i2cFrequency != 100000UL && display.i2cFrequency != 400000UL)) {
            errorMsg = "DISPLAY_PARAMS_INVALID: dimensions 128..320, RGB565, 1..64 lines and valid bus rates required";
            return false;
        }
        const uint16_t shortSide = std::min(display.width, display.height);
        const uint16_t longSide = std::max(display.width, display.height);
        if (!rgb && (shortSide > 240 || longSide > 320)) {
            errorMsg = "DISPLAY_DIMENSIONS_UNSUPPORTED: ST7789/ILI9341 controller window is at most 240x320";
            return false;
        }
        const bool touchEnabled = strcasecmp(display.touchBackend, "none") != 0;
        const bool controlEnabled = rgb && memchr(display.controlBackend, '\0', sizeof(display.controlBackend)) &&
                                    strcasecmp(display.controlBackend, "none") != 0;
        if (rgb) {
#if !CHIP_IS_ESP32S3 && !defined(FASTBEE_NATIVE_TEST)
            errorMsg = "DISPLAY_RGB_UNAVAILABLE: RGB output requires ESP32-S3";
            return false;
#endif
            if (!memchr(display.rgbInitProfile, '\0', sizeof(display.rgbInitProfile)) ||
                strcmp(display.rgbInitProfile, "st7701_type1") != 0 ||
                display.width != 480 || display.height != 480 ||
                display.rgbFrequency < 1000000UL || display.rgbFrequency > 16000000UL ||
                !display.hsyncPulseWidth || !display.vsyncPulseWidth ||
                display.hsyncFrontPorch > 255 || display.hsyncPulseWidth > 255 || display.hsyncBackPorch > 255 ||
                display.vsyncFrontPorch > 255 || display.vsyncPulseWidth > 255 || display.vsyncBackPorch > 255 ||
                !memchr(display.controlBackend, '\0', sizeof(display.controlBackend)) ||
                (controlEnabled && strcasecmp(display.controlBackend, "tca9554") != 0 &&
                                   strcasecmp(display.controlBackend, "ch32v003") != 0)) {
                errorMsg = "DISPLAY_RGB_PARAMS_INVALID: supported profile, 480x480 and bounded timings required";
                return false;
            }
            for (uint8_t index = 10; index < 30; ++index) {
                if (display.pinAt(index) < 0) {
                    errorMsg = "DISPLAY_RGB_PINS_REQUIRED: sixteen data pins and four timing pins required";
                    return false;
                }
            }
            if (controlEnabled && (display.controlSda < 0 || display.controlScl < 0 ||
                display.controlAddress < 8 || display.controlAddress > 0x77 ||
                (touchEnabled && (display.controlAddress == 0x5D || display.controlAddress == 0x14)) ||
                (touchEnabled && (display.controlSda != display.touchSda ||
                                  display.controlScl != display.touchScl)) ||
                !display.controlMasksDisjoint() ||
                (display.controlBuzzerMask != 0 &&
                 strcasecmp(display.controlBackend, "ch32v003") != 0) ||
                (strcasecmp(display.controlBackend, "ch32v003") == 0 && display.controlBacklightMask != 0))) {
                errorMsg = "DISPLAY_CONTROL_INVALID: valid shared I2C bus and disjoint control masks required";
                return false;
            }
            if (!controlEnabled && (display.controlSda >= 0 || display.controlScl >= 0)) {
                errorMsg = "DISPLAY_CONTROL_INVALID: unused control bus must not claim pins";
                return false;
            }
        }
        if (display.mosi < 0 || display.sclk < 0 || display.cs < 0 || (!rgb && display.dc < 0) ||
            (touchEnabled && (display.touchSda < 0 || display.touchScl < 0)) ||
            (!touchEnabled && (display.touchSda >= 0 || display.touchScl >= 0 ||
                               display.touchInt >= 0 || display.touchRst >= 0))) {
            errorMsg = "DISPLAY_PINS_INVALID: SPI pins and enabled touch SDA/SCL are required";
            return false;
        }
        for (uint8_t i = 0; i < 32; ++i) {
            if (display.pinAt(i) < 0) continue;
            const uint8_t pin = static_cast<uint8_t>(display.pinAt(i));
            if (!isValidPin(pin) || isReservedPin(pin) ||
                (i != 8 && isInputOnlyPin(pin))) {
                errorMsg = String("DISPLAY_PIN_UNUSABLE: GPIO") + String(pin);
                return false;
            }
            for (uint8_t j = 0; j < i; ++j) {
                if (display.pinAt(j) == display.pinAt(i)) {
                    errorMsg = String("DISPLAY_PIN_DUPLICATED: GPIO") + String(pin);
                    return false;
                }
            }
            if (checkRunningGraph && config.enabled && checkPinConflict(pin, config.id) &&
                !((i == 6 || i == 7 || i >= 30) && canShareI2cPin(config, pin, config.id))) {
                errorMsg = String("DISPLAY_PIN_CONFLICT: GPIO") + String(pin);
                return false;
            }
        }
        return true;
    }

    if (config.pinCount == 0) {
        errorMsg = "至少需要配置一个引脚";
        return false;
    }

    // 引脚验证
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255) {
            // 使用增强的引脚验证
            if (!validatePinForType(config.pins[i], config.type, errorMsg)) {
                return false;
            }
        }
    }

    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive) {
        if (config.params.gpio.pwmChannel >= CHIP_MAX_PWM_CH ||
            config.params.gpio.pwmFrequency == 0 ||
            config.params.gpio.pwmResolution < 1 || config.params.gpio.pwmResolution > 16 ||
            (uint64_t)config.params.gpio.pwmFrequency *
                    (1ULL << config.params.gpio.pwmResolution) > 80000000ULL) {
            errorMsg = "无源蜂鸣器 PWM 参数无效";
            return false;
        }
    }

    // 类型特定参数验证
    switch (config.type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE:
            if (config.params.gpio.inputMode > static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)) {
                errorMsg = "DI 输入模式无效 (有效值: 0=自动, 1=浮空, 2=上拉, 3=下拉)";
                return false;
            }
            if (config.params.gpio.debounceMs > 5000) {
                errorMsg = "DI 去抖时间无效 (有效范围: 0-5000ms)";
                return false;
            }
            if (config.params.gpio.reportMode > static_cast<uint8_t>(DigitalReportMode::EVENT_ONLY)) {
                errorMsg = "DI 上报模式无效 (有效值: 0-2)";
                return false;
            }
            break;

        case PeripheralType::UART:
            // UART 波特率验证
            if (config.params.uart.baudRate == 0 || config.params.uart.baudRate > 5000000) {
                errorMsg = "UART 波特率无效 (有效范围: 1-5000000)";
                return false;
            }
            if (config.params.uart.dataBits < 5 || config.params.uart.dataBits > 8) {
                errorMsg = "UART 数据位无效 (有效值: 5-8)";
                return false;
            }
            if (config.params.uart.stopBits < 1 || config.params.uart.stopBits > 2) {
                errorMsg = "UART 停止位无效 (有效值: 1, 2)";
                return false;
            }
            if (config.params.uart.parity > 2) {
                errorMsg = "UART 校验位无效 (0=无, 1=奇, 2=偶)";
                return false;
            }
            break;

        case PeripheralType::I2C:
            // I2C 频率验证
            if (config.params.i2c.frequency != 100000 &&
                config.params.i2c.frequency != 400000 &&
                config.params.i2c.frequency != 1000000) {
                errorMsg = "I2C 频率无效 (支持: 100000, 400000, 1000000)";
                return false;
            }
            if (!config.params.i2c.isMaster && config.params.i2c.address == 0) {
                errorMsg = "I2C 从机模式需要设置地址";
                return false;
            }
            if (config.params.i2c.address > 127) {
                errorMsg = "I2C 地址无效 (有效范围: 0-127)";
                return false;
            }
            break;

        case PeripheralType::SPI:
            // SPI 频率验证
            if (config.params.spi.frequency == 0 || config.params.spi.frequency > 80000000) {
                errorMsg = "SPI 频率无效 (有效范围: 1-80000000)";
                return false;
            }
            if (config.params.spi.mode > 3) {
                errorMsg = "SPI 模式无效 (有效值: 0-3)";
                return false;
            }
            break;

        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
            // PWM 参数验证
            if (config.params.gpio.pwmChannel >= CHIP_MAX_PWM_CH) {
                errorMsg = "PWM 通道无效 (有效范围: 0-" + String(CHIP_MAX_PWM_CH - 1) + ")";
                return false;
            }
            if (config.params.gpio.pwmFrequency == 0 || config.params.gpio.pwmFrequency > 40000000) {
                errorMsg = "PWM 频率无效 (有效范围: 1-40000000)";
                return false;
            }
            if (config.params.gpio.pwmResolution < 1 || config.params.gpio.pwmResolution > 16) {
                errorMsg = "PWM 分辨率无效 (有效范围: 1-16 位)";
                return false;
            }
            // 检查频率和分辨率组合是否有效
            // ESP32: freq * (2^resolution) <= 80MHz
            {
                uint64_t maxFreqResProduct = 80000000ULL;
                uint64_t freqResProduct = (uint64_t)config.params.gpio.pwmFrequency * (1ULL << config.params.gpio.pwmResolution);
                if (freqResProduct > maxFreqResProduct) {
                    errorMsg = "PWM 频率和分辨率组合无效 (freq * 2^resolution 不能超过 80MHz)";
                    return false;
                }
            }
            break;

        case PeripheralType::PWM_SERVO:
            if (!isServoConfigValid(config)) {
                errorMsg = "舵机参数无效：频率 40-400Hz，脉宽 100-5000us，角度 0-360 度";
                return false;
            }
            if (config.params.servo.pwmChannel >= CHIP_MAX_PWM_CH) {
                errorMsg = "舵机 PWM 通道无效 (有效范围: 0-" + String(CHIP_MAX_PWM_CH - 1) + ")";
                return false;
            }
            if (static_cast<uint64_t>(config.params.servo.frequency) *
                    (1ULL << config.params.servo.pwmResolution) > 80000000ULL) {
                errorMsg = "舵机 PWM 频率和分辨率组合无效";
                return false;
            }
            break;

        case PeripheralType::ADC:
        case PeripheralType::GPIO_ANALOG_INPUT:
            // ADC 参数验证
            if (config.params.adc.attenuation > 3) {
                errorMsg = "ADC 衰减系数无效 (有效值: 0-3)";
                return false;
            }
            if (config.params.adc.resolution < 9 || config.params.adc.resolution > 12) {
                errorMsg = "ADC 分辨率无效 (有效范围: 9-12 位)";
                return false;
            }
            if (config.params.adc.sampleCount > 16) {
                errorMsg = "ADC 采样次数无效 (有效范围: 1-16)";
                return false;
            }
            if (config.params.adc.filterMode > 1) {
                errorMsg = "ADC 滤波模式无效 (有效值: 0=均值, 1=中值)";
                return false;
            }
            if (config.params.adc.signalMode > static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR) ||
                config.params.adc.decimalPlaces > 6) {
                errorMsg = "AI 转换参数无效";
                return false;
            }
            if (config.params.adc.conversionEnabled &&
                config.params.adc.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW)) {
                const auto& a = config.params.adc;
                if (isnan(a.engineeringMin) || isinf(a.engineeringMin) ||
                    isnan(a.engineeringMax) || isinf(a.engineeringMax) ||
                    a.engineeringMin == a.engineeringMax) {
                    errorMsg = "AI 工程量上下限不能相同或为空";
                    return false;
                }
                const bool hasExplicitInputRange =
                    a.inputMin != 0.0f || a.inputMax != 0.0f;
                if ((hasExplicitInputRange &&
                     (isnan(a.inputMin) || isinf(a.inputMin) ||
                      isnan(a.inputMax) || isinf(a.inputMax) ||
                      a.inputMax <= a.inputMin)) ||
                    (!isnan(a.voltageRatio) && !isinf(a.voltageRatio) && a.voltageRatio < 0.0f) ||
                    (!isnan(a.shuntResistance) && !isinf(a.shuntResistance) && a.shuntResistance < 0.0f) ||
                    (!isnan(a.inputGain) && !isinf(a.inputGain) && a.inputGain < 0.0f)) {
                    errorMsg = "AI 输入量程或校准参数无效";
                    return false;
                }
                if (a.signalMode == static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR) &&
                    a.rawMax > 0 && a.rawMax <= a.rawMin) {
                    errorMsg = "AI 自定义原始量程无效";
                    return false;
                }
            }
            break;

        case PeripheralType::DAC:
            // DAC 参数验证
            if (config.params.dac.channel != 1 && config.params.dac.channel != 2) {
                errorMsg = "DAC 通道无效 (有效值: 1, 2)";
                return false;
            }
            break;

        case PeripheralType::STEPPER_MOTOR:
            if (config.pinCount < 4) {
                errorMsg = "步进电机需要 4 个引脚 (IN1,IN2,IN3,IN4)";
                return false;
            }
            for (uint8_t i = 0; i < 4; i++) {
                if (isReservedPin(config.pins[i])) {
                    errorMsg = String("GPIO") + String(config.pins[i]) + " 是当前芯片保留引脚，不能用于步进电机";
                    return false;
                }
            }
            if (config.params.stepper.stepsPerRevolution == 0) {
                errorMsg = "步进电机每圈步数必须大于 0";
                return false;
            }
            if (config.params.stepper.speed > STEPPER_MAX_RPM) {
                errorMsg = "步进电机默认转速过高 (最大 " + String(STEPPER_MAX_RPM) + " RPM)";
                return false;
            }
            break;

        case PeripheralType::ENCODER:
            // 编码器参数验证
            if (config.pinCount < 2) {
                errorMsg = "编码器需要 2 个引脚 (A相, B相)";
                return false;
            }
            if (config.pins[0] == config.pins[1]) {
                errorMsg = "编码器 A相和B相不能使用同一引脚";
                return false;
            }
            if (config.params.encoder.resolution == 0) {
                errorMsg = "编码器每转脉冲数不能为0";
                return false;
            }
            if (!config.params.encoder.useInterrupt) {
                errorMsg = "ENCODER_INTERRUPT_REQUIRED: 当前固件仅支持 GPIO ISR 编码器模式";
                return false;
            }
            break;

        case PeripheralType::SDIO:
            if (config.params.sdcard.interface != SDCARD_INTERFACE_SPI) {
                errorMsg = "SDMMC_UNSUPPORTED: 当前固件仅支持 SDSPI";
                return false;
            }
            if (config.pinCount < 4) {
                errorMsg = "SDSPI 需要 4 个引脚 (SCK,MOSI,MISO,CS)";
                return false;
            }
            if (config.params.sdcard.frequency < 100000UL ||
                config.params.sdcard.frequency > 40000000UL) {
                errorMsg = "SDSPI 频率必须在 100kHz-40MHz";
                return false;
            }
            break;

        case PeripheralType::DS1302_RTC:
            if ((config.params.ds1302.utcOffsetMinutes != INT16_MIN &&
                 (config.params.ds1302.utcOffsetMinutes < -840 || config.params.ds1302.utcOffsetMinutes > 840)) ||
                (config.params.ds1302.bootSeed && config.params.ds1302.utcOffsetMinutes == INT16_MIN)) {
                errorMsg = "DS1302_BOOT_OFFSET_REQUIRED";
                return false;
            }
            if (config.pinCount < 3 || config.pins[0] == config.pins[1] ||
                config.pins[0] == config.pins[2] || config.pins[1] == config.pins[2]) {
                errorMsg = "DS1302 需要不同的 CE/IO/SCLK 三个引脚";
                return false;
            }
            switch (config.params.ds1302.trickleCharge) {
                case 0x00: case 0xA5: case 0xA6: case 0xA7:
                case 0xA9: case 0xAA: case 0xAB:
                    break;
                default:
                    errorMsg = "DS1302 涓流充电模式无效";
                    return false;
            }
            break;

        case PeripheralType::LCD1602:
            if (config.pinCount < 2 || config.pins[0] == config.pins[1]) {
                errorMsg = "LCD1602 需要不同的 SDA/SCL 两个引脚";
                return false;
            }
            if (!((config.params.lcd1602.cols == 16 && config.params.lcd1602.rows == 2) ||
                  (config.params.lcd1602.cols == 20 && config.params.lcd1602.rows == 4))) {
                errorMsg = "LCD1602 尺寸仅支持 16x2 或 20x4";
                return false;
            }
            if (config.params.lcd1602.address != 0 &&
                !((config.params.lcd1602.address >= 0x20 && config.params.lcd1602.address <= 0x27) ||
                  (config.params.lcd1602.address >= 0x38 && config.params.lcd1602.address <= 0x3F))) {
                errorMsg = "LCD1602 I2C 地址需为 0(自动)、0x20-0x27 或 0x38-0x3F";
                return false;
            }
            break;

        case PeripheralType::RFID_MFRC522:
            if (config.pinCount < 5) {
                errorMsg = "MFRC522 需要 5 个引脚 (SCK,MOSI,MISO,SS,RST)";
                return false;
            }
            for (uint8_t i = 0; i < 5; ++i) {
                for (uint8_t j = i + 1; j < 5; ++j) {
                    if (config.pins[i] == config.pins[j]) {
                        errorMsg = "MFRC522 的 SCK/MOSI/MISO/SS/RST 引脚不能重复";
                        return false;
                    }
                }
            }
            if (config.params.rfid.frequency != RFID_DEFAULT_FREQUENCY) {
                errorMsg = "MFRC522 SPI 频率固定为 4000000Hz";
                return false;
            }
            break;

        case PeripheralType::NEO_PIXEL:
            if (config.pinCount < 1) {
                errorMsg = "WS2812B 需要 1 个数据引脚";
                return false;
            }
            if (isReservedPin(config.pins[0]) || isInputOnlyPin(config.pins[0])) {
                errorMsg = String("GPIO") + String(config.pins[0]) + " 不适合输出 WS2812B 信号";
                return false;
            }
            if (config.params.neopixel.count == 0 || config.params.neopixel.count > NEOPIXEL_MAX_COUNT) {
                errorMsg = "WS2812B 灯珠数量无效 (1-" + String(NEOPIXEL_MAX_COUNT) + ")";
                return false;
            }
            break;

        case PeripheralType::RF_MODULE:
            if (config.pinCount < 1) {
                errorMsg = "RF module requires 1 data pin";
                return false;
            }
            if (config.params.rf.mode > RF_MODE_RX) {
                errorMsg = "RF module mode must be 0(TX) or 1(RX)";
                return false;
            }
            if (config.params.rf.mode == RF_MODE_TX &&
                (isReservedPin(config.pins[0]) || isInputOnlyPin(config.pins[0]))) {
                errorMsg = String("GPIO") + String(config.pins[0]) + " cannot output RF signal";
                return false;
            }
            if (config.params.rf.pulseWidth < RF_MIN_PULSE_WIDTH_US ||
                config.params.rf.pulseWidth > RF_MAX_PULSE_WIDTH_US) {
                errorMsg = "RF pulseWidth must be 100-2000 us";
                return false;
            }
            if (config.params.rf.repeat < RF_MIN_REPEAT ||
                config.params.rf.repeat > RF_MAX_REPEAT) {
                errorMsg = "RF repeat must be 1-20";
                return false;
            }
            if (config.params.rf.bitLength < RF_MIN_BIT_LENGTH ||
                config.params.rf.bitLength > RF_MAX_BIT_LENGTH) {
                errorMsg = "RF bitLength must be 1-32";
                return false;
            }
            break;

        case PeripheralType::RADAR_SENSOR:
            if (config.pinCount < 1) {
                errorMsg = "Radar sensor requires 1 OUT pin";
                return false;
            }
            if (config.params.radar.mode != RADAR_MODE_DIGITAL) {
                errorMsg = "Radar sensor mode currently supports digital OUT only";
                return false;
            }
            if (config.params.radar.debounceMs > 5000 || config.params.radar.holdMs > 60000) {
                errorMsg = "Radar debounceMs/holdMs is out of range";
                return false;
            }
            break;

        case PeripheralType::IR_RECEIVER:
            if (config.pinCount < 1) {
                errorMsg = "IR receiver requires 1 data pin";
                return false;
            }
#if !FASTBEE_ENABLE_IR_REMOTE
            errorMsg = "固件未启用红外接收支持 (FASTBEE_ENABLE_IR_REMOTE=0)";
            return false;
#else
            break;
#endif

        default:
            // 其他类型暂不需要特殊验证
            break;
    }

    return true;
}

