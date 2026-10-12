#pragma once

#include <memory>
#include "peripherals/DigitalIoProvider.h"
#include "peripherals/drivers/MCP23017Device.h"

namespace FastBee {

// Production transport/session adapter used by the digital-I/O factory.
// The integrating manager must validate the complete child group and
// native RESET resource before transferring its claimed control lease here.
// All calls/getters require the manager's state lock, including destruction.
class MCP23017Provider final : public DigitalIoProvider {
public:
    enum class Error : uint8_t {
        None, Arguments, AlreadyAttempted, Busy, ResetOwnership, Begin,
        Session, NotReady, Closing, StopUnconfirmed, ControlRelease
    };
    MCP23017Provider(int sda, int scl, uint32_t frequency, uint32_t address,
                    std::unique_ptr<MCP23017ResetLease> reset)
        : _reset(std::move(reset)), _sda(sda), _scl(scl),
          _frequency(frequency), _address(address) {}
    // Destruction only relinquishes software leases. Explicit shutdown is
    // required for a confirmed final latch; neither path proves load safety.
    ~MCP23017Provider() override = default;
    uint16_t channelMask() const override { return 0xffff; }
    bool begin(uint16_t outputs, uint16_t startupHigh, uint16_t intervalMs,
               uint32_t now, uint16_t inputs = 0) override;
    bool configure(uint16_t outputs, uint16_t startupHigh, uint32_t now,
                   uint16_t inputs = 0) override;
    bool poll(uint32_t now) override;
    bool writeChannel(uint8_t channel, bool high, uint32_t now) override;
    bool readChannel(uint8_t channel, bool output, uint32_t now, bool& high) const override;
    bool healthy(uint32_t now) const override;
    bool initialized() const override { return !_closing && !_faulted && _device.session().ready(); }
    uint32_t generation() const override { return _generation; }
    uint32_t sampledAt() const override { return _device.session().sample().capturedMs; }
    uint8_t failures() const override { return _failures; }
    bool closing() const override { return _closing; }
    IoExpanderResult shutdown(uint16_t safeHigh) override;
    Error error() const { return _error; }
    MCP23017PortSession::Error sessionError() const { return _sessionError; }

private:
    bool reject(Error error) { _error = error; return false; }
    bool resetValid() const;
    bool fault(Error error);
    bool checkReset();
    bool sample(uint32_t now);
    // Destruction order is device, then RESET. Do not reorder these members:
    // the pin lease must outlive the device's final bus release/deferRelease.
    std::unique_ptr<MCP23017ResetLease> _reset;
    MCP23017Device _device;
    int _sda, _scl, _resetPin = -1;
    uint32_t _frequency, _address;
    uint16_t _outputs = 0, _inputs = 0, _interval = 20, _stopHigh = 0;
    uint32_t _lastAttempt = 0, _generation = 0;
    uint8_t _failures = 0;
    bool _attempted = false, _faulted = false, _closing = false;
    bool _stopApplied = false, _deviceEnded = false, _terminal = false;
    IoExpanderResult _stopResult = IoExpanderResult::Error;
    Error _error = Error::None;
    MCP23017PortSession::Error _sessionError = MCP23017PortSession::Error::None;
};

} // namespace FastBee
