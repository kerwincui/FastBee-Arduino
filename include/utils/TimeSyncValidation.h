#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace FastBee {

// Strict parsing for the legacy XML/numeric-string HTTP time response. Do not
// accept a numeric prefix, embedded NUL, infinity or an oversized token.
inline bool parseTimeMilliseconds(const char* text, size_t size, double& value) {
    value = 0;
    if (!text || !size || size > 40 || std::memchr(text, '\0', size)) return false;
    char token[41];
    std::memcpy(token, text, size); token[size] = '\0';
    char* end = nullptr;
    const double parsed = std::strtod(token, &end);
    if (end == token) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

// Preserve the existing FastBee four-timestamp estimate, but reject impossible
// samples before narrowing to the platform time_t. Monotonic elapsed arithmetic
// deliberately uses 32 bits so a millis() rollover is not a multi-day setback.
// This is plausibility, NOT authentication, precision or NTP provenance.
template <typename Seconds>
bool estimateHttpTimeMilliseconds(double serverRecvMs, double serverSendMs,
                                 uint32_t sentAt, uint32_t receivedAt,
                                 unsigned long timeout, long long& result) {
    result = 0;
    // HTTPClient read timeout is uint16_t; do not silently truncate it.
    if (!timeout || timeout > UINT16_MAX) return false;
    const uint32_t elapsed = receivedAt - sentAt;
    if (elapsed > timeout || !std::isfinite(serverRecvMs) || !std::isfinite(serverSendMs) ||
        serverRecvMs <= 1000000000000.0 || serverSendMs <= 1000000000000.0 ||
        serverSendMs < serverRecvMs || serverSendMs - serverRecvMs > elapsed) return false;
    const double estimated = serverRecvMs + (serverSendMs - serverRecvMs + elapsed) / 2.0;
    // Strict upper bound avoids the rounded double representation of LLONG_MAX.
    if (!std::isfinite(estimated) || estimated >= static_cast<double>(std::numeric_limits<long long>::max()))
        return false;
    const long long milliseconds = static_cast<long long>(estimated);
    if (milliseconds / 1000 > std::numeric_limits<Seconds>::max()) return false;
    result = milliseconds;
    return true;
}

} // namespace FastBee
