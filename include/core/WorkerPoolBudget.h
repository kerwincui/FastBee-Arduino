#ifndef FASTBEE_WORKER_POOL_BUDGET_H
#define FASTBEE_WORKER_POOL_BUDGET_H
#include <stdint.h>
namespace FastBee {
struct WorkerPoolBudget {
    // Separate idle diagnostic admission from the measured HTTP transient.
    // 12KB covers the observed 8324-byte transient with 3964-byte margin;
    // this is a conservative reservation, not a measured worst-case bound.
    static constexpr uint32_t DIAGNOSTIC_FREE = 32768U;
    static constexpr uint32_t WEB_PEAK = 12288U;
    static constexpr uint32_t POST_WORKER_FREE = DIAGNOSTIC_FREE + WEB_PEAK;
    static constexpr uint32_t POST_WORKER_LARGEST = 20480U;
    static constexpr uint32_t CONTROL_RESERVE = 4096U;
    static constexpr bool canAdd(uint32_t free8, uint32_t largest8,
                                 uint32_t stackBytes, bool online,
                                 unsigned actual, unsigned maximum,
                                 bool concurrencyDemand) {
        return online && actual < maximum &&
            (actual == 0 || concurrencyDemand) &&
            stackBytes <= UINT32_MAX - CONTROL_RESERVE - POST_WORKER_FREE &&
            free8 >= stackBytes + CONTROL_RESERVE + POST_WORKER_FREE &&
            largest8 >= stackBytes + CONTROL_RESERVE + POST_WORKER_LARGEST;
    }
};
}
#endif
