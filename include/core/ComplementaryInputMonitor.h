#pragma once
#include <stdint.h>

namespace FastBee {
// Both equal levels, or an unreadable input, are faults. Confirm the complete
// pair so a brief overlap during NO/NC switching cannot create an alarm.
class ComplementaryInputMonitor {
public:
    bool sample(int first, int second, uint32_t now, uint16_t debounceMs) {
        const uint8_t value = first < 0 || second < 0 ? 4 : (first * 2 + second);
        if (!_sampled || value != _candidate) {
            _sampled = true;
            _candidate = value;
            _since = now;
            return false;
        }
        if (uint32_t(now - _since) < debounceMs) return false;
        const bool fault = value == 0 || value == 3 || value == 4;
        if (fault == _fault) return false;
        _fault = fault;
        _pending = fault;
        _attempted = false;
        return fault;
    }

    bool fault() const { return _fault; }
    bool publishDue(uint32_t now) const {
        return _pending && (!_attempted || uint32_t(now - _lastAttempt) >= 5000);
    }
    void published(uint32_t now, bool success) {
        _attempted = true;
        _lastAttempt = now;
        if (success) _pending = false;
    }

private:
    bool _sampled = false, _fault = false, _pending = false, _attempted = false;
    uint8_t _candidate = 0;
    uint32_t _since = 0, _lastAttempt = 0;
};
}
