#ifndef FASTBEE_SENSOR_CONFIG_EPOCH_H
#define FASTBEE_SENSOR_CONFIG_EPOCH_H

#include <atomic>
#include <stdint.h>

namespace FastBee {

// Configuration writers advance this while holding PeripheralManager's mutex.
// Readers carry the captured epoch through sampling, commit and event claim.
// Zero means "not captured" and is never a valid publication epoch.
class SensorConfigEpoch {
public:
    static uint32_t current() { return epoch().load(std::memory_order_acquire); }
    static bool matches(uint32_t expected) { return expected && expected == current(); }
    static void advance() {
        if (epoch().fetch_add(1, std::memory_order_acq_rel) == UINT32_MAX) {
            epoch().fetch_add(1, std::memory_order_acq_rel);
        }
    }

private:
    static std::atomic<uint32_t>& epoch() {
        static std::atomic<uint32_t> value{1};
        return value;
    }
};

} // namespace FastBee
#endif
