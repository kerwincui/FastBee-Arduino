#pragma once
#include <atomic>
#include <cstdint>
#include "UartPortPolicy.h"
#include "RuntimeResourceSnapshot.h"

namespace FastBee {
// Process-wide ownership for Arduino HardwareSerial controllers, not GPIO or
// protocol transaction locking. Callers serialize their own handle operations.
// Stop hardware and dispose all clients BEFORE releaseAfterStop(). A busy
// teardown deliberately retains its generation even if the handle is destroyed.
class UartPortLease {
public:
    static constexpr uint8_t PORT_COUNT = 3;
    UartPortLease() = default;
    UartPortLease(const UartPortLease&) = delete;
    UartPortLease& operator=(const UartPortLease&) = delete;

    bool acquire(uint8_t port, ResourceConsumerRole role = ResourceConsumerRole::Unknown,
                 const char* consumerId = nullptr) {
        if (port == 0 || port >= PORT_COUNT) return false;
        ResourceConsumerIdentity consumer;
        if (!consumer.assign(role, consumerId)) return false;
        if (token_) {
            if (port_ != port || !owned()) return false;
            if (!consumer.specified()) return true;
            const auto& label = registry().consumers[port];
            if (label.role.load() != consumer.role) return false;
            for (size_t i = 0; i < ResourceConsumerIdentity::ID_CAPACITY; ++i)
                if (label.id[i].load() != static_cast<uint8_t>(consumer.id[i])) return false;
            return true;
        }
        auto& state = registry();
        uint32_t token = state.next.load(std::memory_order_relaxed);
        do {
            if (token == UINT32_MAX) return false; // Never wrap/reuse a generation.
        } while (!state.next.compare_exchange_weak(token, token + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed));
        uint32_t free = 0;
        if (!state.owners[port].compare_exchange_strong(free, token,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        auto& label = state.consumers[port];
        label.role.store(consumer.role);
        for (size_t i = 0; i < ResourceConsumerIdentity::ID_CAPACITY; ++i)
            label.id[i].store(static_cast<uint8_t>(consumer.id[i]));
        label.generation.store(token);
        port_ = port;
        token_ = token;
        return true;
    }
    bool owned() const {
        return token_ && port_ < PORT_COUNT &&
            registry().owners[port_].load(std::memory_order_acquire) == token_;
    }
    bool releaseAfterStop() {
        if (!token_ || port_ >= PORT_COUNT) return false;
        // Invalidate before publishing free. Clearing after the owner CAS
        // could erase the next owner's already-published label.
        uint32_t labelToken = token_;
        registry().consumers[port_].generation.compare_exchange_strong(labelToken, 0);
        uint32_t expected = token_;
        if (!registry().owners[port_].compare_exchange_strong(expected, 0,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        port_ = UINT8_MAX;
        token_ = 0;
        return true;
    }
    static bool available(uint8_t port) {
        return port > 0 && port < PORT_COUNT &&
            registry().owners[port].load(std::memory_order_acquire) == 0;
    }
    // Independent atomic observations; no cross-port atomicity or cleanup state.
    // A destroyed handle's unreleased generation remains visible as reserved.
    static void snapshotResources(UartResourceSnapshot& result, uint8_t chipPortCount) {
        static_assert(PORT_COUNT == UartResourceSnapshot::CAPACITY, "snapshot capacity");
        result = UartResourceSnapshot{};
        result.portCount = chipPortCount < PORT_COUNT ? chipPortCount : PORT_COUNT;
        auto& state = registry();
        for (uint8_t port = 1; port < result.portCount; ++port) {
            const uint32_t token = state.owners[port].load(std::memory_order_acquire);
            result.generations[port] = token;
            const auto& label = state.consumers[port];
            if (!token || label.generation.load() != token) continue;
            ResourceConsumerIdentity consumer;
            consumer.role = label.role.load();
            // Atomic bytes prevent a data race even when a different owner
            // starts publishing during this copy. Sequential consistency with
            // generation invalidation makes a mixed copy fail the final check
            // even on weakly ordered targets. Generations never repeat.
            for (size_t i = 0; i < ResourceConsumerIdentity::ID_CAPACITY; ++i)
                consumer.id[i] = static_cast<char>(label.id[i].load());
            if (state.owners[port].load(std::memory_order_acquire) == token &&
                label.generation.load() == token) {
                consumer.id[ResourceConsumerIdentity::ID_CAPACITY - 1] = '\0';
                result.consumers[port] = consumer;
            }
        }
    }
private:
    struct Registry {
        struct Consumer {
            std::atomic<uint32_t> generation{0};
            std::atomic<ResourceConsumerRole> role{ResourceConsumerRole::Unknown};
            std::atomic<uint8_t> id[ResourceConsumerIdentity::ID_CAPACITY]{};
        } consumers[PORT_COUNT]{};
        std::atomic<uint32_t> owners[PORT_COUNT]{};
        std::atomic<uint32_t> next{1};
    };
    static Registry& registry() { static Registry value; return value; }
    uint32_t token_ = 0;
    uint8_t port_ = UINT8_MAX;
};
}
