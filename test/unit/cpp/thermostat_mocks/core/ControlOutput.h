#pragma once
#include <Arduino.h>
namespace FastBee {
struct ThermostatOutputFixture {
    bool claimFails = false, releaseFails = false, highFails = false, lowFails = false;
    bool high = true;
    const void* owner = nullptr;
    unsigned claims = 0, highs = 0, lows = 0, releases = 0;
};
inline ThermostatOutputFixture thermostatOutput;
class ControlOutput {
public:
    ~ControlOutput() { if (pin_ >= 0) end(); }
    bool begin(const String&) {
        ++thermostatOutput.claims;
        if (thermostatOutput.owner) return false;
        thermostatOutput.owner = this; pin_ = 26;
        if (thermostatOutput.claimFails) return false;
        thermostatOutput.high = true; return true;
    }
    bool owned() const { return thermostatOutput.owner == this; }
    bool isHigh() const { return owned() && thermostatOutput.high; }
    bool high() {
        ++thermostatOutput.highs;
        if (!owned() || thermostatOutput.highFails) return false;
        thermostatOutput.high = true; return true;
    }
    bool low() {
        ++thermostatOutput.lows;
        if (!owned() || thermostatOutput.lowFails) return false;
        thermostatOutput.high = false; return true;
    }
    bool end() {
        if (pin_ < 0) return true;
        ++thermostatOutput.releases;
        if (!owned() || thermostatOutput.releaseFails) return false;
        thermostatOutput.high = true; thermostatOutput.owner = nullptr; pin_ = -1;
        return true;
    }
    int pin() const { return pin_; }
private:
    int pin_ = -1;
};
}
