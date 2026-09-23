#ifndef FASTBEE_I2C_SENSOR_BUS_H
#define FASTBEE_I2C_SENSOR_BUS_H

#include "core/I2CBusManager.h"
#include "core/SensorDriverMetadata.h"
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>

namespace FastBee {

// A driver's bus lease. Conversion delays belong outside these short locked
// transactions, so an 80 ms sensor conversion never stalls digital inputs.
class I2CSensorBus {
public:
    ~I2CSensorBus() {
        if (!close()) I2CBusManager::instance().deferRelease(_owner);
    }
    I2CSensorBus() = default;
    I2CSensorBus(const I2CSensorBus&) = delete;
    I2CSensorBus& operator=(const I2CSensorBus&) = delete;

    bool open(const void* owner, const char* driver, const char* params,
              JsonDocument& doc) {
        // Admission also covers closing an existing lease. Taking the recursive
        // lock first prevents release() from adding its default one-second wait,
        // and a busy reopen leaves the current lease intact for a later retry.
        I2CBusLock admissionLock(100);
        if (!admissionLock.locked()) return fail("SENSOR_I2C_BUSY");
        if (!close()) return false;
        if (params && params[0] &&
            (deserializeJson(doc, params) != DeserializationError::Ok || !doc.is<JsonObject>())) {
            return fail("SENSOR_PARAMS_INVALID");
        }
        const auto* meta = sensorDriverMetadata(driver);
        if (!meta) return fail("SENSOR_DRIVER_UNKNOWN");
        long address = meta->defaultAddress;
        if (!doc["addr"].isNull()) {
            if (doc["addr"].is<const char*>()) {
                const char* input = doc["addr"].as<const char*>();
                char* end = nullptr;
                address = strtol(input, &end, 0);
                if (!input[0] || *end) return fail("SENSOR_ADDRESS_INVALID");
            } else if (doc["addr"].is<int>()) address = doc["addr"].as<int>();
            else return fail("SENSOR_ADDRESS_INVALID");
        }
        if (address < 0 || address > 127 || !meta->supportsAddress(static_cast<uint8_t>(address))) {
            return fail("SENSOR_ADDRESS_INVALID");
        }
        auto& manager = I2CBusManager::instance();
        const int activeSda = manager.sda();
        const int activeScl = manager.scl();
        const uint32_t activeFrequency = manager.frequency();
        const int defaultSda = activeSda >= 0 ? activeSda : SDA;
        const int defaultScl = activeScl >= 0 ? activeScl : SCL;
        const int sda = doc["sda"] | defaultSda;
        const int scl = doc["scl"] | defaultScl;
        const uint32_t frequency = doc["frequency"] |
            (activeFrequency ? activeFrequency : 100000UL);
        if (sda < 0 || sda > 48 || scl < 0 || scl > 48 || sda == scl) {
            return fail("SENSOR_I2C_PINS_INVALID");
        }
        if (frequency != 100000 && frequency != 400000) return fail("SENSOR_I2C_FREQUENCY_INVALID");
        String error;
        if (!manager.acquire(owner, sda, scl, frequency, error)) return fail(error.c_str());
        _owner = owner;
        _wire = manager.wire(owner);
        _address = static_cast<uint8_t>(address);
        if (!_wire) { close(); return fail("SENSOR_I2C_NOT_READY"); }
        _error[0] = '\0';
        return true;
    }

    bool close() {
        if (_owner && !I2CBusManager::instance().release(_owner, 100)) {
            return fail("SENSOR_I2C_BUSY");
        }
        _owner = nullptr;
        _wire = nullptr;
        return true;
    }
    TwoWire* wire() const { return _wire; }
    uint8_t address() const { return _address; }
    const char* error() const { return _error; }
    bool fail(const char* error) {
        snprintf(_error, sizeof(_error), "%s", error ? error : "SENSOR_I2C_ERROR");
        return false;
    }

    bool write(const uint8_t* data, size_t length) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail("SENSOR_I2C_BUSY");
        if (!_wire) return fail("SENSOR_I2C_NOT_READY");
        _wire->beginTransmission(_address);
        const size_t written = length ? _wire->write(data, length) : 0;
        const uint8_t status = _wire->endTransmission();
        if (status != 0 || written != length) return fail("SENSOR_I2C_WRITE_FAILED");
        _error[0] = '\0';
        return true;
    }

    bool read(uint8_t* data, size_t length) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail("SENSOR_I2C_BUSY");
        return readLocked(data, length);
    }

    bool readRegister(uint8_t reg, uint8_t* data, size_t length) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail("SENSOR_I2C_BUSY");
        if (!_wire) return fail("SENSOR_I2C_NOT_READY");
        _wire->beginTransmission(_address);
        _wire->write(reg);
        if (_wire->endTransmission(false) != 0) return fail("SENSOR_I2C_WRITE_FAILED");
        return readLocked(data, length);
    }

    bool readRegister16(uint8_t reg, uint16_t& value) {
        uint8_t bytes[2];
        if (!readRegister(reg, bytes, sizeof(bytes))) return false;
        value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
        return true;
    }

    bool writeRegister16(uint8_t reg, uint16_t value) {
        const uint8_t bytes[] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        return write(bytes, sizeof(bytes));
    }

private:
    bool readLocked(uint8_t* data, size_t length) {
        if (!_wire) return fail("SENSOR_I2C_NOT_READY");
        if (_wire->requestFrom(_address, static_cast<uint8_t>(length)) != length) {
            while (_wire->available()) _wire->read();
            return fail("SENSOR_I2C_SHORT_READ");
        }
        for (size_t i = 0; i < length; ++i) data[i] = static_cast<uint8_t>(_wire->read());
        _error[0] = '\0';
        return true;
    }

    const void* _owner = nullptr;
    TwoWire* _wire = nullptr;
    uint8_t _address = 0;
    char _error[96] = {};
};

} // namespace FastBee
#endif
