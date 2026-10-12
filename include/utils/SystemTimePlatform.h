#pragma once
#include <cstdint>
#include <cstddef>

namespace FastBee {
// Small platform boundary: Host tests supply fakes, never change the host clock.
class SystemTimePlatform {
public:
    using Observer = void (*)(int64_t epochMs);
    // esp_timer_get_time is signed microseconds. Negative/unavailable samples
    // use UINT64_MAX; zero is a valid instant at boot.
    static constexpr uint64_t MAX_MONOTONIC_MS = static_cast<uint64_t>(INT64_MAX) / 1000;
    static bool monotonicAvailable(uint64_t value) { return value <= MAX_MONOTONIC_MS; }
    static uint64_t monotonicFromMicroseconds(int64_t value) {
        return value >= 0 ? static_cast<uint64_t>(value) / 1000 : UINT64_MAX;
    }
    static uint64_t monotonicMs();
    static int64_t wallMs();
    static bool writeMs(int64_t epochMs);
    static void installObserver(Observer observer);
    // TZ calls are only made under TimeZoneGuard by SystemTimeZone.
    static const char* readTimezone();
    static bool writeTimezone(const char* timezone);
    static void configureSntp(const char* timezone, const char* server1,
                              const char* server2, const char* server3);
    static bool formatLocal(int64_t epochMs, char* output, size_t capacity);
};
} // namespace FastBee
