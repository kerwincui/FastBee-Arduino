#pragma once
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimeZone.h"

namespace FastBee {
inline void appendSystemTimeStatus(JsonObject data) {
    const auto status = SystemTimeQuality::snapshot();
    FastBee::setFixedJsonField(data, "synced", status.synced);
    FastBee::setFixedJsonField(data, "timeValid", status.timeValid);
    FastBee::setFixedJsonField(data, "timeSource", status.source);
    FastBee::setFixedJsonField(data, "timeQuality", status.quality);
    FastBee::setFixedJsonField(data, "rtcBootStatus", status.rtcBootStatus);
    FastBee::setFixedJsonField(data, "timeSourceAuthenticated", status.transportAuthenticated);
    FastBee::setFixedJsonField(data, "timeSyncAgeMs", status.ageMs);
    FastBee::setFixedJsonField(data, "timeSyncSequence", status.sequence);
    auto local = SystemTimeZone::localSnapshot(status.timeValid ? status.epochMs : -1);
    FastBee::setFixedJsonField(data, "activeTimezoneAvailable", local.timezoneAvailable);
    // Keep the mutable array overload so ArduinoJson copies stack storage.
    if (local.timezoneAvailable) FastBee::setFixedJsonField(data, "activeTimezone", local.timezone);
    else FastBee::setFixedJsonField(data, "activeTimezone", "unknown");
    FastBee::setFixedJsonField(data, "timeDisplayAvailable", local.calendarAvailable);
    if (status.timeValid) FastBee::setFixedJsonField(data, "timestamp", status.epochMs / 1000);
    else FastBee::setFixedJsonField(data, "timestamp", nullptr);
    if (local.calendarAvailable) FastBee::setFixedJsonField(data, "datetime", local.calendar);
    else FastBee::setFixedJsonField(data, "datetime", nullptr);
}
} // namespace FastBee
