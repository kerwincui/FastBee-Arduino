#pragma once

#include <memory>
#include <new>
#include <utility>
#include "peripherals/drivers/PCA9685Device.h"

namespace FastBee {

// Owning adapter for the production PWM manager. Not a DigitalIoProvider:
// these channels share one frequency and cannot be offered as digital inputs.
// Caller holds its state lock for every method/getter and lifetime operation.
// Fresh power-on evidence is runtime-only; no persisted flag can provide it.
class PCA9685Provider final {
public:
    enum class State : uint8_t { Idle, Waking, ReadyDisabled, Enabled, Faulted, Closing, Closed };
    enum class Error : uint8_t { None, Arguments, AlreadyAttempted, Busy, Allocation,
        GateOwnership, Begin, NotReady, Session, Gate, Closing, ControlRelease };
    enum class StopResult : uint8_t { Ok, Pending, Error };

    PCA9685Provider(int sda, int scl, uint32_t frequency, uint32_t address,
                    std::unique_ptr<PCA9685OutputLease> gate)
        : _gate(std::move(gate)), _sda(sda), _scl(scl), _frequency(frequency), _address(address) {}
    PCA9685Provider(const PCA9685Provider&) = delete;
    PCA9685Provider& operator=(const PCA9685Provider&) = delete;
    // Device dies before its OE lease: best-effort gate-off / bus retirement
    // is not a confirmed shutdown and never implicitly enables outputs.
    ~PCA9685Provider() = default;

    bool begin(PCA9685OutputSession::ResetEvidence evidence, double pwmHz,
               double clockHz, uint32_t channels, bool openDrain) {
        if (_state == State::Closing || _state == State::Closed) return reject(Error::Closing);
        if (_attempted) return reject(Error::AlreadyAttempted);
        PCA9685FrequencyConfig calculated;
        if (!_gate || evidence != PCA9685OutputSession::ResetEvidence::ConfirmedPowerOn ||
            _sda < 0 || _sda > 48 || _scl < 0 || _scl > 48 || _sda == _scl ||
            (_frequency != 100000 && _frequency != 400000) || _address < 0x40 || _address > 0x77 ||
            _address == 0x70 || !channels || channels > 0xffff ||
            !makePCA9685FrequencyConfig(pwmHz, clockHz, calculated)) return reject(Error::Arguments);
        const int pin = _gate->pin();
        if (!_gate->owned() || pin < 0 || pin > 48 || pin == _sda || pin == _scl)
            return reject(Error::GateOwnership);
        I2CBusLock lock(0);
        if (!lock.locked()) return reject(Error::Busy);
        _device.reset(new (std::nothrow) PCA9685Device(*_gate));
        if (!_device) return reject(Error::Allocation);
        _gatePin = pin;
        _attempted = true;
        _channels = static_cast<uint16_t>(channels);
        if (!_device->begin(_sda, _scl, _frequency, _address, evidence, pwmHz, clockHz, channels, openDrain))
            return fault(Error::Begin);
        _state = State::Waking; _error = Error::None;
        return true;
    }

    bool service() {
        if (_state != State::Waking && _state != State::ReadyDisabled && _state != State::Enabled)
            return reject(Error::NotReady);
        if (!checkGate()) return false;
        if (_state != State::Waking) return true; // Cached state, no health polling claim.
        I2CBusLock lock(0);
        if (!lock.locked()) return reject(Error::Busy);
        if (!_device->service()) return fault(Error::Session);
        if (_device->session().ready()) _state = State::ReadyDisabled;
        _error = Error::None; return true;
    }

    bool setTicks(uint32_t channel, uint32_t phase, uint32_t width) {
        if (!ready()) return reject(Error::NotReady);
        PCA9685ChannelRegisters value;
        if (!makePCA9685ChannelRegisters(channel, phase, width, value) ||
            !(_channels & (uint16_t(1) << channel))) return reject(Error::Arguments);
        if (!checkGate()) return false;
        I2CBusLock lock(0);
        if (!lock.locked()) return reject(Error::Busy);
        if (!_device->setChannel(channel, phase, width)) return fault(Error::Session);
        _error = Error::None; return true;
    }

    bool setDutyPercent(uint32_t channel, double percent) {
        uint16_t ticks;
        if (!makePCA9685DutyTicks(percent, ticks)) return reject(Error::Arguments);
        return setTicks(channel, 0, ticks);
    }
    bool setPulseMicros(uint32_t channel, double pulseUs) {
        uint16_t ticks;
        if (!makePCA9685PulseTicks(pulseUs, calculatedFrequencyHz(), ticks)) return reject(Error::Arguments);
        return setTicks(channel, 0, ticks);
    }
    bool setServoAngle(uint32_t channel, double angle, double minAngle, double maxAngle,
                        double minPulseUs, double maxPulseUs) {
        uint16_t ticks;
        if (!makePCA9685ServoTicks(angle, minAngle, maxAngle, minPulseUs, maxPulseUs,
                                  calculatedFrequencyHz(), ticks)) return reject(Error::Arguments);
        return setTicks(channel, 0, ticks);
    }

    bool enableOutputs() {
        if (!ready()) return reject(Error::NotReady);
        if (!checkGate()) return false;
        if (_state == State::Enabled) return true; // Do not gate-off on duplicate enable.
        I2CBusLock lock(0);
        if (!lock.locked()) return reject(Error::Busy);
        if (!_device->enableOutputs()) return fault(Error::Gate);
        _state = State::Enabled; _error = Error::None; return true;
    }

    bool disableOutputs() {
        if (!ready()) return reject(Error::NotReady);
        if (!gateValid()) return fault(Error::GateOwnership);
        if (!_gate->disable() || !_gate->disabled()) return fault(Error::Gate);
        _state = State::ReadyDisabled; _error = Error::None; return true;
    }

    StopResult shutdown() {
        if (_state == State::Closed) return _stopFailed ? StopResult::Error : StopResult::Ok;
        _state = State::Closing;
        if (!_deviceEnded) {
            // OE gating is independent of I2C bus admission. A busy shared bus
            // cannot prevent this explicit inactive-HIGH request.
            if (_device && (!gateValid() || !_gate->disable() || !_gate->disabled())) {
                _stopFailed = true; recordFailure(Error::Gate); return StopResult::Pending;
            }
            I2CBusLock lock(0);
            if (!lock.locked()) { reject(Error::Busy); return StopResult::Pending; }
            if (_device && !_device->end()) {
                _stopFailed = true; recordFailure(Error::Gate); return StopResult::Pending;
            }
            _deviceEnded = true;
        }
        // Both primary and 0x70 aliases must retire before releasing OE. Once
        // here, retries are lease release only, never PWM writes or gate enable.
        if (_gate && !_gate->release()) { reject(Error::ControlRelease); return StopResult::Pending; }
        _state = State::Closed;
        if (!_stopFailed) _error = Error::None;
        return _stopFailed ? StopResult::Error : StopResult::Ok;
    }

    State state() const { return _state; }
    Error error() const { return _error; }
    Error firstFailure() const { return _firstFailure; }
    PCA9685OutputSession::Error sessionFailure() const { return _sessionFailure; }
    uint32_t failures() const { return _failures; }
    uint16_t channels() const { return _channels; }
    bool ready() const { return _state == State::ReadyDisabled || _state == State::Enabled; }
    double calculatedFrequencyHz() const {
        return _device ? _device->session().calculatedFrequencyHz() : 0;
    }
    // Last verified register shadow, not a sampled voltage or pulse measurement.
    bool knownChannel(uint32_t channel, PCA9685ChannelRegisters& value) const {
        return ready() && gateValid() && channel < 16 && (_channels & (uint16_t(1) << channel)) &&
            _device->session().knownChannel(channel, value);
    }

private:
    bool gateValid() const { return _gate && _gate->owned() && _gatePin >= 0 && _gate->pin() == _gatePin; }
    bool checkGate() {
        if (!gateValid()) return fault(Error::GateOwnership);
        if (_gate->disabled() != (_state != State::Enabled)) return fault(Error::Gate);
        return true;
    }
    bool reject(Error error) { _error = error; return false; }
    void recordFailure(Error error) {
        _error = error;
        if (_firstFailure == Error::None) _firstFailure = error;
        if (_failures < UINT32_MAX) ++_failures;
        if (_device && _sessionFailure == PCA9685OutputSession::Error::None)
            _sessionFailure = _device->session().error();
    }
    bool fault(Error error) {
        _state = State::Faulted; _stopFailed = true; recordFailure(error);
        // Never command a replaced/unowned control pin, even to disable it.
        if (gateValid()) _gate->disable();
        return false;
    }
    // Reverse destruction order is intentional: device before OE lease.
    std::unique_ptr<PCA9685OutputLease> _gate;
    std::unique_ptr<PCA9685Device> _device;
    int _sda, _scl, _gatePin = -1;
    uint32_t _frequency, _address;
    uint16_t _channels = 0;
    uint32_t _failures = 0;
    bool _attempted = false, _deviceEnded = false, _stopFailed = false;
    State _state = State::Idle;
    Error _error = Error::None, _firstFailure = Error::None;
    PCA9685OutputSession::Error _sessionFailure = PCA9685OutputSession::Error::None;
};
} // namespace FastBee
