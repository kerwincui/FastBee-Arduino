#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FastBee {
enum class IpcOwnerStatus : uint8_t {
    Unsupported, NotRequested, Pending, TimedOut, Busy, SdkUnavailable, ClockUnavailable,
    InvalidIdentity, PublishedAfterCapture, Partial, Observed
};
struct IpcTaskOwnerEvidence {
    IpcOwnerStatus status = IpcOwnerStatus::Unsupported;
    uint32_t taskNumber = 0;
    uint32_t generation = 0;
    bool publishedLate = false;
    uint64_t requestedAtUptimeMs = 0;
    uint64_t publishedAtUptimeMs = 0;
};
struct IpcTaskOwnerAttribution {
    IpcOwnerStatus status = IpcOwnerStatus::Unsupported;
    std::array<IpcTaskOwnerEvidence, 2> cores{};
    uint32_t fixedRamBytes = 0;
    bool callerMayChangeSdkPriority = false;
};

// Accepted contexts are permanent. Ready fields become immutable; a rejected
// SDK call did not retain its context and may be retried by a later explicit POST.
class IpcTaskOwnerSlot {
public:
    static constexpr uint32_t TIMEOUT_MS = 1000;
    bool begin(int64_t nowUs) {
        if (nowUs < 0) return false;
        uint32_t expected = state_.load(std::memory_order_acquire);
        if (expected != Empty && expected != Rejected && expected != SdkRejected) return false;
        if (!state_.compare_exchange_strong(expected, Reserved, std::memory_order_acq_rel)) return false;
        requestedUs_ = nowUs;
        requestedMs_.store(static_cast<uint32_t>(nowUs / 1000), std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_relaxed);
        state_.store(Pending, std::memory_order_release);
        return true;
    }
    void reject(bool busy = true) {
        uint32_t expected = Pending;
        state_.compare_exchange_strong(expected, busy ? Rejected : SdkRejected, std::memory_order_acq_rel);
    }
    void publish(uint32_t taskNumber, bool correctCore, int64_t nowUs) {
        uint32_t expected = Pending;
        if (!state_.compare_exchange_strong(expected, Publishing, std::memory_order_acq_rel)) return;
        number_ = taskNumber;
        valid_ = correctCore && taskNumber != 0;
        publishedUs_ = nowUs;
        state_.store(Ready, std::memory_order_release);
    }
    IpcTaskOwnerEvidence read(int64_t captureUs) const {
        IpcTaskOwnerEvidence out;
        const auto state = state_.load(std::memory_order_acquire);
        out.generation = generation_.load(std::memory_order_relaxed);
        if (captureUs < 0) { out.status = IpcOwnerStatus::ClockUnavailable; return out; }
        if (state == Empty) { out.status = IpcOwnerStatus::NotRequested; return out; }
        if (state == Rejected) { out.status = IpcOwnerStatus::Busy; return out; }
        if (state == SdkRejected) { out.status = IpcOwnerStatus::SdkUnavailable; return out; }
        if (state != Ready) {
            const uint32_t elapsed = static_cast<uint32_t>(captureUs / 1000) - requestedMs_.load(std::memory_order_relaxed);
            if (state != Reserved && elapsed > UINT32_MAX / 2) {
                out.status = IpcOwnerStatus::ClockUnavailable;
                return out;
            }
            out.status = state != Reserved && elapsed >= TIMEOUT_MS
                ? IpcOwnerStatus::TimedOut : IpcOwnerStatus::Pending;
            return out;
        }
        // No future begin() can modify an accepted context after Ready.
        if (requestedUs_ < 0 || publishedUs_ < requestedUs_) {
            out.status = IpcOwnerStatus::ClockUnavailable;
            return out;
        }
        out.requestedAtUptimeMs = static_cast<uint64_t>(requestedUs_) / 1000;
        out.publishedAtUptimeMs = static_cast<uint64_t>(publishedUs_) / 1000;
        out.publishedLate = (publishedUs_ - requestedUs_) / 1000 >= TIMEOUT_MS;
        if (publishedUs_ > captureUs) { out.status = IpcOwnerStatus::PublishedAfterCapture; return out; }
        if (!valid_) { out.status = IpcOwnerStatus::InvalidIdentity; return out; }
        out.taskNumber = number_;
        out.status = IpcOwnerStatus::Observed;
        return out;
    }
private:
    enum : uint32_t { Empty, Reserved, Pending, Publishing, Rejected, SdkRejected, Ready };
    std::atomic<uint32_t> state_{Empty}, requestedMs_{0}, generation_{0};
    int64_t requestedUs_ = 0, publishedUs_ = 0;
    uint32_t number_ = 0;
    bool valid_ = false;
};

IpcTaskOwnerAttribution sampleIpcTaskOwners(int64_t captureUs, bool explicitRequest);
const char* ipcOwnerReason(IpcOwnerStatus status);
} // namespace FastBee
