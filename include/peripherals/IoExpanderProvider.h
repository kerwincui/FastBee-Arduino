#pragma once

#include <Arduino.h>
#include "peripherals/DigitalIoProvider.h"

namespace FastBee {

// Owned by PeripheralManager; all callers hold its recursive state lock.
class IoExpanderProvider final : public Pcf8574ProviderSession, public IoExpanderTransport {
public:
    explicit IoExpanderProvider(uint8_t address)
        : Pcf8574ProviderSession(static_cast<IoExpanderTransport&>(*this)), _address(address) {}
    ~IoExpanderProvider() override;
    bool acquire(uint8_t sda, uint8_t scl, String& error);
    // Busy keeps this object and its lease alive for a later maintenance retry.
    // Ok/Error release while holding the bus lock; destruction then cannot wait.
    IoExpanderResult shutdown(uint16_t safeHigh) override;
    bool closing() const override { return _closing; }
    IoExpanderResult writeByte(uint8_t value) override;
    IoExpanderResult readByte(uint8_t& value) override;
private:
    uint8_t _address;
    uint8_t _shutdownByte = 0xff;
    bool _leased = false;
    bool _closing = false;
    IoExpanderResult _shutdownResult = IoExpanderResult::Error;
};

} // namespace FastBee
