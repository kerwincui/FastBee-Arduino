#ifndef FASTBEE_PCA9685_OUTPUT_SESSION_H
#define FASTBEE_PCA9685_OUTPUT_SESSION_H

#include "peripherals/drivers/PCA9685Contract.h"
#include <cstddef>

namespace FastBee {

// NXP PCA9685 Rev.4 sections 7.3/7.4. Driver work in progress, UNTESTED.
// One owner retains the entire chip and default all-call address. Each method
// runs under the shared bus lock; caller serializes this object's lifetime.
// Bus: exclusive(), read(reg,data,size), write(reg,data,size), nowMicros(),
// disableOutputs(), outputsDisabled(). OE is a separately owned hardware gate;
// software acknowledgement is not proof of the voltage at a physical load.
class PCA9685OutputSession {
public:
    enum class ResetEvidence { Unknown, ConfirmedPowerOn };
    enum class State { Idle, Waking, Ready, Failed, Stopped };
    enum class Error { None, InvalidConfig, AlreadyActive, ResetRequired,
                       OutputGate, OwnershipLost, Transport, ResetMismatch,
                       ConfigLost, ChannelLost, NotReady, WakeTimeout };
    PCA9685OutputSession() = default;
    PCA9685OutputSession(const PCA9685OutputSession&) = delete;
    PCA9685OutputSession& operator=(const PCA9685OutputSession&) = delete;

    template<class Bus>
    bool begin(Bus& bus, ResetEvidence reset, double frequencyHz, double clockHz,
               uint32_t channelMask, bool openDrain) {
        if (_state == State::Ready || _state == State::Waking) return reject(Error::AlreadyActive);
        PCA9685FrequencyConfig config;
        if (!channelMask || channelMask > 0xffff ||
            !makePCA9685FrequencyConfig(frequencyHz, clockHz, config)) return reject(Error::InvalidConfig);
        if (reset != ResetEvidence::ConfirmedPowerOn) return reject(Error::ResetRequired);
        _known = 0; _state = State::Idle;
        if (!bus.disableOutputs() || !bus.outputsDisabled()) return fail(bus, Error::OutputGate);
        uint8_t value = 0;
        // Reject inherited EXTCLK, aliases or active PWM. Register defaults are
        // consistency checks, never a replacement for the power-on evidence.
        const uint8_t regs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0xfe};
        const uint8_t defaults[] = {0x11, 0x04, 0xe2, 0xe4, 0xe8, 0xe0, 0x1e};
        for (size_t i = 0; i < sizeof(regs); ++i) {
            if (!read(bus, regs[i], &value, 1)) return false;
            if (value != defaults[i]) return fail(bus, Error::ResetMismatch);
        }
        _config = config; _channels = static_cast<uint16_t>(channelMask);
        _mode2 = openDrain ? 0 : 4; // non-inverted, update on STOP, OE-high -> low
        // Enable auto-increment while asleep; disable all-call/subcalls. Do not
        // send a general-call reset or attempt to clear sticky EXTCLK by write.
        if (!write8(bus, 0, 0x30) || !expect8(bus, 0, 0x30) ||
            !write8(bus, 1, _mode2) || !expect8(bus, 1, _mode2) ||
            !write8(bus, 0xfe, _config.prescale) || !expect8(bus, 0xfe, _config.prescale)) return false;
        const uint8_t off[] = {0, 0, 0, 0x10};
        // ALL_LED registers read as zero; confirm the individual channel
        // registers instead. This deliberately owns/initializes all 16 outputs.
        if (!write(bus, 0xfa, off, sizeof(off))) return false;
        for (uint8_t channel = 0; channel < 16; ++channel) {
            for (uint8_t i = 0; i < 4; ++i) _shadow[channel][i] = off[i];
            if (!verifyChannel(bus, channel)) return false;
        }
        if (!write8(bus, 0, 0x20)) return false;
        // Start the 500us window AFTER the wake write has completed. No delay
        // under the I2C lock; service() completes in a later scheduler turn.
        _wokeUs = bus.nowMicros(); _state = State::Waking; _error = Error::None;
        return true;
    }

    template<class Bus> bool service(Bus& bus) {
        if (_state != State::Waking) return reject(Error::NotReady);
        if (!bus.outputsDisabled()) return fail(bus, Error::OutputGate);
        if (!owned(bus)) return false;
        const uint32_t elapsed = static_cast<uint32_t>(bus.nowMicros() - _wokeUs);
        if (elapsed >= 1000000U) return fail(bus, Error::WakeTimeout); // project admission bound
        if (elapsed < 500U) return true;
        if (!verifyConfig(bus)) return false;
        for (uint8_t channel = 0; channel < 16; ++channel)
            if (!verifyChannel(bus, channel)) return false;
        _known = 0xffff; _state = State::Ready; _error = Error::None;
        return true; // OE stays disabled; enabling is a separate explicit action.
    }

    template<class Bus>
    bool setChannel(Bus& bus, uint32_t channel, uint32_t phase, uint32_t widthTicks) {
        if (_state != State::Ready) return reject(Error::NotReady);
        PCA9685ChannelRegisters update;
        if (!makePCA9685ChannelRegisters(channel, phase, widthTicks, update) ||
            !(_channels & (1U << channel))) return reject(Error::InvalidConfig);
        if (!verifyConfig(bus) || !verifyChannel(bus, static_cast<uint8_t>(channel))) return false;
        // A single pointer+four-byte transfer updates on STOP. A short transfer
        // may still partially change the chip: fail disables OE, no rollback.
        if (!write(bus, update.startRegister, update.bytes, sizeof(update.bytes))) return false;
        for (uint8_t i = 0; i < 4; ++i) _shadow[channel][i] = update.bytes[i];
        if (!verifyChannel(bus, static_cast<uint8_t>(channel))) return false;
        _error = Error::None; return true;
    }
    template<class Bus> bool confirmForEnable(Bus& bus) {
        if (_state != State::Ready) return reject(Error::NotReady);
        if (!bus.outputsDisabled()) return fail(bus, Error::OutputGate);
        if (!verifyConfig(bus)) return false;
        for (uint8_t channel = 0; channel < 16; ++channel)
            if (!verifyChannel(bus, channel)) return false;
        return true;
    }
    template<class Bus> bool stop(Bus& bus) {
        const bool gated = bus.disableOutputs() && bus.outputsDisabled();
        _known = 0; _state = State::Stopped;
        if (!gated) return reject(Error::OutputGate);
        _error = Error::None; return true;
    }
    State state() const { return _state; }
    Error error() const { return _error; }
    bool ready() const { return _state == State::Ready; }
    double calculatedFrequencyHz() const { return _config.actualHz; }
    bool knownChannel(uint32_t channel, PCA9685ChannelRegisters& output) const {
        if (!ready() || channel >= 16 || !(_known & (1U << channel))) return false;
        output = {}; output.startRegister = static_cast<uint8_t>(6 + 4 * channel);
        for (uint8_t i = 0; i < 4; ++i) output.bytes[i] = _shadow[channel][i];
        output.valid = true; return true;
    }

private:
    bool reject(Error error) { _error = error; return false; }
    template<class Bus> bool fail(Bus& bus, Error error) {
        _known = 0; _state = State::Failed; _error = error;
        // Preserve the original bus/config failure even if gating also fails.
        bus.disableOutputs(); return false;
    }
    template<class Bus> bool owned(Bus& bus) {
        return bus.exclusive() || fail(bus, Error::OwnershipLost);
    }
    template<class Bus> bool read(Bus& bus, uint8_t reg, uint8_t* data, size_t size) {
        return owned(bus) && (bus.read(reg, data, size) || fail(bus, Error::Transport));
    }
    template<class Bus> bool write(Bus& bus, uint8_t reg, const uint8_t* data, size_t size) {
        return owned(bus) && (bus.write(reg, data, size) || fail(bus, Error::Transport));
    }
    template<class Bus> bool write8(Bus& bus, uint8_t reg, uint8_t value) {
        return write(bus, reg, &value, 1);
    }
    template<class Bus> bool expect8(Bus& bus, uint8_t reg, uint8_t expected) {
        uint8_t value = 0;
        return read(bus, reg, &value, 1) && (value == expected || fail(bus, Error::ConfigLost));
    }
    template<class Bus> bool verifyConfig(Bus& bus) {
        return expect8(bus, 0, 0x20) && expect8(bus, 1, _mode2) && expect8(bus, 0xfe, _config.prescale);
    }
    template<class Bus> bool verifyChannel(Bus& bus, uint8_t channel) {
        uint8_t data[4] = {};
        if (!read(bus, static_cast<uint8_t>(6 + 4 * channel), data, sizeof(data))) return false;
        for (uint8_t i = 0; i < 4; ++i)
            if (data[i] != _shadow[channel][i]) return fail(bus, Error::ChannelLost);
        return true;
    }
    PCA9685FrequencyConfig _config;
    uint8_t _shadow[16][4] = {};
    uint16_t _channels = 0, _known = 0;
    uint32_t _wokeUs = 0;
    uint8_t _mode2 = 4;
    State _state = State::Idle;
    Error _error = Error::None;
};
} // namespace FastBee
#endif
