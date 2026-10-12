#pragma once
#include <Arduino.h>
#include <atomic>
#include "InternalAtomicWord.h"
#include "ResourceConsumerIdentity.h"

namespace FastBee {
// Commands apply only to an already configured, dedicated native output.
// HIGH is inactive for both MCP RESET and PCA OE. No persisted reset evidence.
enum class ControlOutputCommand : uint8_t { Claim, Owned, IsHigh, High, Low, PulseReset, Release };

class ControlOutput {
public:
    ControlOutput() = default;
    ~ControlOutput();
    ControlOutput(const ControlOutput&) = delete;
    ControlOutput& operator=(const ControlOutput&) = delete;
    bool begin(const String& peripheralId, const String& consumerId = "",
               ResourceConsumerRole role = ResourceConsumerRole::Unknown);
    bool owned() const;
    bool isHigh() const;
    bool high();
    bool low();
    bool pulseReset();
    bool end();
    int pin() const { return _pin; }
private:
    bool command(ControlOutputCommand operation) const;
    String _id;
    ResourceConsumerIdentity _consumer;
    mutable int _pin = -1;
    // Manager-owned permanent internal-DRAM slot, checked before claim. Never
    // a pointer into this object's lifetime or a potentially blocking PSRAM atomic.
    // Only the owning object may use this handle; concurrent object destruction
    // and method calls are not supported.
    mutable std::atomic<bool>* _retirement = nullptr;
};
} // namespace FastBee
