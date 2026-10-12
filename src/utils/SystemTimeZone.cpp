#include "utils/SystemTimeZone.h"
#include "utils/SystemTimePlatform.h"
#include <cstring>

namespace FastBee {
namespace {
bool copyZone(const char* input, char* output, size_t capacity) {
    if (!input || !output || capacity == 0) return false;
    size_t length = 0;
    while (length < capacity && input[length]) ++length;
    if (length == 0 || length >= capacity) return false;
    std::memcpy(output, input, length + 1);
    return true;
}
const char* activeZone() {
    const char* value = SystemTimePlatform::readTimezone();
    return value ? value : "UTC0";
}
bool applyZone(const char* zone) {
    if (!SystemTimePlatform::writeTimezone(zone)) return false;
    return std::strcmp(activeZone(), zone) == 0;
}
} // namespace

bool SystemTimeZone::setPosix(const char* timezone) {
    TimeZoneGuard guard;
    if (!guard.held()) return false;
    char stable[CAPACITY]{};
    return copyZone(timezone, stable, sizeof(stable)) && applyZone(stable);
}

bool SystemTimeZone::startSntp(const char* timezone, const char* server1,
                               const char* server2, const char* server3) {
    if (!server1 || !server1[0]) return false;
    TimeZoneGuard guard;
    if (!guard.held()) return false;
    char stable[CAPACITY]{};
    if (!copyZone(timezone ? timezone : activeZone(), stable, sizeof(stable)) ||
        !applyZone(stable)) return false;
    // Arduino configTzTime also writes TZ. Keep the stable copy and guard alive
    // across that entire call. Its void return cannot prove network success.
    SystemTimePlatform::configureSntp(stable, server1, server2, server3);
    return std::strcmp(activeZone(), stable) == 0;
}

bool SystemTimeZone::copyActive(char* output, size_t capacity) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';
    TimeZoneGuard guard;
    return guard.held() && copyZone(activeZone(), output, capacity);
}

SystemTimeZone::LocalSnapshot SystemTimeZone::localSnapshot(int64_t epochMs) {
    LocalSnapshot result;
    TimeZoneGuard guard;
    if (!guard.held()) return result;
    result.timezoneAvailable = copyZone(activeZone(), result.timezone, sizeof(result.timezone));
    if (result.timezoneAvailable && epochMs >= 0)
        result.calendarAvailable = SystemTimePlatform::formatLocal(epochMs, result.calendar, sizeof(result.calendar));
    if (!result.calendarAvailable) result.calendar[0] = '\0';
    return result;
}
} // namespace FastBee
