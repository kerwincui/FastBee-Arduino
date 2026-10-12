#ifndef FASTBEE_MCP23017_PORT_SESSION_H
#define FASTBEE_MCP23017_PORT_SESSION_H

#include "peripherals/drivers/MCP23017Contract.h"

namespace FastBee {

// Register session for MCP23017 (DS20001952D). Not a registered provider.
// The integrating driver must hold one recursive bus lock for EACH public call,
// retain exclusive address ownership for the whole session, and implement:
//   bool exclusive(); bool read8(uint8_t, uint8_t&); bool write8(uint8_t, uint8_t);
// Transport failures include short transfers and lost ownership. No retries.
// ConfirmedBank0Reset is an external reset/power-on guarantee, NOT something
// inferred from register values or a software restart. Never probe-write to
// infer BANK: the same address may be an output latch in the other bank.
class MCP23017PortSession {
public:
    enum class ResetEvidence { Unknown, ConfirmedBank0Reset };
    enum class Error { None, InvalidConfig, ResetRequired, OwnershipLost,
                       Transport, ResetMismatch, ConfigLost, LatchLost, NotReady };
    struct Sample {
        uint16_t inputs = 0;
        uint32_t capturedMs = 0;
        bool valid = false;
    };
    MCP23017PortSession() = default;
    MCP23017PortSession(const MCP23017PortSession&) = delete;
    MCP23017PortSession& operator=(const MCP23017PortSession&) = delete;

    template<class Bus>
    bool begin(Bus& bus, ResetEvidence evidence, uint32_t outputs, uint32_t inputs,
               uint32_t startupHigh, uint32_t pullups, uint32_t inversion) {
        // Caller must first stop/serialize a previous session and provide NEW
        // reset evidence. Invalidation itself does not drive physical outputs.
        forget();
        if (!makeMCP23017PortConfig(outputs, inputs, startupHigh, pullups,
                                   inversion, _config)) return fail(Error::InvalidConfig);
        if (evidence != ResetEvidence::ConfirmedBank0Reset) return fail(Error::ResetRequired);
        if (!owned(bus)) return false;
        // Sanity check the promised reset before the first output write. These
        // values cannot independently prove BANK or the physical reset event.
        const uint8_t regs[] = {0x0a, 0x0b, 0x00, 0x01, 0x04, 0x05,
                                0x14, 0x15, 0x02, 0x03, 0x0c, 0x0d};
        for (uint8_t reg : regs) {
            uint8_t value = 0;
            if (!read(bus, reg, value)) return false;
            if (value != (reg <= 1 ? 0xff : 0)) return fail(Error::ResetMismatch);
        }
        _outputs = static_cast<uint16_t>(outputs);
        _inputs = static_cast<uint16_t>(inputs);
        _latch = static_cast<uint16_t>(startupHigh);
        // Individual addressed bytes avoid assumptions about SEQOP. Preload
        // BOTH OLATs before either IODIR enables outputs (including A7/B7).
        if (!writePair(bus, 0x14, _config.latch) || !verifyLatch(bus) ||
            !writePair(bus, 0x02, _config.polarity) ||
            !writePair(bus, 0x0c, _config.pullup) ||
            !writePair(bus, 0x00, _config.direction)) return false;
        if (!verify(bus)) return false;
        _ready = true; _error = Error::None;
        return true;
    }

    template<class Bus>
    bool configure(Bus& bus, uint32_t outputs, uint32_t inputs,
                   uint32_t startupHigh, uint32_t pullups, uint32_t inversion) {
        if (!_ready) return reject(Error::NotReady);
        MCP23017PortConfig next;
        if (!makeMCP23017PortConfig(outputs, inputs, startupHigh, pullups, inversion, next))
            return reject(Error::InvalidConfig);
        // Keep live levels on continuously owned outputs. Startup applies only
        // to newly acquired output bits; a sibling edit is never a chip reset.
        const uint16_t retained = static_cast<uint16_t>(_outputs & outputs);
        const uint16_t added = static_cast<uint16_t>(outputs & ~_outputs);
        const uint16_t desired = static_cast<uint16_t>((_latch & retained) | (startupHigh & added));
        next.latch[0] = static_cast<uint8_t>(desired);
        next.latch[1] = static_cast<uint8_t>(desired >> 8);
        if (!verify(bus)) return false; // Drift/ownership loss: no repair writes.
        if (_outputs == outputs && _inputs == inputs &&
            _config.pullup[0] == next.pullup[0] && _config.pullup[1] == next.pullup[1] &&
            _config.polarity[0] == next.polarity[0] && _config.polarity[1] == next.polarity[1]) {
            _error = Error::None;
            return true;
        }

        // Release retired outputs BEFORE changing their latch/pull/polarity;
        // leave new outputs as inputs until both OLAT bytes are read back.
        // High impedance is not proof of a safe physical load state.
        const uint8_t interim[] = {static_cast<uint8_t>(~retained),
                                  static_cast<uint8_t>(static_cast<uint16_t>(~retained) >> 8)};
        if ((_config.direction[0] != interim[0] || _config.direction[1] != interim[1]) &&
            (!writePair(bus, 0x00, interim) || !verifyPair(bus, 0x00, interim, Error::ConfigLost)))
            return false;
        if (desired != _latch &&
            (!writePair(bus, 0x14, next.latch) || !verifyPair(bus, 0x14, next.latch, Error::LatchLost)))
            return false;
        if ((_config.polarity[0] != next.polarity[0] || _config.polarity[1] != next.polarity[1]) &&
            !writePair(bus, 0x02, next.polarity)) return false;
        if ((_config.pullup[0] != next.pullup[0] || _config.pullup[1] != next.pullup[1]) &&
            !writePair(bus, 0x0c, next.pullup)) return false;
        // Readback is required even if the numeric latch stayed unchanged.
        if (!verifyPair(bus, 0x14, next.latch, Error::LatchLost)) return false;
        if ((interim[0] != next.direction[0] || interim[1] != next.direction[1]) &&
            !writePair(bus, 0x00, next.direction)) return false;
        _config = next;
        _outputs = static_cast<uint16_t>(outputs);
        _inputs = static_cast<uint16_t>(inputs);
        _latch = desired;
        _sample = {};
        if (!verify(bus)) return false;
        _error = Error::None;
        return true;
    }

    template<class Bus>
    bool setOutputs(Bus& bus, uint32_t selected, uint32_t high) {
        if (!_ready) return reject(Error::NotReady);
        uint16_t desired = 0;
        if (!updateMCP23017Latch(_latch, _outputs, selected, high, desired))
            return reject(Error::InvalidConfig); // Invalid command does not alter known state.
        if (!verify(bus)) return false;
        // Never read GPIO to synthesize an output latch. GPIO can differ under
        // load, and includes input polarity. Two ports are NOT an atomic write.
        for (uint8_t port = 0; port < 2; ++port) {
            if (selected & (0xffU << (port * 8U))) {
                if (!write(bus, static_cast<uint8_t>(0x14 + port),
                           static_cast<uint8_t>(desired >> (port * 8U)))) return false;
            }
        }
        // A NACK can occur after a device accepted a byte: never roll back or
        // keep an old 'known' shadow after ANY failed/partial transaction.
        _latch = desired;
        if (!verify(bus)) return false;
        _sample = {}; _error = Error::None;
        return true;
    }

    template<class Bus>
    bool sampleInputs(Bus& bus, uint32_t nowMs) {
        if (!_ready) return reject(Error::NotReady);
        if (!verify(bus)) return false;
        uint8_t a = 0, b = 0;
        if (!read(bus, 0x12, a) || !read(bus, 0x13, b)) return false;
        // A/B are sequential observations, not a simultaneous 16-bit sample.
        _sample.inputs = static_cast<uint16_t>((a | (static_cast<uint16_t>(b) << 8)) & _inputs);
        _sample.capturedMs = nowMs; _sample.valid = true; _error = Error::None;
        return true;
    }

    void forget() { _ready = false; _sample = {}; _latch = 0; _error = Error::None; }
    bool ready() const { return _ready; }
    Error error() const { return _error; }
    const Sample& sample() const { return _sample; }
    bool knownLatch(uint16_t& value) const {
        if (!_ready) return false;
        value = _latch; return true;
    }

private:
    bool reject(Error error) { _error = error; return false; }
    bool fail(Error error) { _ready = false; _sample = {}; _latch = 0; return reject(error); }
    template<class Bus> bool owned(Bus& bus) {
        return bus.exclusive() || fail(Error::OwnershipLost);
    }
    template<class Bus> bool read(Bus& bus, uint8_t reg, uint8_t& value) {
        return owned(bus) && (bus.read8(reg, value) || fail(Error::Transport));
    }
    template<class Bus> bool write(Bus& bus, uint8_t reg, uint8_t value) {
        return owned(bus) && (bus.write8(reg, value) || fail(Error::Transport));
    }
    template<class Bus> bool writePair(Bus& bus, uint8_t reg, const uint8_t* data) {
        return write(bus, reg, data[0]) && write(bus, reg + 1, data[1]);
    }
    template<class Bus>
    bool verifyPair(Bus& bus, uint8_t reg, const uint8_t* expected, Error mismatch) {
        for (uint8_t port = 0; port < 2; ++port) {
            uint8_t value = 0;
            if (!read(bus, static_cast<uint8_t>(reg + port), value)) return false;
            if (value != expected[port]) return fail(mismatch);
        }
        return true;
    }
    template<class Bus> bool verifyLatch(Bus& bus) {
        // ACK alone cannot confirm preloaded startup levels. Confirm both
        // latches before enabling either output port; do not trust only writes.
        for (uint8_t port = 0; port < 2; ++port) {
            uint8_t value = 0;
            if (!read(bus, static_cast<uint8_t>(0x14 + port), value)) return false;
            if (value != static_cast<uint8_t>(_latch >> (port * 8U))) return fail(Error::LatchLost);
        }
        return true;
    }
    template<class Bus> bool verify(Bus& bus) {
        const uint8_t regs[] = {0x0a, 0x0b, 0x04, 0x05, 0x00, 0x01,
                                0x02, 0x03, 0x0c, 0x0d, 0x14, 0x15};
        const uint8_t expected[] = {0, 0, 0, 0, _config.direction[0], _config.direction[1],
            _config.polarity[0], _config.polarity[1], _config.pullup[0], _config.pullup[1],
            static_cast<uint8_t>(_latch), static_cast<uint8_t>(_latch >> 8)};
        for (uint8_t i = 0; i < sizeof(regs); ++i) {
            uint8_t value = 0;
            if (!read(bus, regs[i], value)) return false;
            if (value != expected[i]) return fail(i >= 10 ? Error::LatchLost : Error::ConfigLost);
        }
        return true;
    }
    MCP23017PortConfig _config;
    Sample _sample;
    uint16_t _outputs = 0, _inputs = 0, _latch = 0;
    bool _ready = false;
    Error _error = Error::None;
};
} // namespace FastBee
#endif
