#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FastBee {
// All application TZ writers share this zero-wait guard. RTC copying holds it
// from calendar conversion through readback. Never hold it while waiting for
// network synchronization, and never acquire PeripheralManager while held.
class TimeZoneGuard {
public:
    TimeZoneGuard() : held_(!busy().test_and_set(std::memory_order_acquire)) {}
    ~TimeZoneGuard() { if (held_) busy().clear(std::memory_order_release); }
    bool held() const { return held_; }
    TimeZoneGuard(const TimeZoneGuard&) = delete;
    TimeZoneGuard& operator=(const TimeZoneGuard&) = delete;
private:
    static std::atomic_flag& busy() {
        static std::atomic_flag flag = ATOMIC_FLAG_INIT;
        return flag;
    }
    const bool held_;
};

class SystemTimeZone {
public:
    static constexpr size_t CAPACITY = 128;
    struct LocalSnapshot {
        bool timezoneAvailable = false, calendarAvailable = false;
        char timezone[CAPACITY]{};
        char calendar[32]{};
    };
    // Captures label and calendar under one guard using exactly the supplied
    // epoch. Negative epoch means no valid time; never substitute a new now().
    static LocalSnapshot localSnapshot(int64_t epochMs);
    static bool setPosix(const char* timezone);
    // nullptr preserves the active TZ, copied under the guard, never a borrowed
    // getenv pointer passed to an SDK routine which can replace that pointer.
    // True means configuration accepted, not that SNTP has synchronized.
    static bool startSntp(const char* timezone, const char* server1,
                          const char* server2 = nullptr, const char* server3 = nullptr);
    // Failure clears output; a busy snapshot must not masquerade as UTC.
    static bool copyActive(char* output, size_t capacity);
};
} // namespace FastBee
