#pragma once
#include "core/FeatureObservationCounter.h"

namespace FastBee {

// Dynamic managers can be replaced while a status request is in flight. Only
// immutable identities and enums enter these static slots; never an owner this
// pointer, SDK pointer, configuration, or callback into a destructible manager.
template<uint32_t IdentityLimit = 0x00ffffffu, uint32_t RevisionLimit = UINT32_MAX, uint8_t SlotLimit = 4>
class BasicFeatureOwnerGroup {
public:
    static constexpr uint8_t Capacity = SlotLimit;
    static constexpr uint32_t LastIdentity = IdentityLimit;
    static_assert(IdentityLimit > 0 && IdentityLimit <= 0x00ffffffu, "Identity must fit slot");
    static_assert(RevisionLimit > 0, "Revision must have an exhausted state");
    static_assert(SlotLimit > 0, "At least one owner slot required");
    void invalidate() { unavailable_.store(true, std::memory_order_release); }
    bool acquire(uint8_t& slot, uint32_t& identity) {
        Mutation mutation(*this);
        auto current = sequence_.load(std::memory_order_acquire);
        while (current < LastIdentity && !sequence_.compare_exchange_weak(current, current + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
        if (current == LastIdentity) { unavailable_ = true; return false; }
        identity = current + 1;
        for (uint8_t i = 0; i < Capacity; ++i) {
            uint32_t empty = 0;
            if (slots_[i].compare_exchange_strong(empty, encode(identity, FeatureInstanceState::Busy),
                    std::memory_order_acq_rel, std::memory_order_acquire)) { slot = i; return true; }
        }
        unavailable_ = true; // Never hide an owner after bounded capacity exhaustion.
        return false;
    }
    bool publish(uint8_t slot, uint32_t identity, FeatureInstanceState state) {
        if (slot >= Capacity || !identity || identity > LastIdentity ||
            (state != FeatureInstanceState::Busy && state != FeatureInstanceState::Allocated &&
             state != FeatureInstanceState::Retiring)) return false;
        Mutation mutation(*this);
        auto current = slots_[slot].load(std::memory_order_acquire);
        while ((current >> 8) == identity) {
            if (slots_[slot].compare_exchange_weak(current, encode(identity, state),
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }
    bool release(uint8_t slot, uint32_t identity) {
        if (slot >= Capacity || !identity || identity > LastIdentity) return false;
        Mutation mutation(*this);
        auto current = slots_[slot].load(std::memory_order_acquire);
        while ((current >> 8) == identity) {
            if (slots_[slot].compare_exchange_weak(current, 0,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }
    FeatureInstanceSnapshot snapshot() const {
        constexpr auto scope = FeatureInstanceScope::OwnerLifetime;
        const auto before = revision_.load(std::memory_order_acquire);
        if (unavailable_.load(std::memory_order_acquire) || before == RevisionLimit)
            return FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::InvalidSnapshot, scope);
        if (writers_.snapshot(FeatureInstanceScope::ExecutionActivity).state != FeatureInstanceState::Idle)
            return {FeatureInstanceState::Busy, 0, scope};
        auto state = FeatureInstanceState::OwnerAbsent;
        for (const auto& slot : slots_) {
            const auto value = slot.load(std::memory_order_acquire);
            if (!value) continue;
            const auto current = static_cast<FeatureInstanceState>(value & 255);
            if (current == FeatureInstanceState::Busy) { state = current; break; }
            if (current == FeatureInstanceState::Retiring || state == FeatureInstanceState::OwnerAbsent) state = current;
        }
        if (before != revision_.load(std::memory_order_acquire) ||
            writers_.snapshot(FeatureInstanceScope::ExecutionActivity).state != FeatureInstanceState::Idle)
            return {FeatureInstanceState::Busy, 0, scope};
        return {state, 0, scope};
    }
private:
    static uint32_t encode(uint32_t identity, FeatureInstanceState state) {
        return (identity << 8) | static_cast<uint8_t>(state);
    }
    void advanceRevision() {
        auto current = revision_.load(std::memory_order_acquire);
        while (current != RevisionLimit && !revision_.compare_exchange_weak(current, current + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }
    class Mutation {
    public:
        explicit Mutation(BasicFeatureOwnerGroup& owner) : owner_(owner) { owner_.writers_.enter(); }
        ~Mutation() { owner_.advanceRevision(); owner_.writers_.leave(); }
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;
    private:
        BasicFeatureOwnerGroup& owner_;
    };
    std::atomic<uint32_t> slots_[Capacity]{};
    std::atomic<uint32_t> sequence_{0}, revision_{0};
    std::atomic<bool> unavailable_{false};
    FeatureObservationCounter<> writers_;
};
using FeatureOwnerGroup = BasicFeatureOwnerGroup<>;

enum class ObservedFeatureOwner : uint8_t { WebServer, LogViewer, Modbus, Ble, Cellular, Ota };

inline FeatureOwnerGroup& featureOwnerGroup(ObservedFeatureOwner owner) {
    static FeatureOwnerGroup groups[6];
    static const char* const keys[] = {"webServer", "logViewer", "modbus", "ble", "cellular", "ota"};
    static const FeatureInstanceObserver observers[] = {
        {keys[0], [](const void* p) { return static_cast<const FeatureOwnerGroup*>(p)->snapshot(); }, &groups[0]},
        {keys[1], [](const void* p) { return static_cast<const FeatureOwnerGroup*>(p)->snapshot(); }, &groups[1]},
        {keys[2], [](const void* p) { return static_cast<const FeatureOwnerGroup*>(p)->snapshot(); }, &groups[2]},
        {keys[3], [](const void* p) { return static_cast<const FeatureOwnerGroup*>(p)->snapshot(); }, &groups[3]},
        {keys[4], [](const void* p) { return static_cast<const FeatureOwnerGroup*>(p)->snapshot(); }, &groups[4]},
        {keys[5], [](const void* p) { return static_cast<const FeatureOwnerGroup*>(p)->snapshot(); }, &groups[5]}
    };
    const auto index = static_cast<uint8_t>(owner);
    FeatureInstanceRegistry::instance().registerObserver(&observers[index]);
    return groups[index];
}

class FeatureOwnerLifetime {
public:
    explicit FeatureOwnerLifetime(ObservedFeatureOwner owner) : group_(featureOwnerGroup(owner)) {
        tracked_ = group_.acquire(slot_, identity_);
    }
    ~FeatureOwnerLifetime() { if (tracked_) group_.release(slot_, identity_); }
    void ready() { if (tracked_) group_.publish(slot_, identity_, FeatureInstanceState::Allocated); }
    void retiring() { if (tracked_) group_.publish(slot_, identity_, FeatureInstanceState::Retiring); }
    FeatureOwnerLifetime(const FeatureOwnerLifetime&) = delete;
    FeatureOwnerLifetime& operator=(const FeatureOwnerLifetime&) = delete;
private:
    FeatureOwnerGroup& group_;
    uint32_t identity_ = 0;
    uint8_t slot_ = 0;
    bool tracked_ = false;
};

} // namespace FastBee
