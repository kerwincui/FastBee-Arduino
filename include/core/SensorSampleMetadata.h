#pragma once
#include <cstdint>
#include <cmath>

namespace FastBee {
enum class SensorSampleQuality : uint8_t { Unknown, Good, Stale };
// Timestamp is boot-local monotonic milliseconds, never UTC or receipt time.
struct SensorSampleMetadata {
    uint64_t sampledAtMs = 0;
    uint64_t capturedAtMs = 0;
    uint32_t maxAgeMs = 0;
    SensorSampleQuality quality = SensorSampleQuality::Unknown;
    uint64_t sampleGroup = 0; // Nonzero only for one completed cache-owned scan; not simultaneity.
};

inline float describeSensorSample(float value, uint64_t sampledAt, uint64_t now,
                                 uint32_t maxAgeMs, bool healthy,
                                 SensorSampleMetadata* metadata) {
    if (metadata) {
        *metadata = {};
        if (std::isfinite(value) && now >= sampledAt && maxAgeMs) {
            metadata->sampledAtMs = sampledAt;
            metadata->capturedAtMs = now;
            metadata->maxAgeMs = maxAgeMs;
            metadata->quality = healthy && now - sampledAt < maxAgeMs
                ? SensorSampleQuality::Good : SensorSampleQuality::Stale;
        }
    }
    return value;
}
} // namespace FastBee
