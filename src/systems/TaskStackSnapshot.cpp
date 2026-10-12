#include "systems/TaskStackSnapshot.h"
#include <atomic>
#include <new>

#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <freertos/timers.h>
#endif

namespace FastBee {

const char* taskStackCaptureReason(TaskStackCaptureStatus status) {
    switch (status) {
        case TaskStackCaptureStatus::Observed: return "SDK_SCALAR_SNAPSHOT";
        case TaskStackCaptureStatus::Busy: return "CAPTURE_BUSY";
        case TaskStackCaptureStatus::Cooldown: return "CAPTURE_COOLDOWN";
        case TaskStackCaptureStatus::MemoryPressure: return "INTERNAL_MEMORY_BUDGET";
        case TaskStackCaptureStatus::CapacityExceeded: return "TASK_CAPACITY_EXCEEDED";
        case TaskStackCaptureStatus::AllocationFailed: return "SNAPSHOT_ALLOCATION_FAILED";
        case TaskStackCaptureStatus::SdkFailed: return "SDK_SNAPSHOT_UNCONFIRMED";
        case TaskStackCaptureStatus::ClockUnavailable: return "CAPTURE_CLOCK_UNAVAILABLE";
        default: return "SDK_TRACE_UNAVAILABLE";
    }
}

TaskStackSnapshot captureTaskStackSnapshot(bool severeMemoryPressure, bool requestIpcIdentity) {
    TaskStackSnapshot out;
#if defined(ESP_PLATFORM) && configUSE_TRACE_FACILITY == 1
    // Atomic storage must stay in internal DRAM on Xtensa; a PSRAM atomic can
    // enter the SDK's waiting fallback. One strong CAS is the entire admission.
    static DRAM_ATTR std::atomic<uint32_t> capturing{0};
    static bool attempted = false; // Protected by capturing, including millis()==0.
    static uint32_t lastAttemptMs = 0;
    uint32_t expected = 0;
    if (!capturing.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
        out.status = TaskStackCaptureStatus::Busy;
        return out;
    }
    struct ReleaseCapture {
        std::atomic<uint32_t>& flag;
        ~ReleaseCapture() { flag.store(0, std::memory_order_release); }
    } release{capturing};

    const uint32_t now = static_cast<uint32_t>(millis());
    const uint32_t elapsed = now - lastAttemptMs;
    if (attempted && elapsed < TaskStackSnapshot::COOLDOWN_MS) {
        out.status = TaskStackCaptureStatus::Cooldown;
        out.retryAfterMs = TaskStackSnapshot::COOLDOWN_MS - elapsed;
        return out;
    }
    constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (severeMemoryPressure ||
        heap_caps_get_free_size(caps) < TaskStackSnapshot::MIN_FREE_INTERNAL8 ||
        heap_caps_get_largest_free_block(caps) < TaskStackSnapshot::MIN_LARGEST_INTERNAL8) {
        out.status = TaskStackCaptureStatus::MemoryPressure;
        return out;
    }
    // Failed admitted attempts also back off; an OOM/capacity error must not
    // invite an unbounded retry loop. Rejected auth/memory never starts a scan.
    attempted = true;
    lastAttemptMs = now;
    out.countBefore = static_cast<uint32_t>(uxTaskGetNumberOfTasks());
    out.beforeAvailable = true;
    if (out.countBefore > TaskStackSnapshot::CAPACITY) {
        out.status = TaskStackCaptureStatus::CapacityExceeded;
        return out;
    }
    out.scratchBytes = sizeof(TaskStatus_t) * TaskStackSnapshot::CAPACITY;
    struct FreeScratch {
        void operator()(TaskStatus_t* ptr) const { heap_caps_free(ptr); }
    };
    std::unique_ptr<TaskStatus_t, FreeScratch> scratch(static_cast<TaskStatus_t*>(
        heap_caps_calloc(TaskStackSnapshot::CAPACITY, sizeof(TaskStatus_t), caps)));
    out.entries.reset(new (std::nothrow) TaskStackEntry[TaskStackSnapshot::CAPACITY]);
    if (!scratch || !out.entries) {
        out.entries.reset();
        out.status = TaskStackCaptureStatus::AllocationFailed;
        return out;
    }
    const int64_t startedUs = esp_timer_get_time();
    if (startedUs < 0) {
        out.entries.reset();
        out.status = TaskStackCaptureStatus::ClockUnavailable;
        return out;
    }
    out.ipcAttribution = sampleIpcTaskOwners(startedUs, requestIpcIdentity);
    // Only scheduler-lifetime SDK handles are eligible. Never look up dynamic
    // task names, read writable trace task numbers, or re-query captured handles.
    TaskHandle_t idle0 = nullptr, idle1 = nullptr, timer = nullptr;
#if INCLUDE_xTaskGetSchedulerState == 1
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
#if !CONFIG_FREERTOS_SMP && INCLUDE_xTaskGetIdleTaskHandle == 1
        idle0 = xTaskGetIdleTaskHandleForCore(0);
#if configNUMBER_OF_CORES > 1
        idle1 = xTaskGetIdleTaskHandleForCore(1);
#endif
#endif
#if configUSE_TIMERS == 1
        timer = xTimerGetTimerDaemonTaskHandle();
#endif
    }
#endif
    const UBaseType_t captured = uxTaskGetSystemState(scratch.get(), TaskStackSnapshot::CAPACITY, nullptr);
    const int64_t finishedUs = esp_timer_get_time();
    out.countAfter = static_cast<uint32_t>(uxTaskGetNumberOfTasks());
    out.afterAvailable = true;
    if (finishedUs >= startedUs) {
        out.timeAvailable = true;
        out.sampledAtUptimeMs = static_cast<uint64_t>(startedUs) / 1000;
        out.captureDurationUs = static_cast<uint64_t>(finishedUs - startedUs);
    }
    if (!captured || captured > TaskStackSnapshot::CAPACITY || !out.timeAvailable) {
        out.entries.reset();
        out.status = !out.timeAvailable ? TaskStackCaptureStatus::ClockUnavailable
            : captured > TaskStackSnapshot::CAPACITY ? TaskStackCaptureStatus::CapacityExceeded
            : TaskStackCaptureStatus::SdkFailed;
        return out;
    }
    for (UBaseType_t i = 0; i < captured; ++i) {
        // TaskStatus_t owns these scalars. pcTaskName/xHandle/pxStackBase can
        // already refer to a deleted task: never dereference or re-query them.
        out.entries[i] = {static_cast<uint32_t>(scratch.get()[i].xTaskNumber),
            static_cast<uint32_t>(scratch.get()[i].uxCurrentPriority),
            static_cast<uint32_t>(scratch.get()[i].usStackHighWaterMark),
            static_cast<uint32_t>(scratch.get()[i].eCurrentState),
            matchTaskStackPermanentOwner(scratch.get()[i].xHandle, idle0, idle1, timer)};
        if (out.entries[i].permanentOwner == TaskStackPermanentOwner::Unavailable &&
            out.ipcAttribution.status == IpcOwnerStatus::Observed) {
            for (uint32_t core = 0; core < 2; ++core) {
                if (out.entries[i].taskNumber == out.ipcAttribution.cores[core].taskNumber) {
                    out.entries[i].permanentOwner = core == 0 ? TaskStackPermanentOwner::IpcCore0 : TaskStackPermanentOwner::IpcCore1;
                }
            }
        }
    }
    out.count = static_cast<uint32_t>(captured);
    out.status = TaskStackCaptureStatus::Observed;
#else
    (void)severeMemoryPressure;
    (void)requestIpcIdentity;
#endif
    return out;
}

CurrentTaskStackSample readCurrentTaskStackSample() {
    CurrentTaskStackSample out;
#if defined(ESP_PLATFORM) && INCLUDE_uxTaskGetStackHighWaterMark == 1
    const int64_t nowUs = esp_timer_get_time();
    if (nowUs >= 0) {
        out.highWaterBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        out.sampledAtUptimeMs = static_cast<uint64_t>(nowUs) / 1000;
        out.observed = true;
    }
#endif
    return out;
}

} // namespace FastBee
