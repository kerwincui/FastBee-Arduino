#pragma once
#include <Arduino.h>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "ReportInstanceBinding.h"

namespace FastBee {
// Local, volatile report retention. Not a durable outbox or MQTT delivery ack.
class PendingReportQueue {
public:
    static constexpr size_t Capacity = 5;
    static constexpr size_t MaxBytes = 512;
    static constexpr uint32_t RetryMs = 5000;
    enum class Admission { Accepted, ReplacedOldest, Unavailable, Busy, Invalid, LowMemory, Full, UnknownInstance, InstanceChanged };
    enum class Retry { Idle, Busy, Accepted, Rejected, InstanceChanged };
    enum class LastRetry { None, Accepted, Rejected, InstanceChanged };
    struct Status {
        bool available = false, inFlight = false;
        size_t depth = 0;
        uint32_t oldestAgeMs = 0;
        uint32_t admitted = 0, evicted = 0, rejectedInvalid = 0, rejectedMemory = 0;
        uint32_t rejectedFull = 0, rejectedBusy = 0, contention = 0, retryAccepted = 0, retryRejected = 0;
        uint32_t oldestAttempts = 0, retryDelayMs = 0;
        uint32_t rejectedUnknownInstance = 0, discardedInstanceChanged = 0;
        LastRetry lastRetry = LastRetry::None;
    };

    PendingReportQueue() = default;
    PendingReportQueue(const PendingReportQueue&) = delete;
    PendingReportQueue& operator=(const PendingReportQueue&) = delete;
    // Owner must stop all producers/consumers before destruction. Production
    // owner is the process-lifetime PeriphExecManager singleton.
    ~PendingReportQueue() { if (_mutex) vSemaphoreDelete(_mutex); }
    bool begin() { if (!_mutex) _mutex = xSemaphoreCreateMutex(); return _mutex != nullptr; }

    Admission enqueue(const String& value, uint32_t now, bool memoryAllowed, uint32_t instanceId,
                      uint32_t currentInstanceId = 0) {
        return enqueueAt(value, [now] { return now; }, memoryAllowed, instanceId, currentInstanceId);
    }
    template<class Clock>
    Admission enqueueAt(const String& value, Clock clock, bool memoryAllowed, uint32_t instanceId,
                        uint32_t currentInstanceId = 0) {
        if (!_mutex) return Admission::Unavailable;
        Admission rejection = Admission::Accepted;
        String prepared;
        if (!instanceId) rejection = Admission::UnknownInstance;
        // currentInstanceId is observed under the caller's MQTT lease, not a
        // setting or guessed identity. Zero means unavailable, not changed.
        // Never allocate/store a known stale report or evict a valid slot for it.
        else if (currentInstanceId && currentInstanceId != instanceId) rejection = Admission::InstanceChanged;
        else if (value.isEmpty() || value.length() > MaxBytes || value.length() != std::strlen(value.c_str()))
            rejection = Admission::Invalid;
        else if (!memoryAllowed || !prepared.reserve(value.length()) || !prepared.concat(value) ||
                 prepared.length() != value.length()) rejection = Admission::LowMemory;
        // Prepare allocations outside the queue lock. Move-only publication
        // below never leaves a partially copied payload in a live slot.
        Guard guard(_mutex);
        if (!guard.locked) { addContention(); incrementAtomic(_rejectedBusy); return Admission::Busy; }
        reconcile();
        if (rejection == Admission::UnknownInstance) { increment(_status.rejectedUnknownInstance); return rejection; }
        if (rejection == Admission::InstanceChanged) {
            increment(_status.discardedInstanceChanged);
            return rejection; // No retry happened; preserve lastRetry/cooldown/head.
        }
        if (rejection == Admission::Invalid) { increment(_status.rejectedInvalid); return rejection; }
        if (rejection == Admission::LowMemory) { increment(_status.rejectedMemory); return rejection; }
        Admission result = Admission::Accepted;
        if (_count == Capacity) {
            // An unlocked callback borrows the immutable head. Never replace
            // that slot, and never mutate its String while the callback reads.
            if (_inFlight) { increment(_status.rejectedFull); return Admission::Full; }
            pop(); increment(_status.evicted); result = Admission::ReplacedOldest;
        }
        Slot& slot = _slots[(_head + _count) % Capacity];
        slot.payload = std::move(prepared); slot.queuedAt = clock(); slot.attempts = 0;
        slot.instanceId = instanceId;
        ++_count; increment(_status.admitted);
        return result;
    }

    template<class Send>
    Retry retryOne(uint32_t now, bool connected, Send send) {
        return retryOneAt([now] { return now; }, connected, send);
    }
    template<class Clock, class Send>
    Retry retryOneAt(Clock clock, bool connected, Send send) {
        if (!_mutex || !connected) return Retry::Idle;
        const String* payload = nullptr;
        uint32_t instanceId = 0;
        {
            Guard guard(_mutex);
            if (!guard.locked) { addContention(); return Retry::Busy; }
            reconcile();
            if (_inFlight) return Retry::Busy;
            const uint32_t now = clock();
            if (!_count || (_attempted && static_cast<uint32_t>(now - _lastAttempt) < RetryMs))
                return Retry::Idle;
            _attempted = true; _lastAttempt = now; _inFlight = true;
            increment(_slots[_head].attempts);
            payload = &_slots[_head].payload;
            instanceId = _slots[_head].instanceId;
        }
        // One callback per five seconds; no queue lock across protocol code or
        // logs. Reentry cannot claim the head a second time. Completion uses a
        // release/acquire handoff, so even a busy queue never loses an ack or
        // replays locally accepted bytes merely because final lock admission failed.
        struct Completion {
            std::atomic<uint8_t>& state;
            ReportReplayResult result = ReportReplayResult::Rejected;
            ~Completion() { state.store(static_cast<uint8_t>(result), std::memory_order_release); }
        } completion{_completion};
        completion.result = send(*payload, instanceId);
        return completion.result == ReportReplayResult::Accepted ? Retry::Accepted :
            completion.result == ReportReplayResult::InstanceChanged ? Retry::InstanceChanged : Retry::Rejected;
    }

    Status snapshot(uint32_t now) {
        return snapshotAt([now] { return now; });
    }
    template<class Clock>
    Status snapshotAt(Clock clock) {
        Status result;
        result.contention = _contention.load(std::memory_order_relaxed);
        Guard guard(_mutex);
        if (!guard.locked) return result; // unavailable is never a healthy empty queue
        reconcile();
        const uint32_t now = clock();
        result = _status; result.available = true;
        result.depth = _count; result.inFlight = _inFlight;
        result.oldestAgeMs = _count ? static_cast<uint32_t>(now - _slots[_head].queuedAt) : 0;
        result.oldestAttempts = _count ? _slots[_head].attempts : 0;
        const uint32_t sinceAttempt = static_cast<uint32_t>(now - _lastAttempt);
        result.retryDelayMs = _count && !_inFlight && _attempted && sinceAttempt < RetryMs
            ? RetryMs - sinceAttempt : 0;
        result.contention = _contention.load(std::memory_order_relaxed);
        result.rejectedBusy = _rejectedBusy.load(std::memory_order_relaxed);
        return result;
    }

private:
    struct Guard {
        SemaphoreHandle_t handle;
        bool locked;
        explicit Guard(SemaphoreHandle_t h) : handle(h), locked(h && xSemaphoreTake(h, 0) == pdTRUE) {}
        ~Guard() { if (locked) xSemaphoreGive(handle); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
    struct Slot { String payload; uint32_t queuedAt = 0, attempts = 0, instanceId = 0; };
    static void increment(uint32_t& value) { if (value != UINT32_MAX) ++value; }
    static void incrementAtomic(std::atomic<uint32_t>& counter) {
        uint32_t value = counter.load(std::memory_order_relaxed);
        while (value != UINT32_MAX && !counter.compare_exchange_weak(
            value, value + 1, std::memory_order_relaxed)) {}
    }
    void addContention() { incrementAtomic(_contention); }
    void pop() {
        _slots[_head].payload = String(); // release payload allocation when unused
        _head = (_head + 1) % Capacity; --_count;
    }
    void reconcile() {
        const uint8_t completed = _completion.exchange(0, std::memory_order_acquire);
        if (!completed) return;
        if (completed == 1) {
            pop(); increment(_status.retryAccepted); _status.lastRetry = LastRetry::Accepted;
        } else if (completed == 3) {
            // Retire exactly the pinned head. New producers may already have
            // admitted reports for the replacement instance behind it.
            pop(); increment(_status.discardedInstanceChanged); _status.lastRetry = LastRetry::InstanceChanged;
        } else {
            increment(_status.retryRejected); _status.lastRetry = LastRetry::Rejected;
        } // Leave the failed head in place, with its own attempt count.
        _inFlight = false;
    }
    SemaphoreHandle_t _mutex = nullptr;
    std::array<Slot, Capacity> _slots;
    size_t _head = 0, _count = 0;
    bool _inFlight = false, _attempted = false;
    uint32_t _lastAttempt = 0;
    Status _status;
    std::atomic<uint8_t> _completion{0};
    std::atomic<uint32_t> _contention{0};
    std::atomic<uint32_t> _rejectedBusy{0};
};
} // namespace FastBee
