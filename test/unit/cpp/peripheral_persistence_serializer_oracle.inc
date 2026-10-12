void serializePersistedPeripheral(JsonObject obj, const PeripheralConfig& config) {
    obj["id"] = config.id;
    obj["name"] = config.name;
    obj["type"] = static_cast<int>(config.type);
    obj["enabled"] = config.enabled;

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
        params["ioSource"] = config.params.gpio.ioSource;
        params["providerId"] = config.params.gpio.providerId;
        params["channel"] = config.params.gpio.channel;
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        params["driver"] = config.params.ioExpander.driver;
        params["busPeripheralId"] = config.params.ioExpander.busPeripheralId;
        params["resetPeripheralId"] = config.params.ioExpander.resetPeripheralId;
        params["address"] = config.params.ioExpander.address;
        params["sampleInterval"] = config.params.ioExpander.sampleInterval;
    }

    if (config.type == PeripheralType::UART) {
        params["baudRate"] = config.params.uart.baudRate;
        params["dataBits"] = config.params.uart.dataBits;
        params["stopBits"] = config.params.uart.stopBits;
        params["parity"] = config.params.uart.parity;
    }
    else if (config.type == PeripheralType::ENCODER) {
        params["resolution"] = config.params.encoder.resolution;
        params["useInterrupt"] = config.params.encoder.useInterrupt;
    }
    else if (config.type == PeripheralType::I2C) {
        params["frequency"] = config.params.i2c.frequency;
        params["address"] = config.params.i2c.address;
        params["isMaster"] = config.params.i2c.isMaster;
    }
    else if (config.type == PeripheralType::SPI) {
        params["frequency"] = config.params.spi.frequency;
        params["mode"] = config.params.spi.mode;
        params["msbFirst"] = config.params.spi.msbFirst;
    }
    else if (config.type == PeripheralType::PWM_SERVO) {
        params["frequency"] = config.params.servo.frequency;
        params["minPulse"] = config.params.servo.minPulse;
        params["maxPulse"] = config.params.servo.maxPulse;
        params["minAngle"] = config.params.servo.minAngle;
        params["maxAngle"] = config.params.servo.maxAngle;
        params["initialAngle"] = config.params.servo.initialAngle;
        params["pwmChannel"] = config.params.servo.pwmChannel;
        params["pwmResolution"] = config.params.servo.pwmResolution;
        params["inverted"] = config.params.servo.inverted;
        params["releaseAfterMove"] = config.params.servo.releaseAfterMove;
    }
    else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
        params["initialState"] = static_cast<int>(config.params.gpio.initialState);
        params["pwmChannel"] = config.params.gpio.pwmChannel;
        params["pwmFrequency"] = config.params.gpio.pwmFrequency;
        params["pwmResolution"] = config.params.gpio.pwmResolution;
        params["defaultDuty"] = config.params.gpio.defaultDuty;
        if (config.params.gpio.passive) params["passive"] = true;
        // Logical PCF8574 outputs also use invertLogic to map the
        // configured startup state to the physical channel level.
        if (config.isExpanderChannel() && config.params.gpio.invertLogic) {
            params["invertLogic"] = true;
        }
        // AO 模拟量输出模式（非默认值时才写入，保持配置精简）
        if (config.params.gpio.aoMode) {
            params["aoMode"] = true;
            params["aoMinPct"] = config.params.gpio.aoMinPct;
            params["aoMaxPct"] = config.params.gpio.aoMaxPct;
        }
        // DI 参数只在非默认值时写入，保持旧配置文件紧凑。
        if (config.type == PeripheralType::GPIO_DIGITAL_INPUT ||
            config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
            config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
            config.type == PeripheralType::GPIO_INTERRUPT_RISING ||
            config.type == PeripheralType::GPIO_INTERRUPT_FALLING ||
            config.type == PeripheralType::GPIO_INTERRUPT_CHANGE) {
            if (config.params.gpio.inputMode != static_cast<uint8_t>(DigitalInputMode::AUTO)) {
                params["inputMode"] = config.params.gpio.inputMode;
            }
            if (config.params.gpio.invertLogic) params["invertLogic"] = true;
            if (config.params.gpio.debounceMs > 0) params["debounceMs"] = config.params.gpio.debounceMs;
            if (config.params.gpio.pairedInputId[0]) {
                params["pairedInputId"] = config.params.gpio.pairedInputId;
                params["pairDebounceMs"] = config.params.gpio.pairDebounceMs;
            }
            if (config.params.gpio.reportMode != static_cast<uint8_t>(DigitalReportMode::ON_CHANGE)) {
                params["reportMode"] = config.params.gpio.reportMode;
            }
        }
    }
    else if (isAdcInputType(config.type)) {
        params["attenuation"] = config.params.adc.attenuation;
        params["resolution"] = config.params.adc.resolution;
        params["sampleRate"] = config.params.adc.sampleRate;
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
            params["signalMode"] = config.params.adc.signalMode;
            params["conversionEnabled"] = config.params.adc.conversionEnabled;
            params["clampEnabled"] = config.params.adc.clampEnabled;
            params["calibratedVoltage"] = config.params.adc.calibratedVoltage;
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
    else if (config.type == PeripheralType::DAC) {
        params["channel"] = config.params.dac.channel;
    }
    else if (config.type == PeripheralType::STEPPER_MOTOR) {
        params["stepsPerRevolution"] = config.params.stepper.stepsPerRevolution;
        params["speed"] = config.params.stepper.speed;
    }
    else if (config.type == PeripheralType::SDIO) {
        params["interface"] = config.params.sdcard.interface;
        params["frequency"] = config.params.sdcard.frequency;
    }
    else if (config.type == PeripheralType::DS1302_RTC) {
        params["use24Hour"] = config.params.ds1302.use24Hour;
        params["trickleCharge"] = config.params.ds1302.trickleCharge;
    }
    else if (config.type == PeripheralType::LCD1602) {
        params["cols"] = config.params.lcd1602.cols;
        params["rows"] = config.params.lcd1602.rows;
        params["address"] = config.params.lcd1602.address;
        params["backlight"] = config.params.lcd1602.backlight;
    }
    else if (config.type == PeripheralType::RFID_MFRC522) {
        params["frequency"] = config.params.rfid.frequency;
    }
    else if (config.type == PeripheralType::LCD) {
        params["width"]     = config.params.lcd.width;
        params["height"]    = config.params.lcd.height;
        params["interface"] = config.params.lcd.interface;
        params["contrast"]  = config.params.lcd.contrast;
    }
    else if (config.type == PeripheralType::SCREEN_DISPLAY) {
        const auto& display = config.params.display;
        params["backend"] = display.backend;
        params["driver"] = display.driver;
        params["touchBackend"] = display.touchBackend;
        params["width"] = display.width;
        params["height"] = display.height;
        params["rotation"] = display.rotation;
        params["colorDepth"] = display.colorDepth;
        params["bufferLines"] = display.bufferLines;
        params["brightness"] = display.brightness;
        params["backlightChannel"] = display.backlightChannel;
        params["backlightActiveHigh"] = display.backlightActiveHigh;
        params["spiFrequency"] = display.spiFrequency;
        params["i2cFrequency"] = display.i2cFrequency;
        params["rgbInitProfile"] = display.rgbInitProfile;
        params["rgbFrequency"] = display.rgbFrequency;
        params["hsyncFrontPorch"] = display.hsyncFrontPorch;
        params["hsyncPulseWidth"] = display.hsyncPulseWidth;
        params["hsyncBackPorch"] = display.hsyncBackPorch;
        params["vsyncFrontPorch"] = display.vsyncFrontPorch;
        params["vsyncPulseWidth"] = display.vsyncPulseWidth;
        params["vsyncBackPorch"] = display.vsyncBackPorch;
        params["hsyncIdleLow"] = display.hsyncIdleLow;
        params["vsyncIdleLow"] = display.vsyncIdleLow;
        params["pclkActiveNeg"] = display.pclkActiveNeg;
        params["controlBackend"] = display.controlBackend;
        params["controlAddress"] = display.controlAddress;
        params["controlResetMask"] = display.controlResetMask;
        params["controlBacklightMask"] = display.controlBacklightMask;
        params["controlEnableMask"] = display.controlEnableMask;
        params["controlBuzzerMask"] = display.controlBuzzerMask;
        params["touchBuzzerDurationMs"] = display.touchBuzzerDurationMs;
        params["controlInactiveMask"] = display.controlInactiveMask;
        params["controlTouchIntMask"] = display.controlTouchIntMask;
        JsonObject namedPins = params["pins"].to<JsonObject>();
        namedPins["mosi"] = display.mosi;
        namedPins["sclk"] = display.sclk;
        namedPins["cs"] = display.cs;
        namedPins["dc"] = display.dc;
        namedPins["rst"] = display.rst;
        namedPins["backlight"] = display.backlight;
        namedPins["touchSda"] = display.touchSda;
        namedPins["touchScl"] = display.touchScl;
        namedPins["touchInt"] = display.touchInt;
        namedPins["touchRst"] = display.touchRst;
        JsonArray rgbPins = namedPins["rgbDataPins"].to<JsonArray>();
        for (uint8_t i = 0; i < 16; ++i) rgbPins.add(display.rgbDataPins[i]);
        namedPins["rgbDe"] = display.rgbDe;
        namedPins["rgbVsync"] = display.rgbVsync;
        namedPins["rgbHsync"] = display.rgbHsync;
        namedPins["rgbPclk"] = display.rgbPclk;
        namedPins["controlSda"] = display.controlSda;
        namedPins["controlScl"] = display.controlScl;
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        params["brightness"] = config.params.segment.brightness;
    }
    else if (config.type == PeripheralType::NEO_PIXEL) {
        params["count"] = clampNeoPixelCount(config.params.neopixel.count);
        params["brightness"] = config.params.neopixel.brightness;
    }
    else if (config.type == PeripheralType::RF_MODULE) {
        params["mode"] = config.params.rf.mode;
        params["pulseWidth"] = clampRfPulseWidth(config.params.rf.pulseWidth);
        params["repeat"] = clampRfRepeat(config.params.rf.repeat);
        params["bitLength"] = clampRfBitLength(config.params.rf.bitLength);
        params["activeHigh"] = config.params.rf.activeHigh;
    }
    else if (config.type == PeripheralType::RADAR_SENSOR) {
        params["mode"] = config.params.radar.mode;
        params["activeHigh"] = config.params.radar.activeHigh;
        params["debounceMs"] = config.params.radar.debounceMs;
        params["holdMs"] = config.params.radar.holdMs;
    }
    else if (config.type == PeripheralType::SENSOR_GENERIC) {
        FastBee::serializeSensorConfig(params, config);
    }
}
