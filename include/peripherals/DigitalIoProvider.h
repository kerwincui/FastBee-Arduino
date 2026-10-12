#pragma once

#include "peripherals/IoExpander.h"

namespace FastBee {

// Manager-facing digital I/O contract. Calls and observations are serialized by
// PeripheralManager; each implementation owns its bus admission/recovery policy.
// This interface does not grant ownership of any channel or register a driver.
class DigitalIoProvider {
public:
    DigitalIoProvider() = default;
    virtual ~DigitalIoProvider() = default;
    DigitalIoProvider(const DigitalIoProvider&) = delete;
    DigitalIoProvider& operator=(const DigitalIoProvider&) = delete;
    virtual uint16_t channelMask() const = 0;
    bool supportsChannel(uint8_t channel) const {
        return channel < 16 && (channelMask() & (uint16_t(1) << channel)) != 0;
    }
    virtual bool begin(uint16_t outputs, uint16_t startupHigh, uint16_t intervalMs,
                       uint32_t now, uint16_t inputs = 0) = 0;
    virtual bool configure(uint16_t outputs, uint16_t startupHigh, uint32_t now,
                           uint16_t inputs = 0) = 0;
    virtual bool poll(uint32_t now) = 0;
    virtual bool writeChannel(uint8_t channel, bool high, uint32_t now) = 0;
    virtual bool readChannel(uint8_t channel, bool output, uint32_t now, bool& high) const = 0;
    virtual bool healthy(uint32_t now) const = 0;
    virtual bool initialized() const = 0;
    virtual uint32_t generation() const = 0;
    virtual uint32_t sampledAt() const = 0;
    virtual uint8_t failures() const = 0;
    virtual bool closing() const = 0;
    // Busy retains the object and any remaining bus/control leases. Once
    // closing(), Ok/Error are terminal and must release all leases before
    // returning. Invalid masks may reject
    // with Error BEFORE closing; the manager must not destroy that live owner.
    virtual IoExpanderResult shutdown(uint16_t safeHigh) = 0;
};

// Shared production adapter, also used with a fake byte transport in PM tests.
// Reject wider values before narrowing: channel 8 must never alias channel 0.
// PCF recovery/startup behavior remains entirely in the existing state machine.
class Pcf8574ProviderSession : public DigitalIoProvider {
public:
    explicit Pcf8574ProviderSession(IoExpanderTransport& transport) : driver(transport) {}
    uint16_t channelMask() const override { return 0x00ff; }
    bool begin(uint16_t outputs, uint16_t startupHigh, uint16_t intervalMs,
               uint32_t now, uint16_t inputs = 0) override {
        return !closing() && fits(outputs, startupHigh, inputs) &&
            driver.begin(static_cast<uint8_t>(outputs), static_cast<uint8_t>(startupHigh),
                         intervalMs, now, static_cast<uint8_t>(inputs));
    }
    bool configure(uint16_t outputs, uint16_t startupHigh, uint32_t now,
                   uint16_t inputs = 0) override {
        return !closing() && fits(outputs, startupHigh, inputs) &&
            driver.configure(static_cast<uint8_t>(outputs), static_cast<uint8_t>(startupHigh),
                             now, static_cast<uint8_t>(inputs));
    }
    bool poll(uint32_t now) override { return !closing() && driver.poll(now); }
    bool writeChannel(uint8_t channel, bool high, uint32_t now) override {
        return !closing() && supportsChannel(channel) && driver.writeChannel(channel, high, now);
    }
    bool readChannel(uint8_t channel, bool output, uint32_t now, bool& high) const override {
        return !closing() && supportsChannel(channel) && driver.readChannel(channel, output, now, high);
    }
    bool healthy(uint32_t now) const override { return !closing() && driver.healthy(now); }
    bool initialized() const override { return !closing() && driver.initialized(); }
    uint32_t generation() const override { return driver.generation(); }
    uint32_t sampledAt() const override { return driver.sampledAt(); }
    uint8_t failures() const override { return driver.failures(); }
    Pcf8574 driver;
private:
    static bool fits(uint16_t outputs, uint16_t startupHigh, uint16_t inputs) {
        return ((outputs | startupHigh | inputs) & 0xff00U) == 0;
    }
};

} // namespace FastBee
