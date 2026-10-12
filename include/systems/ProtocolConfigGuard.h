#pragma once
#include <atomic>

namespace FastBee {
// Serialize protocol.json loads (which may repair .bak) and mutations through
// ConfigStorage. Raw filesystem access is outside this contract.
class ProtocolConfigGuard {
    static std::atomic_flag& gate() {
        static std::atomic_flag value = ATOMIC_FLAG_INIT;
        return value;
    }
    bool _required;
    bool _held;
public:
    explicit ProtocolConfigGuard(bool required = true)
        : _required(required),
          _held(required && !gate().test_and_set(std::memory_order_acquire)) {}
    ~ProtocolConfigGuard() {
        if (_held) gate().clear(std::memory_order_release);
    }
    ProtocolConfigGuard(const ProtocolConfigGuard&) = delete;
    ProtocolConfigGuard& operator=(const ProtocolConfigGuard&) = delete;
    explicit operator bool() const { return !_required || _held; }
};
} // namespace FastBee
