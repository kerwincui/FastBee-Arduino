#pragma once

#include <stdint.h>

namespace FastBee {

enum class IoExpanderResult : uint8_t { Ok, Busy, Error };

// Byte transport is injected so the real state machine is testable without
// Arduino, and the provider can share the application's I2C owner.
class IoExpanderTransport {
public:
    virtual ~IoExpanderTransport() = default;
    virtual IoExpanderResult writeByte(uint8_t value) = 0;
    virtual IoExpanderResult readByte(uint8_t& value) = 0;
};

class Pcf8574 {
public:
    explicit Pcf8574(IoExpanderTransport& transport) : _transport(transport) {}
    bool begin(uint8_t outputMask, uint8_t startupHigh, uint16_t intervalMs,
               uint32_t now, uint8_t inputMask = 0);
    bool configure(uint8_t outputMask, uint8_t startupHigh, uint32_t now,
                   uint8_t inputMask = 0);
    bool poll(uint32_t now);
    bool writeChannel(uint8_t channel, bool high, uint32_t now);
    bool readChannel(uint8_t channel, bool output, uint32_t now, bool& high) const;
    bool healthy(uint32_t now) const;
    // Passive initialization state only; not freshness or physical health.
    // Access under the owning PeripheralManager lock, like other driver fields.
    bool initialized() const { return _started && _outputsKnown && !_recover; }
    uint32_t generation() const { return _generation; }
    uint32_t sampledAt() const { return _sampledAt; }
    uint8_t outputShadow() const { return _shadow; }
    uint8_t startupByte() const { return _startup; }
    uint8_t failures() const { return _failures; }
    IoExpanderResult shutdown(uint8_t safeByte);
    void end();

private:
    bool fail(bool uncertainWrite, uint32_t now);
    IoExpanderTransport& _transport;
    uint8_t _outputMask = 0;
    uint8_t _inputMask = 0;
    uint8_t _startup = 0xff;
    uint8_t _shadow = 0xff;
    uint8_t _sample = 0xff;
    uint8_t _failures = 0;
    uint16_t _interval = 20;
    uint16_t _retryDelay = 0;
    uint32_t _lastAttempt = 0;
    uint32_t _sampledAt = 0;
    uint32_t _generation = 0;
    bool _started = false;
    bool _valid = false;
    bool _outputsKnown = false;
    bool _recover = false;
};

} // namespace FastBee
