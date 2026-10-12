#ifndef FASTBEE_MCP23017_DEVICE_H
#define FASTBEE_MCP23017_DEVICE_H

#include "core/I2CBusManager.h"
#include "peripherals/drivers/MCP23017PortSession.h"

namespace FastBee {

// A runtime control capability, never a persisted "reset confirmed" flag.
// The provider must own/validate a dedicated RESET pin before constructing it.
// pulse() must drive/read low for >=1us (DS20001952D table 1-2), then release
// high and confirm locally. Its implementation must be bounded and must not
// reenter this device or mutate its bus lease. Local GPIO feedback does not
// independently prove chip wiring or load safety.
class MCP23017ResetControl {
public:
    virtual ~MCP23017ResetControl() = default;
    virtual int pin() const = 0;
    virtual bool owned() const = 0;
    virtual bool pulse() = 0;
};

// A provider takes exclusive ownership of this object, retaining its native
// control lease until device I/O has stopped and the bus lease is released.
// Failed release retains the claim; destruction must arrange deferred cleanup.
class MCP23017ResetLease : public MCP23017ResetControl {
public:
    virtual bool release() = 0;
};

// Physical transport adapter, not yet a registered peripheral/provider.
// The owner must serialize calls/getters and validate board resources first.
// beginWithReset reserves the bus/address BEFORE invoking a separately owned
// reset controller. Legacy begin requires independently fresh power-on evidence.
// Neither path guesses a chip's bank after a software restart.
class MCP23017Device {
public:
    enum class Error { None, Arguments, ResetRequired, AlreadyOpen, Busy, Admission, Session, ResetFailed };
    MCP23017Device() = default;
    MCP23017Device(const MCP23017Device&) = delete;
    MCP23017Device& operator=(const MCP23017Device&) = delete;
    ~MCP23017Device() {
        if (!end()) I2CBusManager::instance().deferRelease(this);
    }

    bool begin(int sda, int scl, uint32_t frequency, uint32_t address,
               MCP23017PortSession::ResetEvidence reset, uint32_t outputs,
               uint32_t inputs, uint32_t startupHigh, uint32_t pullups, uint32_t inversion) {
        return beginImpl(sda, scl, frequency, address, reset, outputs, inputs,
                         startupHigh, pullups, inversion, nullptr);
    }
    bool beginWithReset(int sda, int scl, uint32_t frequency, uint32_t address,
                        MCP23017ResetControl& reset, uint32_t outputs,
                        uint32_t inputs, uint32_t startupHigh, uint32_t pullups, uint32_t inversion) {
        return beginImpl(sda, scl, frequency, address, MCP23017PortSession::ResetEvidence::Unknown,
                         outputs, inputs, startupHigh, pullups, inversion, &reset);
    }

private:
    bool beginImpl(int sda, int scl, uint32_t frequency, uint32_t address,
                   MCP23017PortSession::ResetEvidence reset, uint32_t outputs,
                   uint32_t inputs, uint32_t startupHigh, uint32_t pullups, uint32_t inversion,
                   MCP23017ResetControl* control) {
        if (_leased) return fail(Error::AlreadyOpen);
        MCP23017PortConfig validated;
        if (sda < 0 || sda > 48 || scl < 0 || scl > 48 || sda == scl ||
            (frequency != 100000 && frequency != 400000) || address < 0x20 || address > 0x27 ||
            !makeMCP23017PortConfig(outputs, inputs, startupHigh, pullups, inversion, validated))
            return fail(Error::Arguments);
        if (control && (control->pin() < 0 || control->pin() > 48 ||
                        control->pin() == sda || control->pin() == scl || !control->owned()))
            return fail(Error::ResetRequired);
        if (!control && reset != MCP23017PortSession::ResetEvidence::ConfirmedBank0Reset)
            return fail(Error::ResetRequired);
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        auto& manager = I2CBusManager::instance();
        String error;
        if (!manager.acquire(this, static_cast<int8_t>(sda), static_cast<int8_t>(scl),
                             frequency, error, ResourceConsumerRole::Peripheral)) return fail(Error::Admission);
        _leased = true;
        _address = static_cast<uint8_t>(address);
        if (!manager.claimAddress(this, _address, error)) {
            end(); return fail(Error::Admission);
        }
        _wire = manager.wire(this);
        if (control) {
            // Never reset on an address conflict, a full client table, invalid
            // configuration or bus contention. Ownership is checked again at
            // use, and failed reset causes NO register traffic or retry.
            if (!_wire || !control->owned() || !control->pulse() || !control->owned() ||
                !manager.ownsAddress(this, _address)) {
                manager.release(this, 100); _leased = false; _wire = nullptr;
                return fail(Error::ResetFailed);
            }
            reset = MCP23017PortSession::ResetEvidence::ConfirmedBank0Reset;
        }
        Adapter bus(*this);
        if (!_session.begin(bus, reset, outputs, inputs, startupHigh, pullups, inversion)) {
            // Retain the session's failure diagnostics. Releasing a lease does
            // not reverse a partially applied register write or shut off loads.
            manager.release(this, 100); _leased = false; _wire = nullptr;
            return fail(Error::Session);
        }
        _error = Error::None;
        return true;
    }

public:
    // No reset/replay: only an already verified session can change ownership.
    // A busy lock preserves the old session; a partial transfer invalidates it.
    bool configure(uint32_t outputs, uint32_t inputs, uint32_t startupHigh,
                   uint32_t pullups, uint32_t inversion) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        Adapter bus(*this);
        if (!_session.configure(bus, outputs, inputs, startupHigh, pullups, inversion))
            return fail(Error::Session);
        _error = Error::None; return true;
    }

    bool setOutputs(uint32_t selected, uint32_t high) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        Adapter bus(*this);
        if (!_session.setOutputs(bus, selected, high)) return fail(Error::Session);
        _error = Error::None; return true;
    }
    bool sampleInputs(uint32_t nowMs) {
        I2CBusLock lock(100);
        if (!lock.locked()) return fail(Error::Busy);
        Adapter bus(*this);
        if (!_session.sampleInputs(bus, nowMs)) return fail(Error::Session);
        _error = Error::None; return true;
    }
    // Stop software access only; retained chip outputs are NOT declared safe.
    // A busy stop retains the lease so the caller can retry.
    bool end() {
        if (_leased && !I2CBusManager::instance().release(this, 100)) return fail(Error::Busy);
        _leased = false; _wire = nullptr; _session.forget(); _error = Error::None;
        return true;
    }
    Error error() const { return _error; }
    const MCP23017PortSession& session() const { return _session; }

private:
    struct Adapter {
        MCP23017Device& device;
        explicit Adapter(MCP23017Device& d) : device(d) {}
        bool exclusive() const {
            return device._leased && device._wire &&
                I2CBusManager::instance().ownsAddress(&device, device._address);
        }
        bool read8(uint8_t reg, uint8_t& result) {
            auto* wire = device._wire;
            wire->beginTransmission(device._address);
            const size_t count = wire->write(reg);
            const uint8_t status = wire->endTransmission(count != 1);
            if (count != 1 || status != 0) return false;
            if (wire->requestFrom(device._address, static_cast<uint8_t>(1)) != 1) {
                discard(); return false;
            }
            const int value = wire->available() > 0 ? wire->read() : -1;
            if (value < 0 || value > 255 || wire->available() > 0) { discard(); return false; }
            result = static_cast<uint8_t>(value); return true;
        }
        bool write8(uint8_t reg, uint8_t value) {
            auto* wire = device._wire;
            const uint8_t bytes[] = {reg, value};
            wire->beginTransmission(device._address);
            const size_t count = wire->write(bytes, sizeof(bytes));
            const uint8_t status = wire->endTransmission();
            return count == sizeof(bytes) && status == 0;
        }
        void discard() {
            // A broken adapter must not retain the global lock indefinitely.
            for (unsigned n = 0; n < 255 && device._wire->available() > 0; ++n)
                if (device._wire->read() < 0) break;
        }
    };
    bool fail(Error error) { _error = error; return false; }
    MCP23017PortSession _session;
    TwoWire* _wire = nullptr;
    uint8_t _address = 0;
    bool _leased = false;
    Error _error = Error::None;
};
} // namespace FastBee
#endif
