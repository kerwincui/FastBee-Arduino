#pragma once
#include <atomic>
#include <cstdint>
#include <utility>
#include "core/InternalAtomicWord.h"

class MQTTClient;
namespace FastBee {
// Result of this admission attempt, not proof of broker connection/delivery.
enum class MqttRestartResult { NotStarted, Deferred, Ready, StorageUnavailable };
// Protects instance lifetime only. It does not serialize MQTT methods/config,
// authorize replay to a different destination, or own ProtocolManager itself.
class MqttClientLifetime {
    static constexpr uint32_t Writer = UINT32_C(0x80000000);
    InternalAtomicWord _state{0};
public:
    MqttClientLifetime() = default;
    MqttClientLifetime(const MqttClientLifetime&) = delete;
    MqttClientLifetime& operator=(const MqttClientLifetime&) = delete;
    MqttClientLifetime(MqttClientLifetime&&) = delete;
    MqttClientLifetime& operator=(MqttClientLifetime&&) = delete;
    // As with the original inline atomic, owner must outlive ALL Read/Write
    // leases. Drain them before destruction, which releases internal storage.
    ~MqttClientLifetime() = default;
    // Immutable construction result. False requires owner recreation/restart;
    // it is not temporary reader/writer contention and does not auto-recover.
    bool available() const { return _state.get() != nullptr; }
    class Read {
        MqttClientLifetime* _gate = nullptr;
    public:
        Read() = default;
        explicit Read(MqttClientLifetime& gate) {
            if (!gate.available()) return;
            auto& counter = *gate._state.get();
            uint32_t state = counter.load(std::memory_order_acquire);
            // Bounded admission; contention is a deferred operation, not a wait.
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                if (state >= Writer - 1) return;
                if (counter.compare_exchange_weak(state, state + 1,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    _gate = &gate; return;
                }
            }
        }
        ~Read() { release(); }
        Read(Read&& other) noexcept : _gate(other._gate) { other._gate = nullptr; }
        Read& operator=(Read&& other) noexcept {
            if (this != &other) { release(); _gate = other._gate; other._gate = nullptr; }
            return *this;
        }
        Read(const Read&) = delete;
        Read& operator=(const Read&) = delete;
        explicit operator bool() const { return _gate != nullptr; }
        void release() {
            if (_gate) { _gate->_state.get()->fetch_sub(1, std::memory_order_release); _gate = nullptr; }
        }
    };
    class Write {
        MqttClientLifetime* _gate = nullptr;
        bool _owned = false;
    public:
        Write() = default;
        explicit Write(MqttClientLifetime& gate) : _gate(&gate) { tryAcquire(); }
        ~Write() { release(); }
        Write(Write&& other) noexcept : _gate(other._gate), _owned(other._owned) {
            other._gate = nullptr; other._owned = false;
        }
        Write& operator=(Write&& other) noexcept {
            if (this != &other) {
                release(); _gate = other._gate; _owned = other._owned;
                other._gate = nullptr; other._owned = false;
            }
            return *this;
        }
        Write(const Write&) = delete;
        Write& operator=(const Write&) = delete;
        bool tryAcquire() {
            if (_owned) return true;
            if (!_gate || !_gate->available()) return false;
            uint32_t expected = 0;
            _owned = _gate->_state.get()->compare_exchange_strong(expected, Writer,
                std::memory_order_acquire, std::memory_order_relaxed);
            return _owned;
        }
        explicit operator bool() const { return _owned; }
        bool storageUnavailable() const { return _gate && !_gate->available(); }
        void release() {
            if (_owned) { _gate->_state.get()->store(0, std::memory_order_release); _owned = false; }
        }
    };
};

class MqttClientLease {
    MqttClientLifetime::Read _read;
    MQTTClient* _client = nullptr;
public:
    MqttClientLease() = default;
    // Caller must acquire Read BEFORE evaluating the client's raw pointer.
    MqttClientLease(MqttClientLifetime::Read&& read, MQTTClient* client)
        : _read(std::move(read)), _client(_read ? client : nullptr) {}
    MqttClientLease(MqttClientLease&& other) noexcept
        : _read(std::move(other._read)), _client(other._client) { other._client = nullptr; }
    MqttClientLease& operator=(MqttClientLease&& other) noexcept {
        if (this != &other) {
            _read = std::move(other._read); _client = other._client; other._client = nullptr;
        }
        return *this;
    }
    MqttClientLease(const MqttClientLease&) = delete;
    MqttClientLease& operator=(const MqttClientLease&) = delete;
    MQTTClient* get() const { return _client; }
    MQTTClient* operator->() const { return _client; }
    explicit operator bool() const { return _client != nullptr; }
};
} // namespace FastBee
