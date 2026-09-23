#include "screen/ScreenBackend.h"

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
#include "core/I2CBusManager.h"
#include "systems/LoggerSystem.h"
#include <Wire.h>
#endif

#include <cstring>
#include <new>

namespace FastBee {
namespace Screen {

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)

namespace {

constexpr uint8_t GT911_ADDRESS_PRIMARY = 0x5D;
constexpr uint8_t GT911_ADDRESS_SECONDARY = 0x14;
constexpr uint16_t GT911_REG_CONFIG_SIZE = 0x8048;
constexpr uint16_t GT911_REG_CONFIG_START = 0x8047;
constexpr uint16_t GT911_REG_TOUCH_NUMBER = 0x804C;
constexpr uint16_t GT911_REG_CONFIG_CHECKSUM = 0x80FF;
constexpr uint16_t GT911_REG_CONFIG_FRESH = 0x8100;
constexpr size_t GT911_CONFIG_DATA_LENGTH = 184;
constexpr size_t GT911_CONFIG_LENGTH = GT911_CONFIG_DATA_LENGTH + 2;
constexpr uint16_t GT911_REG_PRODUCT_ID = 0x8140;
constexpr uint16_t GT911_REG_STATUS = 0x814E;
constexpr uint16_t GT911_REG_FIRST_POINT = 0x814F;
constexpr uint8_t GT911_STATUS_READY = 0x80;
constexpr uint8_t GT911_STATUS_POINT_MASK = 0x0F;
constexpr uint8_t GT911_MAX_POINTS = 5;

class Gt911TouchBackend final : public ITouchBackend {
public:
    bool begin(const DisplayConfigParams& config, String& error) override {
        if (strcasecmp(config.touchBackend, "gt911") != 0) {
            error = "SCREEN_TOUCH_BACKEND_UNSUPPORTED";
            return false;
        }
        if (config.touchSda < 0 || config.touchScl < 0 ||
            config.touchSda == config.touchScl || config.width == 0 ||
            config.height == 0 || config.rotation > 3) {
            error = "SCREEN_TOUCH_CONFIG_INVALID";
            return false;
        }

        _config = config;
        if (!I2CBusManager::instance().acquire(
                this, config.touchSda, config.touchScl,
                config.i2cFrequency, error)) {
            if (error.isEmpty()) error = "SCREEN_TOUCH_I2C_ACQUIRE_FAILED";
            return false;
        }
        _leased = true;

        resetController(false);
        if (!probeAddress(GT911_ADDRESS_PRIMARY) &&
            !probeAddress(GT911_ADDRESS_SECONDARY)) {
            // Some boards strap INT high and require a reset sequence before
            // responding at the alternate address.
            resetController(true);
            if (!probeAddress(GT911_ADDRESS_SECONDARY) &&
                !probeAddress(GT911_ADDRESS_PRIMARY)) {
                error = "SCREEN_TOUCH_GT911_NOT_FOUND";
                end();
                return false;
            }
        }

        // Product ID, firmware version and the controller's reported X/Y
        // extent are consecutive read-only fields at 0x8140..0x8149.
        uint8_t productId[10] = {};
        uint8_t resolution[4] = {};
        if (!readRegister(GT911_REG_PRODUCT_ID, productId, sizeof(productId)) ||
            !readRegister(GT911_REG_CONFIG_SIZE, resolution, sizeof(resolution))) {
            error = "SCREEN_TOUCH_GT911_ID_READ_FAILED";
            end();
            return false;
        }

        _controllerWidth = static_cast<uint16_t>(resolution[0]) |
            (static_cast<uint16_t>(resolution[1]) << 8U);
        _controllerHeight = static_cast<uint16_t>(resolution[2]) |
            (static_cast<uint16_t>(resolution[3]) << 8U);
        LOGGER.infof("GT911: device product=%02X%02X%02X%02X fw=0x%02X%02X reported=%ux%u config=%ux%u",
                     productId[0], productId[1], productId[2], productId[3], productId[5], productId[4],
                     productId[6] | (static_cast<uint16_t>(productId[7]) << 8U),
                     productId[8] | (static_cast<uint16_t>(productId[9]) << 8U),
                     _controllerWidth, _controllerHeight);
        if (_controllerWidth == 0 || _controllerWidth == 0xFFFFU) {
            _controllerWidth = config.width;
        }
        if (_controllerHeight == 0 || _controllerHeight == 0xFFFFU) {
            _controllerHeight = config.height;
        }

        if (!configureSingleTouch(error)) {
            end();
            return false;
        }

        if (config.touchInt >= 0) {
            pinMode(static_cast<uint8_t>(config.touchInt), INPUT);
        }
        _ready = true;
        _lastPollMs = _lastDiagnosticMs = millis();
        LOGGER.infof("GT911: ready address=0x%02X product=%02X%02X%02X%02X resolution=%ux%u rotation=%u",
                  _address, productId[0], productId[1], productId[2], productId[3],
                  _controllerWidth, _controllerHeight, _config.rotation);
        error = "";
        return true;
    }

    void end() override {
        _ready = false;
        _wasPressed = false;
        if (_leased) {
            if (!I2CBusManager::instance().release(this, 100)) {
                I2CBusManager::instance().deferRelease(this);
            }
            _leased = false;
        }
        if (_config.touchInt >= 0) {
            pinMode(static_cast<uint8_t>(_config.touchInt), INPUT);
        }
        if (_config.touchRst >= 0) {
            pinMode(static_cast<uint8_t>(_config.touchRst), INPUT);
        }
        _address = 0;
    }

    bool ready() const override { return _ready; }

    bool poll(TouchPoint& point) override {
        if (!_ready || _address == 0) return false;
        const uint32_t now = millis();
        const uint32_t gap = now - _lastPollMs;
        if (gap > _maxPollGapMs) _maxPollGapMs = gap;
        _lastPollMs = now;
        ++_polls;
        if (now - _lastDiagnosticMs >= 30000U) {
            LOGGER.infof("GT911: polls=%u ready=%u down=%u up=%u readFail=%u ackFail=%u invalid=%u maxGap=%ums status=0x%02X",
                      _polls, _readyReports, _downReports, _upReports, _readFailures,
                      _ackFailures, _invalidReports, _maxPollGapMs, _lastStatus);
            _lastDiagnosticMs = now;
        }

        uint8_t status = 0;
        if (!readRegister(GT911_REG_STATUS, &status, 1)) { ++_readFailures; return false; }
        _lastStatus = status;
        if ((status & GT911_STATUS_READY) == 0) return false;
        ++_readyReports;

        const uint8_t pointCount = status & GT911_STATUS_POINT_MASK;
        if (pointCount == 0) {
            if (!clearStatus()) { ++_ackFailures; return false; }
            if (!_wasPressed) return false;
            point = _lastPoint;
            point.pressed = false;
            point.timestampMs = millis();
            _wasPressed = false;
            _lastPoint = point;
            ++_upReports;
            logPoint(point);
            return true;
        }
        if (pointCount > GT911_MAX_POINTS) {
            ++_invalidReports;
            if (!clearStatus()) ++_ackFailures;
            return false;
        }

        uint8_t data[8] = {};
        const bool readOk = readRegister(
            GT911_REG_FIRST_POINT, data, sizeof(data));
        // Do not acknowledge a packet we failed to read; retry the latched
        // coordinates next poll instead of dropping the user's press.
        if (!readOk) { ++_readFailures; return false; }
        if (!clearStatus()) { ++_ackFailures; return false; }

        const uint16_t rawX = static_cast<uint16_t>(data[1]) |
            (static_cast<uint16_t>(data[2]) << 8U);
        const uint16_t rawY = static_cast<uint16_t>(data[3]) |
            (static_cast<uint16_t>(data[4]) << 8U);
        uint16_t nativeX = scaleCoordinate(rawX, _controllerWidth, _config.width);
        uint16_t nativeY = scaleCoordinate(rawY, _controllerHeight, _config.height);
        transform(nativeX, nativeY, point.x, point.y);
        point.contactId = data[0];
        point.pressed = true;
        point.timestampMs = millis();
        _wasPressed = true;
        _lastPoint = point;
        ++_downReports;
        logPoint(point);
        return true;
    }

private:
    bool configureSingleTouch(String& error) {
        // Match SensorLib's setMaxTouchPoint/reloadConfig sequence, while
        // retaining the panel vendor's electrode/timing configuration. Read
        // everything before the first write; keep each transfer within Wire's
        // existing buffer rather than reallocating a shared I2C bus buffer.
        uint8_t config[GT911_CONFIG_LENGTH] = {};
        for (size_t offset = 0; offset < sizeof(config); offset += 32) {
            const size_t remaining = sizeof(config) - offset;
            const size_t count = remaining < 32 ? remaining : 32;
            if (!readRegister(GT911_REG_CONFIG_START + offset, config + offset, count)) {
                error = "SCREEN_TOUCH_GT911_CONFIG_READ_FAILED";
                return false;
            }
        }
        uint8_t sum = 0;
        for (size_t i = 0; i <= GT911_CONFIG_DATA_LENGTH; ++i) sum += config[i];
        LOGGER.infof("GT911: config version=0x%02X maxPoints=%u switch1=0x%02X refresh=%ums checksum=0x%02X fresh=0x%02X valid=%u command=write-only",
                     config[0], config[5] & 0x0F, config[6], 5U + (config[15] & 0x0F),
                     config[GT911_CONFIG_DATA_LENGTH], config[GT911_CONFIG_DATA_LENGTH + 1], sum == 0);
        if (sum != 0) {
            error = "SCREEN_TOUCH_GT911_CONFIG_CHECKSUM_INVALID";
            return false;
        }
        const uint16_t width = config[1] | (static_cast<uint16_t>(config[2]) << 8U);
        const uint16_t height = config[3] | (static_cast<uint16_t>(config[4]) << 8U);
        const uint8_t maxPoints = config[5] & 0x0F;
        // Zero-filled/uninitialized memory has a valid additive checksum too.
        // Never commit that as a vendor configuration merely by changing its
        // touch-count byte. The mapping/electrode data must come from a valid
        // controller configuration, not an invented generic fallback table.
        if (!width || width == 0xFFFFU || !height || height == 0xFFFFU ||
            !maxPoints || maxPoints > GT911_MAX_POINTS) {
            error = "SCREEN_TOUCH_GT911_CONFIG_INVALID";
            return false;
        }
        const uint8_t points = (config[5] & 0xF0U) | 1U;
        const uint8_t checksum = config[GT911_CONFIG_DATA_LENGTH] + config[5] - points;
        const uint8_t fresh = 1;
        // Even an already-single-touch config follows the vendor refresh
        // sequence on initialization; normal polling never rewrites config.
        if (!writeRegister(GT911_REG_TOUCH_NUMBER, &points, 1) ||
            !writeRegister(GT911_REG_CONFIG_CHECKSUM, &checksum, 1) ||
            !writeRegister(GT911_REG_CONFIG_FRESH, &fresh, 1)) {
            // A failed I2C acknowledgement may still mean the device received
            // bytes. Restore the original values and commit only if both
            // restoration writes succeeded; never continue with partial data.
            const bool restoredPoints = writeRegister(GT911_REG_TOUCH_NUMBER, config + 5, 1);
            const bool restoredChecksum = writeRegister(GT911_REG_CONFIG_CHECKSUM, config + GT911_CONFIG_DATA_LENGTH, 1);
            const bool restored = restoredPoints && restoredChecksum &&
                writeRegister(GT911_REG_CONFIG_FRESH, &fresh, 1);
            error = restored ? "SCREEN_TOUCH_GT911_CONFIG_WRITE_FAILED" :
                               "SCREEN_TOUCH_GT911_CONFIG_RESTORE_FAILED";
            return false;
        }
        delay(20);
        uint8_t actualPoints = 0, actualChecksum = 0;
        if (!readRegister(GT911_REG_TOUCH_NUMBER, &actualPoints, 1) ||
            !readRegister(GT911_REG_CONFIG_CHECKSUM, &actualChecksum, 1) ||
            actualPoints != points || actualChecksum != checksum) {
            error = "SCREEN_TOUCH_GT911_CONFIG_VERIFY_FAILED";
            return false;
        }
        LOGGER.infof("GT911: single-touch config applied maxPoints=%u checksum=0x%02X", actualPoints & 0x0F, actualChecksum);
        return true;
    }

    void logPoint(const TouchPoint& point) {
        if (_loggedPoints >= 8) return;
        ++_loggedPoints;
        LOGGER.infof("GT911: event=%s x=%u y=%u id=%u",
                  point.pressed ? "down" : "up", point.x, point.y, point.contactId);
    }
    static uint16_t scaleCoordinate(uint16_t value, uint16_t sourceExtent,
                                    uint16_t targetExtent) {
        if (targetExtent <= 1) return 0;
        if (sourceExtent <= 1) {
            return value < targetExtent ? value : targetExtent - 1;
        }
        if (value >= sourceExtent) value = sourceExtent - 1;
        return static_cast<uint16_t>(
            (static_cast<uint32_t>(value) * (targetExtent - 1U)) /
            (sourceExtent - 1U));
    }

    void transform(uint16_t nativeX, uint16_t nativeY,
                   uint16_t& logicalX, uint16_t& logicalY) const {
        const uint16_t maxX = _config.width - 1U;
        const uint16_t maxY = _config.height - 1U;
        switch (_config.rotation & 3U) {
            case 1:
                logicalX = nativeY;
                logicalY = maxX - nativeX;
                break;
            case 2:
                logicalX = maxX - nativeX;
                logicalY = maxY - nativeY;
                break;
            case 3:
                logicalX = maxY - nativeY;
                logicalY = nativeX;
                break;
            default:
                logicalX = nativeX;
                logicalY = nativeY;
                break;
        }
    }

    void resetController(bool selectAlternateAddress) {
        if (_config.touchRst < 0) return;
        const uint8_t resetPin = static_cast<uint8_t>(_config.touchRst);
        pinMode(resetPin, OUTPUT);
        digitalWrite(resetPin, LOW);
        if (_config.touchInt >= 0) {
            const uint8_t interruptPin = static_cast<uint8_t>(_config.touchInt);
            pinMode(interruptPin, OUTPUT);
            // GT911 samples INT during reset: LOW selects 0x5D, HIGH 0x14.
            digitalWrite(interruptPin, selectAlternateAddress ? HIGH : LOW);
        }
        delay(12);
        digitalWrite(resetPin, HIGH);
        delay(12);
        if (_config.touchInt >= 0) {
            pinMode(static_cast<uint8_t>(_config.touchInt), INPUT);
        }
        delay(50);
    }

    bool probeAddress(uint8_t address) {
        _address = address;
        uint8_t productId[4] = {};
        if (!readRegister(GT911_REG_PRODUCT_ID, productId, sizeof(productId))) {
            _address = 0;
            return false;
        }
        bool recognizable = false;
        for (uint8_t value : productId) {
            if ((value >= '0' && value <= '9') ||
                (value >= 'A' && value <= 'Z')) {
                recognizable = true;
                break;
            }
        }
        if (!recognizable) _address = 0;
        return recognizable;
    }

    bool clearStatus() {
        const uint8_t clear = 0;
        return writeRegister(GT911_REG_STATUS, &clear, 1);
    }

    bool readRegister(uint16_t reg, uint8_t* data, size_t length) {
        if (!_leased || _address == 0 || !data || length == 0 || length > 32) {
            return false;
        }
        I2CBusLock lock(50);
        TwoWire* wire = lock.locked()
            ? I2CBusManager::instance().wire(this) : nullptr;
        if (!wire) return false;
        wire->beginTransmission(_address);
        wire->write(static_cast<uint8_t>(reg >> 8U));
        wire->write(static_cast<uint8_t>(reg));
        if (wire->endTransmission(false) != 0) return false;
        const size_t received = wire->requestFrom(
            _address, static_cast<uint8_t>(length));
        if (received != length) {
            while (wire->available()) wire->read();
            return false;
        }
        for (size_t i = 0; i < length; ++i) {
            if (!wire->available()) return false;
            data[i] = static_cast<uint8_t>(wire->read());
        }
        return true;
    }

    bool writeRegister(uint16_t reg, const uint8_t* data, size_t length) {
        if (!_leased || _address == 0 || !data || length == 0 || length > 29) {
            return false;
        }
        I2CBusLock lock(50);
        TwoWire* wire = lock.locked()
            ? I2CBusManager::instance().wire(this) : nullptr;
        if (!wire) return false;
        wire->beginTransmission(_address);
        wire->write(static_cast<uint8_t>(reg >> 8U));
        wire->write(static_cast<uint8_t>(reg));
        wire->write(data, length);
        return wire->endTransmission() == 0;
    }

    DisplayConfigParams _config{};
    TouchPoint _lastPoint{};
    uint16_t _controllerWidth = 0;
    uint16_t _controllerHeight = 0;
    uint8_t _address = 0;
    uint8_t _lastStatus = 0, _loggedPoints = 0;
    uint32_t _lastPollMs = 0, _lastDiagnosticMs = 0, _maxPollGapMs = 0;
    uint32_t _polls = 0, _readyReports = 0, _downReports = 0, _upReports = 0;
    uint32_t _readFailures = 0, _ackFailures = 0, _invalidReports = 0;
    bool _leased = false;
    bool _ready = false;
    bool _wasPressed = false;
};

}  // namespace

#endif

std::unique_ptr<ITouchBackend> createTouchBackend(
        const DisplayConfigParams& config, String& error) {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    if (strcasecmp(config.touchBackend, "none") == 0) {
        error = "";
        return nullptr;
    }
    if (strcasecmp(config.touchBackend, "gt911") == 0) {
        std::unique_ptr<ITouchBackend> backend(
            new (std::nothrow) Gt911TouchBackend());
        if (!backend) error = "SCREEN_TOUCH_ALLOCATION_FAILED";
        return backend;
    }
    error = "SCREEN_TOUCH_BACKEND_UNSUPPORTED";
#else
    (void)config;
    error = "SCREEN_UI_NOT_AVAILABLE_ON_TARGET";
#endif
    return nullptr;
}

}  // namespace Screen
}  // namespace FastBee
