#pragma once
#include "core/ControlOutput.h"
#include "peripherals/drivers/MCP23017Device.h"
#include "peripherals/drivers/PCA9685Device.h"

namespace FastBee {
// Integration requires a dedicated, initialized native output configuration,
// initially HIGH, non-inverted and without a running action ticker. These
// adapters do not create/enable GPIO configs or declare physical wiring safe.
class MCP23017NativeReset final : public MCP23017ResetLease {
public:
    bool begin(const String& id, const String& consumerId = "") {
        return _output.begin(id, consumerId, ResourceConsumerRole::McpReset);
    }
    bool end() { return _output.end(); }
    bool release() override { return end(); }
    int pin() const override { return _output.pin(); }
    bool owned() const override { return _output.owned(); }
    bool pulse() override { return _output.pulseReset(); }
private:
    ControlOutput _output;
};

// Keep this adapter alive until PCA9685Device::end has succeeded/destruction
// has completed. Disabled means local OE pad HIGH, not verified load-off.
class PCA9685NativeGate final : public PCA9685OutputLease {
public:
    bool begin(const String& id, const String& consumerId = "") {
        return _output.begin(id, consumerId, ResourceConsumerRole::PcaGate);
    }
    bool end() { return _output.end(); }
    bool release() override { return end(); }
    int pin() const override { return _output.pin(); }
    bool owned() const override { return _output.owned(); }
    bool disable() override { return _output.high(); }
    bool enable() override { return _output.low(); }
    bool disabled() const override { return _output.isHigh(); }
private:
    ControlOutput _output;
};
} // namespace FastBee
