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
    params["pairedInputId"] = config.params.gpio.pairedInputId;
    params["pairDebounceMs"] = config.params.gpio.pairDebounceMs ? config.params.gpio.pairDebounceMs : 200;
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
        params["ioSource"] = config.params.gpio.ioSource;
        params["providerId"] = config.params.gpio.providerId;
        params["channel"] = config.params.gpio.channel;
        params["initialState"] = static_cast<int>(config.params.gpio.initialState);
        params["invertLogic"] = config.params.gpio.invertLogic;
        params["nativeControlEligible"] = config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
            !config.isExpanderChannel() && config.pinCount == 1 && !config.params.gpio.passive &&
            !config.params.gpio.invertLogic && config.params.gpio.initialState == GPIOState::STATE_HIGH;
    } else if (config.type == PeripheralType::PCA9685_PWM) {
        FastBee::serializePca9685Config(params, config);
    } else if (config.type == PeripheralType::IO_EXPANDER) {
        params["driver"] = config.params.ioExpander.driver;
        params["busPeripheralId"] = config.params.ioExpander.busPeripheralId;
        params["resetPeripheralId"] = config.params.ioExpander.resetPeripheralId;
        params["address"] = config.params.ioExpander.address;
        params["sampleInterval"] = config.params.ioExpander.sampleInterval;
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
        object["feature"] = feature;
        object["featureActive"] = FastBee::RuntimeFeatureManager::instance().shouldEnable(feature);
    }
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
        FastBee::serializeSensorConfig(params, config);
    }
}

void addDisplayParams(JsonObject params, const PeripheralConfig& config) {
    const DisplayConfigParams& display = config.params.display;
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
    JsonObject pins = params["pins"].to<JsonObject>();
    pins["mosi"] = display.mosi;
    pins["sclk"] = display.sclk;
    pins["cs"] = display.cs;
    pins["dc"] = display.dc;
    pins["rst"] = display.rst;
    pins["backlight"] = display.backlight;
    pins["touchSda"] = display.touchSda;
    pins["touchScl"] = display.touchScl;
    pins["touchInt"] = display.touchInt;
    pins["touchRst"] = display.touchRst;
    JsonArray rgbPins = pins["rgbDataPins"].to<JsonArray>();
    for (uint8_t i = 0; i < 16; ++i) rgbPins.add(display.rgbDataPins[i]);
    pins["rgbDe"] = display.rgbDe;
    pins["rgbVsync"] = display.rgbVsync;
    pins["rgbHsync"] = display.rgbHsync;
    pins["rgbPclk"] = display.rgbPclk;
    pins["controlSda"] = display.controlSda;
    pins["controlScl"] = display.controlScl;
}

void addPeripheralDiagnosticCapability(JsonObject target, const PeripheralConfig& config) {
    target["implemented"] = isPeripheralTypeImplemented(config.type);
    if (!config.enabled) target["diagnosticBlockedReason"] = "disabled";
    if (config.type == PeripheralType::UART) {
        String id = config.id;
        id.toLowerCase();
        if (id.startsWith("modbus") || id.startsWith("rs485")) {
            target["diagnosticBlockedReason"] = "protocol.modbus";
        }
    }
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
    data["id"] = id;
    data["groupToken"] = token;
    auto describe = [](JsonObject object, const PeripheralConfig& config) {
        object["id"] = config.id;
        object["name"] = config.name;
        object["type"] = static_cast<int>(config.type);
        object["enabled"] = config.enabled;
        addGenericPeripheralFields(object, config);
        JsonArray pins = object["pins"].to<JsonArray>();
        for (uint8_t i = 0; i < config.pinCount && i < 8; ++i) pins.add(config.pins[i]);
        if (config.type == PeripheralType::I2C) {
            object["frequency"] = config.params.i2c.frequency ? config.params.i2c.frequency : 100000;
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

const char* preflightOperationName(FastBee::PreflightOperation operation) {
    switch (operation) {
        case FastBee::PreflightOperation::ADD: return "add";
        case FastBee::PreflightOperation::UPDATE: return "update";
        case FastBee::PreflightOperation::ENABLE: return "enable";
        default: return "unsupported";
    }
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
    data["profileId"] = profile->key;
    data["profileName"] = profile->displayName;
    data["profileVersion"] = profile->version;
    data["activeProfileId"] = active ? active->key : "";
    data["comparison"] = active == nullptr || !sameText(profile->key, active->key);
    data["claimScope"] = "configuration";
    data["networkClaims"] = "w5500,cellular";
    // The legacy resource rows remain configuration-only. Runtime leases are
    // separate and always refer to the active firmware, even for comparisons.
    data["runtimeReservationsIncluded"] = false;
    data["configuredUseMeaning"] = "enabled_configuration";
    data["availableMeaning"] = "configuration_only_not_runtime_free";
    data["configurationStatus"] = "busy";
    data["resources"] = nullptr;
    data["diagnostics"] = nullptr;
    {
        MutexGuard resultLock(preflightMutex, 0);
        if (resultLock.isLocked()) {
            RecursiveMutexGuard managerLock(manager.getMutex(), 0);
            if (managerLock.isLocked()) {
                const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
                FastBee::PeripheralResourcePlanner planner(
                    *profile, makePreflightContext(*profile, FastBee::ACK_NONE, ctx->networkManager),
                    preflightResult, existing.data(), existing.size());
                for (const PeripheralConfig& config : existing) {
                    planner.addPeripheral(config, config.enabled ? FastBee::ClaimState::ACTIVE
                                                                 : FastBee::ClaimState::DORMANT);
                }
                data["configurationStatus"] = "observed";
                data["claimsTruncated"] = preflightResult.claimsTruncated;
                data["issuesTruncated"] = preflightResult.issuesTruncated;
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
        addPeripheralDiagnosticCapability(doc.as<JsonObject>(), config);
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
        itemDoc["id"] = config.id;
        itemDoc["name"] = config.name;
        itemDoc["type"] = static_cast<int>(config.type);
        itemDoc["typeName"] = getPeripheralTypeName(config.type);
        itemDoc["category"] = getCategoryName(getPeripheralCategory(config.type));
        itemDoc["enabled"] = config.enabled;
        addPeripheralDiagnosticCapability(itemDoc.as<JsonObject>(), config);
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
                    addUartRuntime(itemDoc.as<JsonObject>(), runtimeState, config.enabled);
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

    data["id"] = config->id;
    data["name"] = config->name;
    data["type"] = static_cast<int>(config->type);
    data["typeName"] = getPeripheralTypeName(config->type);
    data["enabled"] = config->enabled;
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
    addGenericPeripheralFields(data, *config);

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
            addUartRuntime(status, *runtimeState, config->enabled);
        } else if (config->type == PeripheralType::ENCODER) {
            EncoderReading reading;
            if (pm.getEncoderReading(id, reading)) addEncoderReading(status, reading);
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
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

    data["id"] = id;
    data["enabled"] = config.enabled;
    if (config.type == PeripheralType::PCA9685_PWM &&
        !pm.getPca9685Status(id, data["pca9685"].to<JsonObject>())) {
        ctx->sendError(request, 503, "PCA_STATUS_BUSY"); return;
    }
    data["status"] = hasRuntimeState ? static_cast<int>(runtimeState.status) : 0;
    data["groupActivation"] = config.type == PeripheralType::IO_EXPANDER &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0;
    data["memorySuspended"] = hasRuntimeState && runtimeState.memorySuspended;
    data["controlOutputStatusAvailable"] = hasRuntimeState;
    if (hasRuntimeState) {
        data["controlOutputClaimed"] = runtimeState.controlOutputClaimed;
        data["controlOutputFault"] = runtimeState.controlOutputFault;
        data["controlOutputRetiring"] = runtimeState.controlOutputRetiring;
        data["controlOutputRetirementAttempts"] = runtimeState.controlOutputRetirementAttempts;
        data["controlOutputRetirementError"] = runtimeState.controlOutputRetirementFailed
            ? "CONTROL_OUTPUT_RETIRE_UNCONFIRMED" : "";
    }

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
        data["initialized"] = rtcAccess.driver() != nullptr;
        if (rtcAccess.driver()) data["running"] = rtcAccess.driver()->isRunning();
        else data["driverError"] = rtcAccess.error();
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
        RFIDDriver::Status rfidStatus;
        String rfidStatusError;
        const bool available = RFIDDriver::getInstance().getStatus(rfidStatus, rfidStatusError);
        data["statusAvailable"] = available;
        if (available) {
            data["initialized"] = rfidStatus.initialized;
            data["cardPresent"] = rfidStatus.cardPresent;
            data["uid"] = rfidStatus.uid;
            data["cardType"] = rfidStatus.cardType;
            if (!rfidStatus.lastError.isEmpty()) data["driverError"] = rfidStatus.lastError;
        } else {
            data["driverError"] = rfidStatusError;
        }
#else
        data["initialized"] = false;
#endif
    } else if (config.type == PeripheralType::SENSOR_GENERIC &&
               config.params.sensor.driver[0] != '\0') {
        data["driver"] = config.params.sensor.driver;
        data["address"] = config.params.sensor.address;
        data["mode"] = config.params.sensor.mode;
        SensorReading sample;
        bool stale = true;
        String sampleError;
        const bool sampled = FastBee::SensorDriverCache::instance().getSnapshot(id, sample, stale, sampleError);
        data["sampled"] = sampled;
        data["stale"] = stale;
        data["timestamp"] = sampled ? sample.timestamp : 0;
        if (!sampleError.isEmpty()) data["driverError"] = sampleError;
        if (sampled) {
            const auto* metadata = FastBee::DriverRegistry::getInstance().getMetadata(config.params.sensor.driver);
            JsonArray channels = data["channels"].to<JsonArray>();
            for (uint8_t i = 0; metadata && i < sample.channelCount && i < 4; ++i) {
                const auto* descriptor = metadata->getChannel(i, config.params.sensor.mode);
                if (!descriptor) break;
                JsonObject channel = channels.add<JsonObject>();
                channel["name"] = descriptor->name;
                channel["unit"] = descriptor->unit;
                channel["value"] = sample.values[i];
                channel["valid"] = !stale;
            }
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
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
            data["configEpoch"] = writeConfigVersion;
        }
        response["success"] = success;
        if (!success) response["error"] = error;
        data["id"] = id; data["physicalOutputVerified"] = false;
        data["statusAvailable"] = pm.getPca9685Status(id, data["status"].to<JsonObject>());
        if (response.overflowed()) { ctx->sendError(request, 503, "PCA_RESPONSE_UNCONFIRMED"); return; }
        HandlerUtils::sendJsonStream(request, response);
        return;
    }

    if (config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] != '\0' &&
        ctx->getParamValue(request, "action", "").equalsIgnoreCase("release")) {
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
        if (action.equalsIgnoreCase("mount")) {
            success = SDCardManager::getInstance().mount(errorMsg, id);
            message = success ? "SD card mounted" : errorMsg;
        } else if (action.equalsIgnoreCase("recover")) {
            success = storage.driver()->recover(id, errorMsg);
            message = success ? "SD remounted; prior file operation was not replayed" : errorMsg;
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
                content.length(), append, errorMsg, id);
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
            success = lcd.printAt(static_cast<uint8_t>(col), static_cast<uint8_t>(row), content);
            message = success ? "LCD1602 text printed" : "LCD1602_PRINT_FAILED";
        } else if (action.equalsIgnoreCase("clear")) {
            success = lcd.clear();
            message = success ? "LCD1602 cleared" : "LCD1602_CLEAR_UNCONFIRMED";
        } else if (action.equalsIgnoreCase("backlight")) {
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

