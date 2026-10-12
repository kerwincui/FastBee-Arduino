#include "network/handlers/PeripheralRouteHandler.h"
#include "network/LiteralStringMatch.h"
#include "network/FixedJsonField.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "PeripheralRouteReadSupport.h"
#include "core/FeatureFlags.h"
#include "core/AsyncExecTypes.h"
#include "core/PeripheralManager.h"
#include "core/SensorDriverCache.h"
#include "core/SensorConfigSupport.h"
#include "core/RuntimeFeatureManager.h"
#include "core/I2CBusManager.h"
#include "core/SdAccess.h"
#include "peripherals/SDCardManager.h"
#include "network/SDCardStatusJson.h"
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
#include <math.h>
#include <vector>

using namespace FastBee::PeripheralRouteReadDetail;

void PeripheralRouteHandler::handleReadPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    uint32_t sensorConfigVersion = 0;
    if (!pm.getPeripheralSnapshot(id, config, &sensorConfigVersion)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    // A legacy logical sensor's physical category belongs to its rule action.
    // Its free-form sensorType ID cannot select a driver for this endpoint.
    if (config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] == '\0') {
        ctx->sendError(request, 409,
            "SENSOR_RULE_READ_REQUIRED: read logical sensor values from the configured sensor rule results");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    FastBee::setFixedJsonField(data, "id", id);
    FastBee::setFixedJsonField(data, "type", static_cast<int>(config.type));
    FastBee::setFixedJsonField(data, "typeName", getPeripheralTypeName(config.type));

    const String requestedAction = ctx->getParamValue(request, "action", "");
    const bool localI2cSensor =
        config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] != '\0';

    if (localI2cSensor && !FastBee::RuntimeFeatureManager::instance().shouldEnable("i2cSensors")) {
        ctx->sendError(request, 409, "I2C_SENSORS_UNAVAILABLE: enable the feature and reboot");
        return;
    }

    if ((config.type == PeripheralType::I2C || localI2cSensor) &&
        FastBee::stringEqualsIgnoreCaseLiteral(requestedAction, "scan")) {
        if (!config.enabled || (config.type == PeripheralType::I2C &&
                               (!config.params.i2c.isMaster || config.params.i2c.address != 0))) {
            ctx->sendError(request, 409,
                           "I2C_NOT_READY: enable an I2C peripheral with SDA/SCL first");
            return;
        }
        uint8_t sda = 255, scl = 255;
        uint32_t frequency = 0;
        String error;
        if (!pm.resolveI2cConfig(config, sda, scl, frequency, error)) {
            ctx->sendError(request, 409, error);
            return;
        }
        struct ScanLease {
            bool acquired = false;
            ~ScanLease() {
                if (!acquired) return;
                auto& bus = FastBee::I2CBusManager::instance();
                if (!bus.release(this, 2)) bus.deferRelease(this);
            }
        } lease;
        auto& bus = FastBee::I2CBusManager::instance();
        {
            FastBee::I2CBusLock lock(2);
            if (!lock.locked() || !bus.acquire(&lease, sda, scl, frequency, error)) {
                ctx->sendError(request, 409, error.isEmpty() ? String("I2C_BUSY") : error);
                return;
            }
            lease.acquired = true;
        }
        TwoWire* wire = bus.wire(&lease);
        if (!wire) { ctx->sendError(request, 409, "I2C_NOT_READY"); return; }
        int firstAddress = ctx->getParamInt(request, "startAddress", 1);
        if (firstAddress < 1 || firstAddress > 126) {
            ctx->sendError(request, 400, "I2C_SCAN_ADDRESS_INVALID"); return;
        }
        JsonArray addresses = data["addresses"].to<JsonArray>();
        const uint32_t started = static_cast<uint32_t>(millis());
        uint8_t address = static_cast<uint8_t>(firstAddress);
        for (; address < 0x7F; ++address) {
            if (static_cast<uint32_t>(millis() - started) >= 150) break;
            {
                // Release between every probe so IO sampling can continue.
                FastBee::I2CBusLock lock(2);
                if (!lock.locked()) break;
                wire->beginTransmission(address);
                if (wire->endTransmission() == 0) addresses.add(address);
            }
            delay(0);
        }
        FastBee::setFixedJsonField(data, "truncated", address < 0x7F);
        if (address < 0x7F) FastBee::setFixedJsonField(data, "nextAddress", address);
        FastBee::setFixedJsonField(data, "count", addresses.size());
        FastBee::setFixedJsonField(data, "sda", sda);
        FastBee::setFixedJsonField(data, "scl", scl);
    } else if (config.type == PeripheralType::SDIO) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "SD_NOT_READY: enable and mount the SD peripheral first");
            return;
        }
        FastBee::SdAccess storage(id, sensorConfigVersion);
        if (!storage.driver()) { ctx->sendError(request, 409, storage.error()); return; }
        const String action = ctx->getParamValue(request, "action", "status");
        String errorMsg;
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "status")) {
            const SDCardStatus status = SDCardManager::getInstance().getStatus();
            FastBee::writeSdCardStatusJson(data, status, id);
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "list")) {
            std::vector<SDCardEntry> entries;
            bool truncated = false;
            const String path = ctx->getParamValue(request, "path", "/fastbee");
            if (!SDCardManager::getInstance().list(path, entries, errorMsg, id, &truncated)) {
                ctx->sendError(request, 400, errorMsg);
                return;
            }
            JsonArray items = data["entries"].to<JsonArray>();
            for (const SDCardEntry& entry : entries) {
                JsonObject item = items.add<JsonObject>();
                FastBee::setFixedJsonField(item, "path", entry.path);
                FastBee::setFixedJsonField(item, "directory", entry.directory);
                FastBee::setFixedJsonField(item, "size", entry.size);
            }
            FastBee::setFixedJsonField(data, "path", path);
            FastBee::setFixedJsonField(data, "count", entries.size());
            FastBee::setFixedJsonField(data, "truncated", truncated);
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "read")) {
            const String path = ctx->getParamValue(request, "path", "");
            int requestedMax = ctx->getParamInt(request, "maxBytes", 4096);
            if (requestedMax < 1) requestedMax = 1;
            if (requestedMax > 4096) requestedMax = 4096;
            String content;
            if (!SDCardManager::getInstance().readFile(
                    path, content, errorMsg, static_cast<size_t>(requestedMax), id)) {
                ctx->sendError(request, 400, errorMsg);
                return;
            }
            FastBee::setFixedJsonField(data, "path", path);
            FastBee::setFixedJsonField(data, "content", content);
            FastBee::setFixedJsonField(data, "length", content.length());
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
        FastBee::RtcAccess rtcAccess(id);
        if (!rtcAccess.driver()) {
            ctx->sendError(request, 409, rtcAccess.error());
            return;
        }
        DS1302Driver& rtc = *rtcAccess.driver();
        const DS1302ClockSnapshot snapshot = rtc.readClockSnapshot();
        const DS1302DateTime& dt = snapshot.dateTime;
        if (!snapshot.valid) {
            ctx->sendError(request, 502, "DS1302_INVALID_DATA: clock returned an invalid date/time");
            return;
        }
        FastBee::setFixedJsonField(data, "year", 2000 + dt.year);
        FastBee::setFixedJsonField(data, "month", dt.month);
        FastBee::setFixedJsonField(data, "day", dt.day);
        FastBee::setFixedJsonField(data, "hour", dt.hour);
        FastBee::setFixedJsonField(data, "minute", dt.minute);
        FastBee::setFixedJsonField(data, "second", dt.second);
        FastBee::setFixedJsonField(data, "dayOfWeek", dt.dayOfWeek);
        FastBee::setFixedJsonField(data, "formatted", dt.toString());
        FastBee::setFixedJsonField(data, "running", snapshot.running);
        FastBee::setFixedJsonField(data, "use24Hour", snapshot.mode24);
        FastBee::setFixedJsonField(data, "timeBasis", "device_calendar_unzoned");
        FastBee::setFixedJsonField(data, "quality", snapshot.running ? "calendar_unverified" : "halted");
#else
        ctx->sendError(request, 501, "DS1302_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        FastBee::setFixedJsonField(data, "initialized", LCD1602Driver::getInstance().isInitialized());
        FastBee::setFixedJsonField(data, "cols", LCD1602Driver::getInstance().getCols());
        FastBee::setFixedJsonField(data, "rows", LCD1602Driver::getInstance().getRows());
#else
        FastBee::setFixedJsonField(data, "initialized", false);
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
        FastBee::setFixedJsonField(data, "uid", uid);
        FastBee::setFixedJsonField(data, "cardType", cardType);
        FastBee::setFixedJsonField(data, "cardPresent", true);
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
            !FastBee::stringEqualsIgnoreCaseLiteral(requestedAction, "read") &&
            !FastBee::stringEqualsIgnoreCaseLiteral(requestedAction, "probe")) {
            ctx->sendError(request, 400,
                           "SENSOR_ACTION_INVALID: expected scan, probe or read");
            return;
        }
        const String requestedChannel = ctx->getParamValue(request, "channel", "");
        String channel, errorMsg;
        uint8_t sda = 255, scl = 255;
        uint32_t frequency = 0;
        if (!FastBee::validateSensorConfig(config, errorMsg) ||
            !pm.resolveI2cConfig(config, sda, scl, frequency, errorMsg) ||
            !FastBee::resolveSensorChannel(config.params.sensor.driver, config.params.sensor.mode,
                                           requestedChannel.c_str(), channel, errorMsg, config.params.sensor.scanMask)) {
            ctx->sendError(request, 409, errorMsg);
            return;
        }
        JsonDocument driverDoc;
        FastBee::buildSensorDriverParams(driverDoc, config, sda, scl, frequency);
        if (driverDoc.overflowed()) { ctx->sendError(request, 503, "SENSOR_PARAMS_MEMORY_UNAVAILABLE"); return; }
        String driverParams;
        serializeJson(driverDoc, driverParams);
        float value = NAN;
        String unit;
        uint32_t sampleTimestamp = 0;
        FastBee::SensorSampleMetadata sample;
        if (!FastBee::SensorDriverCache::instance().readChannel(
                id, config.params.sensor.driver, sda,
                driverParams.c_str(), channel.c_str(), value, unit, errorMsg,
                FastBee::sensorSampleIntervalMs(config), &sampleTimestamp, sensorConfigVersion, nullptr, &sample)) {
            ctx->sendError(request, errorMsg == "SENSOR_CONFIG_CHANGED" ? 409 : 502, errorMsg);
            return;
        }
        const bool committed = pm.commitSensorRead(sensorConfigVersion, [&]() {
            FastBee::setFixedJsonField(data, "driver", config.params.sensor.driver);
            FastBee::setFixedJsonField(data, "channel", channel);
            FastBee::setFixedJsonField(data, "value", value);
            FastBee::setFixedJsonField(data, "unit", unit);
            FastBee::setFixedJsonField(data, "timestamp", sampleTimestamp);
            FastBee::setFixedJsonField(data, "sampledAtMs", sample.sampledAtMs);
            FastBee::setFixedJsonField(data, "ageMs", sample.capturedAtMs - sample.sampledAtMs);
            FastBee::setFixedJsonField(data, "maxAgeMs", sample.maxAgeMs);
            FastBee::setFixedJsonField(data, "quality", "good");
            FastBee::setFixedJsonField(data, "sampleInterval", FastBee::sensorSampleIntervalMs(config));
            FastBee::setFixedJsonField(data, "probed", FastBee::stringEqualsIgnoreCaseLiteral(requestedAction, "probe"));
            return !doc.overflowed();
        });
        if (!committed) {
            ctx->sendError(request, 409, "SENSOR_CONFIG_CHANGED: retry with the current configuration");
            return;
        }
    } else if (config.type == PeripheralType::ENCODER) {
        EncoderReading reading;
        if (!pm.getEncoderReading(id, reading)) {
            ctx->sendError(request, 409, "ENCODER_NOT_READY: enable and initialize the encoder first");
            return;
        }
        addEncoderReading(data, reading);
        FastBee::setFixedJsonField(data, "value", reading.count);
        FastBee::setFixedJsonField(data, "unit", "count");
    } else if (config.type == PeripheralType::PWM_SERVO) {
        PeripheralRuntimeState runtimeState;
        if (pm.getRuntimeStateSnapshot(id, runtimeState)) {
            FastBee::setFixedJsonField(data, "angle", runtimeState.state.servo.currentAngle);
            FastBee::setFixedJsonField(data, "pulseUs", runtimeState.state.servo.currentPulseUs);
            FastBee::setFixedJsonField(data, "value", runtimeState.state.servo.currentAngle);
        } else {
            FastBee::setFixedJsonField(data, "angle", config.params.servo.initialAngle);
            data["pulseUs"] = servoAngleToPulseUs(
                config.params.servo.initialAngle,
                config.params.servo.minAngle, config.params.servo.maxAngle,
                config.params.servo.minPulse, config.params.servo.maxPulse,
                config.params.servo.inverted);
            FastBee::setFixedJsonField(data, "value", config.params.servo.initialAngle);
        }
        FastBee::setFixedJsonField(data, "unit", "deg");
    } else if (isAnalogInputConfigType(config.type)) {
        // AI 读取同时返回原始 ADC 计数和工程量，便于调试接线/量程；
        // conversionEnabled 关闭时 value 与 rawValue 保持一致，兼容旧客户端。
        AnalogReadSample sample;
        const bool acquired = pm.readAnalogSample(id, sample);
        if (acquired) FastBee::setFixedJsonField(data, "rawValue", sample.raw);
        else FastBee::setFixedJsonField(data, "rawValue", nullptr);
        FastBee::setFixedJsonField(data, "conversionEnabled", sample.conversionEnabled);
        FastBee::setFixedJsonField(data, "signalMode", sample.signalMode);
        // Reuse the same ADC sample for rawValue and engineering conversion;
        // taking a second sample can make the two values disagree and doubles
        // the cost of filtered reads (up to 16 checked SDK calls).
        const AnalogInputQuality quality = sample.quality;
        const float pinVoltage = sample.pinVoltage;
        const float converted = sample.value;
        FastBee::setFixedJsonField(data, "calibratedVoltage", sample.calibratedVoltage);
        FastBee::setFixedJsonField(data, "adcPin", sample.pin);
        // Keep the MCU pin voltage distinct from an external terminal meter
        // reading: the latter also depends on the actual board front end.
        data["pinVoltageSource"] = sample.calibratedVoltage
            ? "chip_calibrated_adc" : "linear_adc_estimate";
        if (!isnan(pinVoltage) && !isinf(pinVoltage)) FastBee::setFixedJsonField(data, "pinVoltageMv", pinVoltage * 1000.0f);
        else FastBee::setFixedJsonField(data, "pinVoltageMv", nullptr);
        const float calculatedCurrentMa = sample.calculatedCurrentMa;
        if (!isnan(calculatedCurrentMa) && !isinf(calculatedCurrentMa))
            FastBee::setFixedJsonField(data, "calculatedCurrentMa", calculatedCurrentMa);
        else FastBee::setFixedJsonField(data, "calculatedCurrentMa", nullptr);
        FastBee::setFixedJsonField(data, "quality", analogInputQualityName(quality));
        FastBee::setFixedJsonField(data, "qualityCode", static_cast<uint8_t>(quality));
        if (isnan(converted) || isinf(converted)) {
            FastBee::setFixedJsonField(data, "value", nullptr);
            FastBee::setFixedJsonField(data, "valueValid", false);
        } else {
            FastBee::setFixedJsonField(data, "value", converted);
            FastBee::setFixedJsonField(data, "valueValid", true);
        }
    } else if (config.isGPIOPeripheral()) {
        GPIOState state = pm.readPin(id);
        FastBee::setFixedJsonField(data, "state", static_cast<int>(state));
        FastBee::setFixedJsonField(data, "stateName", (state == GPIOState::STATE_HIGH) ? "HIGH" : (state == GPIOState::STATE_LOW) ? "LOW" : "UNDEFINED");
    } else if (config.isCommunicationPeripheral()) {
        uint8_t buffer[UART_API_MAX_BYTES];
        size_t len = sizeof(buffer);
        bool readOk = false;
        PeripheralRuntimeState uartRuntime;
        const char* readError = nullptr;
        if (config.type == PeripheralType::UART) {
            // Pin configuration, byte consumption and counters to one manager
            // generation. No Web send or retry while this lock is held.
            RecursiveMutexGuard lock(pm.getMutex(), 0);
            PeripheralConfig current;
            uint32_t currentVersion = 0;
            if (!lock.isLocked()) readError = "UART_BUSY: RX was not consumed";
            else if (!pm.getPeripheralSnapshot(id, current, &currentVersion) ||
                     currentVersion != sensorConfigVersion || current.type != PeripheralType::UART)
                readError = "UART_CONFIG_CHANGED: RX was not consumed";
            else if (!current.enabled || !pm.getRuntimeStateSnapshot(id, uartRuntime) ||
                     uartRuntime.memorySuspended || !uartRuntime.state.comm.isConnected)
                readError = "UART_NOT_READY: RX was not consumed";
            else {
                readOk = pm.readData(id, buffer, len);
                if (!readOk) readError = "UART_READ_FAILED: RX may have been consumed; no automatic retry";
                else if (!pm.getRuntimeStateSnapshot(id, uartRuntime))
                    readError = "UART_STATE_UNAVAILABLE: RX may have been consumed";
            }
        } else {
            readOk = pm.readData(id, buffer, len);
            if (!readOk) readError = "COMM_READ_FAILED: transfer result unavailable";
        }
        if (readError) { ctx->sendError(request, 409, readError); return; }
        FastBee::setFixedJsonField(data, "readQuality", len ? "bytes" : "no_data");
        FastBee::setFixedJsonField(data, "frameVerified", false);
        if (config.type == PeripheralType::UART && len) FastBee::setFixedJsonField(data, "receivedAtUptimeMs", static_cast<uint32_t>(millis()));

        if (readOk && len > 0) {
            bool isText = true;
            for (size_t i = 0; i < len; i++) {
                const bool printableAscii = buffer[i] >= 32 && buffer[i] <= 126;
                if (!printableAscii && buffer[i] != '\n' && buffer[i] != '\r' && buffer[i] != '\t') {
                    isText = false;
                    break;
                }
            }

            if (isText) {
                FastBee::setFixedJsonField(data, "text", String((char*)buffer, len));
            }

            String hex;
            if (!hex.reserve(len * 3)) {
                ctx->sendError(request, 503, "COMM_RESPONSE_MEMORY_UNAVAILABLE: received bytes may have been consumed");
                return;
            }
            for (size_t i = 0; i < len; i++) {
                char hexBuf[4];
                snprintf(hexBuf, sizeof(hexBuf), i + 1 < len ? "%02X " : "%02X", buffer[i]);
                hex += hexBuf;
            }
            FastBee::setFixedJsonField(data, "hex", hex);
            FastBee::setFixedJsonField(data, "length", len);
        } else {
            FastBee::setFixedJsonField(data, "text", "");
            FastBee::setFixedJsonField(data, "hex", "");
            FastBee::setFixedJsonField(data, "length", 0);
        }
        if (config.type == PeripheralType::UART) {
            FastBee::setFixedJsonField(data, "framing", uartFramingName(config));
            addUartRuntime(data, uartRuntime, config.enabled);
        }
        if (doc.overflowed()) {
            ctx->sendError(request, 503, "COMM_RESPONSE_MEMORY_UNAVAILABLE: received bytes may have been consumed");
            return;
        }
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        bool detected = false;
        if (pm.readRadarState(id, detected)) {
            FastBee::setFixedJsonField(data, "detected", detected);
            FastBee::setFixedJsonField(data, "value", detected ? 1 : 0);
            FastBee::setFixedJsonField(data, "stateName", detected ? "DETECTED" : "CLEAR");
        } else {
            FastBee::setFixedJsonField(data, "detected", nullptr);
            FastBee::setFixedJsonField(data, "value", nullptr);
        }
    } else if (config.type == PeripheralType::RF_MODULE) {
        bool level = false;
        if (pm.readRfLevel(id, level)) {
            FastBee::setFixedJsonField(data, "level", level);
            FastBee::setFixedJsonField(data, "value", level ? 1 : 0);
            FastBee::setFixedJsonField(data, "stateName", level ? "ACTIVE" : "IDLE");
        } else {
            FastBee::setFixedJsonField(data, "level", nullptr);
            FastBee::setFixedJsonField(data, "value", nullptr);
        }
    } else if (config.type == PeripheralType::DAC) {
        FastBee::setFixedJsonField(data, "channel", config.params.dac.channel);
        FastBee::setFixedJsonField(data, "pin", config.getPrimaryPin());
    } else {
        uint8_t buffer[64];
        size_t len = sizeof(buffer);

        if (pm.readData(id, buffer, len) && len > 0) {
            FastBee::setFixedJsonField(data, "length", len);
            if (len <= 4) {
                uint32_t value = 0;
                for (size_t i = 0; i < len; i++) {
                    value |= ((uint32_t)buffer[i]) << (i * 8);
                }
                FastBee::setFixedJsonField(data, "value", value);
            }
        } else {
            FastBee::setFixedJsonField(data, "value", nullptr);
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}
