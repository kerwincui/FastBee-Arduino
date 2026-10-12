#ifndef FASTBEE_ADS1115_CONVERSION_SESSION_H
#define FASTBEE_ADS1115_CONVERSION_SESSION_H

#include "peripherals/drivers/ADS1115Contract.h"
#include <cstdint>
#include <limits>

namespace FastBee {

// H1 protocol state machine, not a registered/instantiated hardware driver.
// TI SBAS444E sections 7.4.2 and 8.1.3: OS does not restart an active
// conversion. Drain an existing conversion before triggering our own.
// One caller must serialize this object and exclusively own the bus/address
// throughout a session. A bus transaction lock alone is NOT address ownership.
// Polls/ACKs are serialized, tagged with generation, and never cached/replayed.
class ADS1115ConversionSession {
public:
    ADS1115ConversionSession() = default;
    ADS1115ConversionSession(const ADS1115ConversionSession&) = delete;
    ADS1115ConversionSession& operator=(const ADS1115ConversionSession&) = delete;
    enum class Operation { None, ReadStatus, WriteIdle, WriteStart, ReadSample };
    enum class State { Idle, AwaitIdle, StopPending, StartPending, Converting,
                       SamplePending, Complete, Failed, Cancelled };
    enum class Error { None, InvalidConfig, InvalidTimeout, GenerationExhausted,
                       Timeout, StartNotObserved, ConfigLost, Transport, Cancelled };

    // A busy begin fails without replacing the active request or its result.
    // Returns a nonzero generation only on admission. Not thread-safe by itself.
    uint64_t begin(uint32_t mux, uint32_t fullScaleMv, uint32_t rate,
                   uint32_t nowMs, uint32_t timeoutMs) {
        if (pending()) return 0;
        _sample = {}; _sampleTimeMs = 0; _observedBusy = false;
        _state = State::Failed; _error = Error::None;
        if (_generation == std::numeric_limits<uint64_t>::max()) {
            _error = Error::GenerationExhausted; return 0;
        }
        ++_generation; // Never reuse a token, including rejected requests.
        if (!makeADS1115ConversionConfig(mux, fullScaleMv, rate, _config)) {
            _error = Error::InvalidConfig; return 0;
        }
        // Caller policy, not a manufacturer conversion-time guarantee. A short
        // timeout can fail conservatively; nominalPeriodUs is never readiness.
        if (!timeoutMs || timeoutMs > 1000) {
            _error = Error::InvalidTimeout; return 0;
        }
        _fullScaleMv = fullScaleMv; _startedMs = nowMs; _timeoutMs = timeoutMs;
        _state = State::AwaitIdle;
        return _generation;
    }

    Operation operation() const {
        switch (_state) {
        case State::AwaitIdle: case State::Converting: return Operation::ReadStatus;
        case State::StopPending: return Operation::WriteIdle;
        case State::StartPending: return Operation::WriteStart;
        case State::SamplePending: return Operation::ReadSample;
        default: return Operation::None;
        }
    }
    uint16_t writeValue() const {
        return _state == State::StopPending ? _idleConfig :
            (_state == State::StartPending ? _config.registerValue : 0);
    }

    bool observeStatus(uint64_t generation, uint16_t value, uint32_t nowMs) {
        if (!accept(generation, nowMs)) return false;
        if (_state == State::AwaitIdle) {
            if (!(value & 0x0100U)) {
                // Stop continuous mode without a general-call reset and without
                // changing MUX/gain/rate. Its last old conversion is discarded.
                _idleConfig = static_cast<uint16_t>((value & 0x7fffU) | 0x0100U);
                _state = State::StopPending;
            } else if (value & 0x8000U) {
                _state = State::StartPending;
            }
            return true;
        }
        if (_state != State::Converting) return false;
        if ((value & 0x7fffU) != (_config.registerValue & 0x7fffU)) {
            fail(Error::ConfigLost); return false;
        }
        if (!(value & 0x8000U)) _observedBusy = true;
        else if (_observedBusy) _state = State::SamplePending;
        // Require evidence of our start, not a potentially stale idle sample.
        // A scheduler that misses the whole busy interval loses this sample:
        // fail closed rather than claim freshness. Integrators must poll once
        // immediately after start and test worst-case scheduling at every rate.
        return true;
    }

    bool acknowledgeWrite(uint64_t generation, bool ok, uint32_t nowMs) {
        if (!accept(generation, nowMs)) return false;
        if (_state != State::StopPending && _state != State::StartPending) return false;
        if (!ok) { fail(Error::Transport); return false; }
        _state = _state == State::StopPending ? State::AwaitIdle : State::Converting;
        return true;
    }

    bool observeSample(uint64_t generation, uint16_t code, uint32_t nowMs) {
        if (!accept(generation, nowMs) || _state != State::SamplePending) return false;
        if (!decodeADS1115Sample(code, _fullScaleMv, _sample)) {
            fail(Error::InvalidConfig); return false;
        }
        _sampleTimeMs = nowMs; _state = State::Complete;
        return true;
    }

    bool transportFailed(uint64_t generation, uint32_t nowMs) {
        if (!accept(generation, nowMs)) return false;
        fail(Error::Transport); return true;
    }
    bool cancel(uint64_t generation) {
        if (!generation || generation != _generation || !pending()) return false;
        fail(Error::Cancelled); _state = State::Cancelled; return true;
    }
    bool tick(uint64_t generation, uint32_t nowMs) { return accept(generation, nowMs); }
    State state() const { return _state; }
    Error error() const { return _error; }
    const ADS1115DecodedSample& sample() const { return _sample; }
    uint32_t sampleTimeMs() const { return _sampleTimeMs; }
    uint64_t generation() const { return _generation; }
    bool pending() const {
        return _state == State::AwaitIdle || _state == State::StopPending ||
               _state == State::StartPending || _state == State::Converting ||
               _state == State::SamplePending;
    }

private:
    bool accept(uint64_t generation, uint32_t nowMs) {
        if (!generation || generation != _generation || !pending()) return false;
        if (static_cast<uint32_t>(nowMs - _startedMs) >= _timeoutMs) {
            fail(_state == State::Converting && !_observedBusy ?
                 Error::StartNotObserved : Error::Timeout);
            return false;
        }
        return true;
    }
    void fail(Error error) {
        _state = State::Failed; _error = error; _sample = {}; _sampleTimeMs = 0;
    }

    State _state = State::Idle;
    Error _error = Error::None;
    uint64_t _generation = 0;
    ADS1115ConversionConfig _config;
    ADS1115DecodedSample _sample;
    uint32_t _fullScaleMv = 0, _startedMs = 0, _timeoutMs = 0, _sampleTimeMs = 0;
    uint16_t _idleConfig = 0;
    bool _observedBusy = false;
};

} // namespace FastBee
#endif
