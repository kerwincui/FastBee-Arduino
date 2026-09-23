#include "peripherals/IoExpander.h"

namespace FastBee {

bool Pcf8574::begin(uint8_t mask, uint8_t startupHigh, uint16_t intervalMs,
                    uint32_t now, uint8_t inputMask) {
    if (mask & inputMask) return false;
    _outputMask = mask;
    _inputMask = inputMask;
    _startup = startupHigh | static_cast<uint8_t>(~mask);
    _interval = intervalMs >= 20 && intervalMs <= 1000 ? intervalMs : 20;
    _started = true;
    _valid = _outputsKnown = false;
    _recover = true;
    _retryDelay = 0;
    _failures = 0;
    _lastAttempt = now - _interval;
    return poll(now);
}

bool Pcf8574::configure(uint8_t mask, uint8_t startupHigh, uint32_t now, uint8_t inputMask) {
    if (mask & inputMask) return false;
    const uint8_t nextStartup = startupHigh | static_cast<uint8_t>(~mask);
    const uint8_t retired = _outputMask & ~mask;
    const uint8_t added = mask & ~_outputMask;
    // A retired output returns to its own configured startup value. Subsequent
    // sibling writes retain it until reassignment (input explicitly releases HIGH).
    const uint8_t next = ((_shadow & ~retired & ~added) |
                          (_startup & retired) | (nextStartup & added)) | inputMask;
    if (!_started || !_outputsKnown || _recover) {
        // Keep the desired configuration for the eventual physical recovery.
        _outputMask = mask;
        _inputMask = inputMask;
        _startup = nextStartup;
        _outputsKnown = false;
        return false;
    }
    IoExpanderResult result = IoExpanderResult::Ok;
    if (next != _shadow) result = _transport.writeByte(next);
    // No transaction ran: retain the old configuration so a later retry still
    // recognizes added/retired bits and applies their startup values.
    if (result == IoExpanderResult::Busy) return false;
    // A configuration edit must not reapply startup values to sibling outputs.
    _outputMask = mask;
    _inputMask = inputMask;
    _startup = nextStartup;
    if (result == IoExpanderResult::Error) return fail(true, now);
    if (next == _shadow) return true;
    _shadow = next;
    _lastAttempt = now - _interval;
    return true;
}

bool Pcf8574::fail(bool uncertainWrite, uint32_t now) {
    _valid = false;
    _lastAttempt = now;
    if (_failures < 255) ++_failures;
    // An unacknowledged full-port write may have reached the chip: no output
    // channel remains trustworthy, even if only one logical channel changed.
    if (uncertainWrite || _failures >= 3) {
        _outputsKnown = false;
        if (!_recover) ++_generation;
        _recover = true;
    }
    if (_recover) {
        // Retry real outages slowly. Contention never reaches this path.
        _retryDelay = _retryDelay == 0 ? 250 :
            _retryDelay < 2000 ? static_cast<uint16_t>(_retryDelay * 2) : 2000;
    }
    return false;
}

bool Pcf8574::poll(uint32_t now) {
    if (!_started) return false;
    const uint16_t wait = _recover && _retryDelay > _interval ? _retryDelay : _interval;
    if (static_cast<uint32_t>(now - _lastAttempt) < wait) return healthy(now);
    if (_valid && static_cast<uint32_t>(now - _sampledAt) > 3UL * _interval) {
        // Scheduler or shared-bus latency makes the sample stale, but does not
        // prove that the output latch was lost. Invalidate input state so event
        // consumers establish a fresh baseline without replaying startup levels.
        _valid = false;
    }
    if (_recover && !_outputsKnown) {
        const auto result = _transport.writeByte(_startup);
        if (result == IoExpanderResult::Busy) return false;
        if (result == IoExpanderResult::Error) return fail(true, now);
        _shadow = _startup;
        _outputsKnown = true;
    }
    uint8_t sample;
    const auto result = _transport.readByte(sample);
    if (result == IoExpanderResult::Busy) return healthy(now);
    if (result == IoExpanderResult::Error) return fail(false, now);
    if (!_valid) ++_generation; // consumers establish a fresh input baseline
    _sample = sample;
    _sampledAt = now;
    _lastAttempt = now;
    _failures = 0;
    _retryDelay = 0;
    _valid = true;
    _recover = false;
    return true;
}

bool Pcf8574::healthy(uint32_t now) const {
    return _started && _valid && _outputsKnown && !_recover &&
           static_cast<uint32_t>(now - _sampledAt) <= 3UL * _interval;
}

bool Pcf8574::readChannel(uint8_t channel, bool output, uint32_t now, bool& high) const {
    if (channel >= 8 || !healthy(now)) return false;
    const uint8_t bit = static_cast<uint8_t>(1U << channel);
    if (output != ((_outputMask & bit) != 0)) return false;
    high = ((output ? _shadow : _sample) & bit) != 0;
    return true;
}

bool Pcf8574::writeChannel(uint8_t channel, bool high, uint32_t now) {
    if (channel >= 8 || !healthy(now)) return false;
    const uint8_t bit = static_cast<uint8_t>(1U << channel);
    if (!(_outputMask & bit)) return false;
    const uint8_t next = (high ? (_shadow | bit) : (_shadow & ~bit)) | _inputMask;
    const auto result = _transport.writeByte(next);
    if (result == IoExpanderResult::Busy) return false;
    if (result == IoExpanderResult::Error) return fail(true, now);
    _shadow = next;
    return true;
}

IoExpanderResult Pcf8574::shutdown(uint8_t safeByte) {
    // Shutdown is allowed even after an outage invalidated the command shadow.
    // Exactly one full-port attempt; a successful ACK confirms the safe byte.
    const IoExpanderResult result = _transport.writeByte(safeByte);
    if (result == IoExpanderResult::Ok) _shadow = safeByte;
    end();
    return result;
}

void Pcf8574::end() {
    _started = _valid = _outputsKnown = false;
    _recover = false;
    _retryDelay = 0;
    ++_generation;
}

} // namespace FastBee
