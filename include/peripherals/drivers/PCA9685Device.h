#ifndef FASTBEE_PCA9685_DEVICE_H
#define FASTBEE_PCA9685_DEVICE_H

#include "core/I2CBusManager.h"
#include "peripherals/drivers/PCA9685OutputSession.h"

namespace FastBee {

// An integrator must reserve/wire OE before supplying this gate. Its return
// values report the local GPIO operation, not an observed physical load state.
// Keep the gate alive until after the device is destroyed.
class PCA9685OutputGate {
public:
    virtual ~PCA9685OutputGate() = default;
    virtual int pin() const = 0;
    virtual bool owned() const = 0;
    virtual bool disable() = 0;
    virtual bool enable() = 0;
    virtual bool disabled() const = 0;
};

// Owning production adapters retain this lease until device I/O has ended.
class PCA9685OutputLease : public PCA9685OutputGate {
public:
    virtual bool release() = 0;
};

// Production transport used by PCA9685Provider; consolidated verification pending.
// Caller serializes methods/getters, validates board resources and supplies
// actual fresh power-on evidence. No broadcast reset or implicit OE enable.
class PCA9685Device {
public:
    enum class Error { None, Arguments, AlreadyOpen, Busy, Admission, Session, OutputGate };
    explicit PCA9685Device(PCA9685OutputGate& gate) : _gate(gate) {}
    PCA9685Device(const PCA9685Device&) = delete;
    PCA9685Device& operator=(const PCA9685Device&) = delete;
    ~PCA9685Device() {
        if (_gateStarted && controlValid()) _gate.disable();
        auto& manager = I2CBusManager::instance();
        if (_leased && !manager.release(this, 100)) manager.deferRelease(this);
        if (_aliasLeased && !manager.release(&_aliasOwner, 100)) manager.deferRelease(&_aliasOwner);
    }

    bool begin(int sda, int scl, uint32_t frequency, uint32_t address,
               PCA9685OutputSession::ResetEvidence reset, double pwmHz, double clockHz,
               uint32_t channelMask, bool openDrain) {
        if (_leased || _aliasLeased) return fail(Error::AlreadyOpen);
        PCA9685FrequencyConfig config;
        if (sda < 0 || sda > 48 || scl < 0 || scl > 48 || sda == scl ||
            (frequency != 100000 && frequency != 400000) ||
            address < 0x40 || address > 0x77 || address == 0x70 ||
            reset != PCA9685OutputSession::ResetEvidence::ConfirmedPowerOn ||
            !channelMask || channelMask > 0xffff || !makePCA9685FrequencyConfig(pwmHz, clockHz, config))
            return fail(Error::Arguments);
        if (_gate.pin() < 0 || _gate.pin() > 48 || _gate.pin() == sda || _gate.pin() == scl || !_gate.owned())
            return fail(Error::OutputGate);
        _gatePin = _gate.pin();
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        auto& manager = I2CBusManager::instance(); String detail;
        _address = static_cast<uint8_t>(address);
        _leased = manager.acquire(this, static_cast<int8_t>(sda), static_cast<int8_t>(scl), frequency, detail,
                                  ResourceConsumerRole::Peripheral);
        if (!_leased || !manager.claimAddress(this, _address, detail)) {
            releaseLocked(); return fail(Error::Admission);
        }
        _aliasLeased = manager.acquire(&_aliasOwner, static_cast<int8_t>(sda), static_cast<int8_t>(scl), frequency, detail,
                                       ResourceConsumerRole::Peripheral);
        if (!_aliasLeased || !manager.claimAddress(&_aliasOwner, 0x70, detail)) {
            releaseLocked(); return fail(Error::Admission);
        }
        // The power-on all-call address stays reserved even after disabling it:
        // a spontaneous chip reset can restore its response. Initial policy is
        // one PCA9685 per shared bus, rather than silently aliasing another owner.
        _wire = manager.wire(this); _gateStarted = true;
        Adapter bus(*this);
        if (!_session.begin(bus, reset, pwmHz, clockHz, channelMask, openDrain)) {
            releaseLocked(); return fail(Error::Session);
        }
        _error = Error::None; return true;
    }
    bool service() {
        I2CBusLock lock(0);
        if (!lock.locked()) return fail(Error::Busy);
        Adapter bus(*this);
        if (!_session.service(bus)) return fail(Error::Session);
        _error = Error::None; return true;
    }
    bool setChannel(uint32_t channel, uint32_t phase, uint32_t widthTicks) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        Adapter bus(*this);
        if (!_session.setChannel(bus, channel, phase, widthTicks)) return fail(Error::Session);
        _error = Error::None; return true;
    }
    bool enableOutputs() {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        Adapter bus(*this);
        if (!_session.confirmForEnable(bus)) return fail(Error::Session);
        if (!controlValid() || !_gate.enable() || !controlValid() || _gate.disabled()) {
            _session.stop(bus); return fail(Error::OutputGate);
        }
        _error = Error::None; return true;
    }
    bool end() {
        // Gating does not depend on I2C admission. Busy keeps the address lease,
        // but the external OE should already have been commanded disabled.
        Adapter bus(*this);
        if (_gateStarted && !_session.stop(bus)) return fail(Error::OutputGate);
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        releaseLocked(); _gateStarted = false; _error = Error::None; return true;
    }
    Error error() const { return _error; }
    const PCA9685OutputSession& session() const { return _session; }
    bool outputsDisabled() const { return controlValid() && _gate.disabled(); }

private:
    struct Adapter {
        PCA9685Device& device;
        explicit Adapter(PCA9685Device& d) : device(d) {}
        bool exclusive() const {
            auto& manager = I2CBusManager::instance();
            return device._wire && device._leased && device._aliasLeased && device.controlValid() &&
                manager.ownsAddress(&device, device._address) && manager.ownsAddress(&device._aliasOwner, 0x70);
        }
        bool disableOutputs() { return device.controlValid() && device._gate.disable(); }
        bool outputsDisabled() const { return device.outputsDisabled(); }
        uint32_t nowMicros() const { return static_cast<uint32_t>(micros()); }
        bool read(uint8_t reg, uint8_t* data, size_t size) {
            if (!data || !size || size > 4) return false;
            auto* wire = device._wire;
            wire->beginTransmission(device._address);
            const size_t written = wire->write(reg);
            const uint8_t status = wire->endTransmission(written != 1);
            if (written != 1 || status != 0) return false;
            if (wire->requestFrom(device._address, static_cast<uint8_t>(size)) != size) {
                discard(); return false;
            }
            uint8_t staged[4] = {};
            for (size_t i = 0; i < size; ++i) {
                const int value = wire->available() > 0 ? wire->read() : -1;
                if (value < 0 || value > 255) { discard(); return false; }
                staged[i] = static_cast<uint8_t>(value);
            }
            if (wire->available() > 0) { discard(); return false; }
            for (size_t i = 0; i < size; ++i) data[i] = staged[i];
            return true;
        }
        bool write(uint8_t reg, const uint8_t* data, size_t size) {
            if (!data || !size || size > 4) return false;
            uint8_t bytes[5] = {reg};
            for (size_t i = 0; i < size; ++i) bytes[i + 1] = data[i];
            auto* wire = device._wire;
            wire->beginTransmission(device._address);
            const size_t written = wire->write(bytes, size + 1);
            const uint8_t status = wire->endTransmission();
            return written == size + 1 && status == 0;
        }
        void discard() {
            for (unsigned n = 0; n < 255 && device._wire->available() > 0; ++n)
                if (device._wire->read() < 0) break;
        }
    };
    void releaseLocked() {
        auto& manager = I2CBusManager::instance();
        if (_leased) manager.release(this, 100);
        if (_aliasLeased) manager.release(&_aliasOwner, 100);
        _leased = _aliasLeased = false; _wire = nullptr;
    }
    bool fail(Error error) { _error = error; return false; }
    bool controlValid() const {
        return _gatePin >= 0 && _gate.pin() == _gatePin && _gate.owned();
    }
    PCA9685OutputGate& _gate;
    PCA9685OutputSession _session;
    TwoWire* _wire = nullptr;
    uint8_t _address = 0, _aliasOwner = 0;
    bool _leased = false, _aliasLeased = false, _gateStarted = false;
    int _gatePin = -1;
    Error _error = Error::None;
};
} // namespace FastBee
#endif
