#include "peripherals/IoExpanderProvider.h"
#include "core/I2CBusManager.h"

namespace FastBee {

IoExpanderProvider::~IoExpanderProvider() {
    driver.end();
    if (_leased) {
        auto& bus = I2CBusManager::instance();
        if (!bus.release(this, 100)) bus.deferRelease(this);
    }
}

bool IoExpanderProvider::acquire(uint8_t sda, uint8_t scl, String& error) {
    if (_closing) { error = "IO_SHUTDOWN_PENDING"; return false; }
    const bool acquired = I2CBusManager::instance().acquire(this, sda, scl, 100000, error);
    if (acquired) _leased = true;
    return acquired;
}

IoExpanderResult IoExpanderProvider::shutdown(uint8_t safeByte) {
    if (!_closing) {
        _closing = true;
        _shutdownByte = safeByte;
        driver.end(); // No more normal reads/writes while waiting for admission.
    }
    if (!_leased) return _shutdownResult;
    I2CBusLock guard(0);
    if (!guard.locked()) return IoExpanderResult::Busy;
    _shutdownResult = driver.shutdown(_shutdownByte);
    // Both calls nest into this already-held recursive lock, so neither can add
    // the manager's default one-second admission wait.
    I2CBusManager::instance().release(this);
    _leased = false;
    return _shutdownResult;
}

IoExpanderResult IoExpanderProvider::writeByte(uint8_t value) {
    I2CBusLock guard(0);
    if (!guard.locked()) return IoExpanderResult::Busy;
    TwoWire* wire = I2CBusManager::instance().wire(this);
    if (!wire) return IoExpanderResult::Error;
    wire->beginTransmission(_address);
    const size_t written = wire->write(value);
    return wire->endTransmission() == 0 && written == 1
        ? IoExpanderResult::Ok : IoExpanderResult::Error;
}

IoExpanderResult IoExpanderProvider::readByte(uint8_t& value) {
    I2CBusLock guard(0);
    if (!guard.locked()) return IoExpanderResult::Busy;
    TwoWire* wire = I2CBusManager::instance().wire(this);
    if (!wire || wire->requestFrom(_address, static_cast<uint8_t>(1)) != 1 ||
        wire->available() != 1) return IoExpanderResult::Error;
    value = static_cast<uint8_t>(wire->read());
    return IoExpanderResult::Ok;
}

} // namespace FastBee
