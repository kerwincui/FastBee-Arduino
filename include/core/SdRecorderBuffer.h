#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace FastBee {
struct SdRecordedSample {
    double value = 0;
    uint64_t sampledAtMs = 0, recordedAtMs = 0;
    int64_t wallEpochMs = 0;
    uint32_t identity = 0, timeSequence = 0;
    const char* timeSource = "unknown";
    const char* timeQuality = "unknown";
    bool wallKnown = false, timeAuthenticated = false;
};

// Caller serializes access. No heap, disk I/O or clock reads in this queue.
class SdRecorderBuffer {
public:
    static constexpr size_t CAPACITY = 8;
    void arm(uint64_t now) { discard(); armedAfter_ = now; fault_ = nullptr; }
    bool enqueue(const SdRecordedSample& sample) {
        if (fault_ || !std::isfinite(sample.value) || !sample.identity ||
            sample.sampledAtMs <= armedAfter_ || sample.recordedAtMs < sample.sampledAtMs ||
            (seen_ && (sample.sampledAtMs < lastSampleAt_ ||
              (sample.sampledAtMs == lastSampleAt_ && sample.identity == lastIdentity_)))) return false;
        // Dropped samples are observed too; later cache polls cannot replay them.
        seen_ = true; lastSampleAt_ = sample.sampledAtMs; lastIdentity_ = sample.identity;
        if (count_ == CAPACITY) { add(dropped, 1); return false; }
        samples_[(head_ + count_) % CAPACITY] = sample;
        ++count_; add(accepted, 1); return true;
    }
    void fail(const char* reason) { if (!fault_) fault_ = reason; discard(); }
    void discard() { add(dropped, count_); head_ = count_ = 0; }
    void acknowledge(size_t count) {
        if (count > count_) return;
        head_ = (head_ + count) % CAPACITY; count_ -= count; add(written, count);
    }
    const SdRecordedSample& at(size_t index) const { return samples_[(head_ + index) % CAPACITY]; }
    size_t size() const { return count_; }
    const char* fault() const { return fault_; }
    uint32_t accepted = 0, dropped = 0, written = 0;
private:
    static void add(uint32_t& target, size_t count) {
        target = count > UINT32_MAX - target ? UINT32_MAX : target + static_cast<uint32_t>(count);
    }
    std::array<SdRecordedSample, CAPACITY> samples_{};
    size_t head_ = 0, count_ = 0;
    uint64_t armedAfter_ = 0, lastSampleAt_ = 0;
    uint32_t lastIdentity_ = 0;
    bool seen_ = false;
    const char* fault_ = nullptr;
};
} // namespace FastBee
