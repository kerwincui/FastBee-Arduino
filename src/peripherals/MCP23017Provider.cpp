#include "peripherals/MCP23017Provider.h"

namespace FastBee {

bool MCP23017Provider::fault(Error error) {
    if (!_faulted) {
        if (_failures < 255) ++_failures;
        ++_generation;
    }
    _faulted = true;
    _sessionError = _device.session().error();
    return reject(error);
}

bool MCP23017Provider::resetValid() const {
    return _reset && _reset->owned() && _reset->pin() >= 0 &&
            _reset->pin() <= 48 && _reset->pin() != _sda && _reset->pin() != _scl &&
            (_resetPin < 0 || _reset->pin() == _resetPin);
}

bool MCP23017Provider::checkReset() {
    return resetValid() || fault(Error::ResetOwnership);
}

bool MCP23017Provider::begin(uint16_t outputs, uint16_t startupHigh, uint16_t intervalMs,
                            uint32_t now, uint16_t inputs) {
    if (_closing) return reject(Error::Closing);
    if (_attempted || _faulted) return reject(Error::AlreadyAttempted);
    MCP23017PortConfig checked;
    // Startup is a complete port word in the shared manager contract. Only
    // declared outputs may drive; unclaimed/input bits never become outputs.
    if (_sda < 0 || _sda > 48 || _scl < 0 || _scl > 48 || _sda == _scl ||
        (_frequency != 100000 && _frequency != 400000) || _address < 0x20 || _address > 0x27 ||
        intervalMs < 20 || intervalMs > 1000 ||
        !makeMCP23017PortConfig(outputs, inputs, startupHigh & outputs, 0, 0, checked))
        return reject(Error::Arguments);
    if (!checkReset()) return false;
    _resetPin = _reset->pin();
    I2CBusLock lock(0);
    if (!lock.locked()) return reject(Error::Busy);
    // No automatic reset or startup replay after ANY actual begin attempt.
    // A new explicit provider lifecycle is required after failure.
    _attempted = true;
    _outputs = outputs; _inputs = inputs; _interval = intervalMs;
    if (!_device.beginWithReset(_sda, _scl, _frequency, _address, *_reset,
                               outputs, inputs, startupHigh & outputs, 0, 0))
        return fault(Error::Begin);
    return sample(now);
}

bool MCP23017Provider::configure(uint16_t outputs, uint16_t startupHigh,
                                uint32_t now, uint16_t inputs) {
    if (_closing) return reject(Error::Closing);
    if (!initialized()) return reject(Error::NotReady);
    MCP23017PortConfig checked;
    if (!makeMCP23017PortConfig(outputs, inputs, startupHigh & outputs, 0, 0, checked))
        return reject(Error::Arguments);
    if (!checkReset()) return false;
    I2CBusLock lock(0);
    if (!lock.locked()) return reject(Error::Busy);
    if (!_device.configure(outputs, inputs, startupHigh & outputs, 0, 0))
        return fault(Error::Session);
    _outputs = outputs; _inputs = inputs;
    // The real session invalidates inputs after a logical/hardware change.
    // Let the next maintenance pass sample immediately, without replaying outputs.
    if (!_device.session().sample().valid) _lastAttempt = now - _interval;
    _error = Error::None;
    return true;
}

bool MCP23017Provider::sample(uint32_t now) {
    const bool continuous = healthy(now);
    _lastAttempt = now;
    if (!_device.sampleInputs(now)) return fault(Error::Session);
    if (!continuous) ++_generation;
    _error = Error::None;
    return true;
}

bool MCP23017Provider::poll(uint32_t now) {
    if (_closing || !initialized()) return false;
    if (!checkReset()) return false;
    if (static_cast<uint32_t>(now - _lastAttempt) < _interval) return healthy(now);
    I2CBusLock lock(0);
    if (!lock.locked()) return reject(Error::Busy);
    return sample(now);
}

bool MCP23017Provider::healthy(uint32_t now) const {
    const auto& sample = _device.session().sample();
    // Cache-only readers must not expose a pre-loss sample as current while
    // waiting for the next poll to record the terminal ownership fault.
    return initialized() && resetValid() && sample.valid &&
        static_cast<uint32_t>(now - sample.capturedMs) <= 3UL * _interval;
}

bool MCP23017Provider::readChannel(uint8_t channel, bool output, uint32_t now, bool& high) const {
    if (!supportsChannel(channel) || !healthy(now)) return false;
    const uint16_t bit = uint16_t(1) << channel;
    if (((output ? _outputs : _inputs) & bit) == 0) return false;
    uint16_t value = _device.session().sample().inputs;
    if (output && !_device.session().knownLatch(value)) return false;
    high = (value & bit) != 0;
    return true;
}

bool MCP23017Provider::writeChannel(uint8_t channel, bool high, uint32_t now) {
    if (_closing) return reject(Error::Closing);
    if (!supportsChannel(channel) || !(_outputs & (uint16_t(1) << channel)))
        return reject(Error::Arguments);
    if (!checkReset()) return false;
    if (!healthy(now)) return reject(Error::NotReady);
    I2CBusLock lock(0);
    if (!lock.locked()) return reject(Error::Busy);
    const uint16_t selected = uint16_t(1) << channel;
    if (!_device.setOutputs(selected, high ? selected : 0)) return fault(Error::Session);
    _lastAttempt = now - _interval;
    _error = Error::None;
    return true;
}

IoExpanderResult MCP23017Provider::shutdown(uint16_t safeHigh) {
    if (_terminal) return _stopResult;
    if (!_closing) { _closing = true; _stopHigh = safeHigh & _outputs; ++_generation; }
    if (!_deviceEnded) {
        I2CBusLock lock(0);
        if (!lock.locked()) { reject(Error::Busy); return IoExpanderResult::Busy; }
        if (!_stopApplied) {
            _stopApplied = true;
            // Unknown BANK/config/latch is never repaired during shutdown.
            // One verified write attempt only; partial failure is not replayed.
            const bool known = !_faulted && _device.session().ready() && checkReset();
            if (known && _device.setOutputs(_outputs, _stopHigh)) _stopResult = IoExpanderResult::Ok;
            else fault(Error::StopUnconfirmed);
        }
        if (!_device.end()) { reject(Error::Busy); return IoExpanderResult::Busy; }
        _deviceEnded = true;
    }
    // End bus access before returning RESET to ordinary native GPIO ownership.
    // Retry only release, never the previous latch write or the RESET pulse.
    if (_reset && !_reset->release()) {
        reject(Error::ControlRelease);
        return IoExpanderResult::Busy;
    }
    _terminal = true;
    _error = _stopResult == IoExpanderResult::Ok ? Error::None : Error::StopUnconfirmed;
    return _stopResult;
}

} // namespace FastBee
