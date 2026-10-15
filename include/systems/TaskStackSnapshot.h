#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "systems/TaskStackPermanentOwner.h"
#include "systems/TaskStackIpcOwners.h"

namespace FastBee {

enum class TaskStackCaptureStatus : uint8_t {
    Unavailable, Observed, Busy, Cooldown, MemoryPressure, CapacityExceeded,
    AllocationFailed, SdkFailed, ClockUnavailable
};

struct TaskStackEntry {
    uint32_t taskNumber = 0;
    uint32_t priority = 0;
    uint32_t highWaterBytes = 0;
    uint32_t state = 0;
    TaskStackPermanentOwner permanentOwner = TaskStackPermanentOwner::Unavailable;
};

// Owns only scalar copies. No RTOS handles, stack pointers or borrowed names.
struct TaskStackSnapshot {
    static constexpr size_t CAPACITY = 48;
    static constexpr uint32_t COOLDOWN_MS = 30000;
    static constexpr uint32_t MIN_FREE_INTERNAL8 = 32768;
    static constexpr uint32_t MIN_LARGEST_INTERNAL8 = 8192;
    TaskStackCaptureStatus status = TaskStackCaptureStatus::Unavailable;
    std::unique_ptr<TaskStackEntry[]> entries;
    uint32_t count = 0;
    uint32_t countBefore = 0;
    uint32_t countAfter = 0;
    uint32_t retryAfterMs = 0;
    size_t scratchBytes = 0;
    bool beforeAvailable = false;
    bool afterAvailable = false;
    bool timeAvailable = false;
    uint64_t sampledAtUptimeMs = 0;
    uint64_t captureDurationUs = 0;
    IpcTaskOwnerAttribution ipcAttribution{};
};

// The SDK may suspend scheduling while scanning. Permitted callers: the
// explicit POST route and the D6 phase-1 periodic health sampler, which is
// interval-gated (>=5 min) and admitted by the memory/cooldown budget below.
// Normal GETs never trigger a scan.
TaskStackSnapshot captureTaskStackSnapshot(bool severeMemoryPressure, bool requestIpcIdentity = true);
const char* taskStackCaptureReason(TaskStackCaptureStatus status);

struct CurrentTaskStackSample {
    bool observed = false;
    uint32_t highWaterBytes = UINT32_MAX;
    uint64_t sampledAtUptimeMs = 0;
};
// nullptr asks the SDK about the calling task, which cannot vanish mid-call.
CurrentTaskStackSample readCurrentTaskStackSample();

} // namespace FastBee
