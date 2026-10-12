#pragma once
#include <stdint.h>

// The patched dependency owns this optional, allocation-free provider.
extern "C" bool fastbee_async_tcp_admission_snapshot(uint32_t* accepted, uint32_t* limit)
    __attribute__((weak));

namespace FastBee {
struct AsyncTCPAdmissionSnapshot {
    bool available;
    uint32_t accepted;
    uint32_t limit;
};

inline AsyncTCPAdmissionSnapshot captureAsyncTCPAdmissionSnapshot() {
    AsyncTCPAdmissionSnapshot result{false, 0U, 0U};
    if (fastbee_async_tcp_admission_snapshot) {
        result.available = fastbee_async_tcp_admission_snapshot(&result.accepted, &result.limit);
    }
    return result;
}
}  // namespace FastBee
