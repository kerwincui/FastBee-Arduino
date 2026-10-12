#pragma once
#include <atomic>
#include <cstdint>

namespace FastBee {
// Tracks object access, not RTOS stack reclamation. Reserve before task creation;
// finish must be the worker's LAST access to the owning MQTTClient. The owner
// must outlive this gate and must never synchronously destroy itself in a worker
// callback. All public client references still require the manager lifetime lease.
class MqttReconnectLifetime {
public:
    bool reserve() {
        uint8_t expected = 0;
        return _state.compare_exchange_strong(expected, Active, std::memory_order_acq_rel);
    }
    bool requestStop() {
        return !(_state.fetch_or(Stopping, std::memory_order_acq_rel) & Active);
    }
    bool active() const { return (_state.load(std::memory_order_acquire) & Active) != 0; }
    bool stopping() const { return (_state.load(std::memory_order_acquire) & Stopping) != 0; }
    void finish() { _state.fetch_and(static_cast<uint8_t>(~Active), std::memory_order_release); }
    // Explicit reconnect only, after old work has acknowledged its final access.
    bool reopen() {
        uint8_t expected = Stopping;
        return _state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel) || expected == 0;
    }
private:
    static constexpr uint8_t Active = 1, Stopping = 2;
    std::atomic<uint8_t> _state{0};
};
} // namespace FastBee
