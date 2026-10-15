#include "systems/TaskStackIpcOwners.h"
#include "core/ChipConfig.h"

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#endif
#if CHIP_HAS_S3_IPC_OWNER_SNAPSHOT
#include <esp_attr.h>
#include <esp_timer.h>
#include <esp_private/esp_ipc.h>
#include <freertos/task.h>
#define FASTBEE_IPC_OWNER_SUPPORTED 1
#endif

namespace FastBee {
const char* ipcOwnerReason(IpcOwnerStatus status) {
    switch (status) {
        case IpcOwnerStatus::NotRequested: return "EXPLICIT_REQUEST_REQUIRED";
        case IpcOwnerStatus::Pending: return "IPC_IDENTITY_PENDING";
        case IpcOwnerStatus::TimedOut: return "IPC_IDENTITY_TIMEOUT_CONTEXT_RETAINED";
        case IpcOwnerStatus::Busy: return "IPC_BUSY_EXPLICIT_REQUEST_REQUIRED";
        case IpcOwnerStatus::SdkUnavailable: return "IPC_SDK_REJECTED_CONTEXT_NOT_QUEUED";
        case IpcOwnerStatus::ClockUnavailable: return "IPC_IDENTITY_CLOCK_UNAVAILABLE";
        case IpcOwnerStatus::InvalidIdentity: return "IPC_IDENTITY_UNCONFIRMED";
        case IpcOwnerStatus::PublishedAfterCapture: return "IPC_IDENTITY_PUBLISHED_AFTER_CAPTURE";
        case IpcOwnerStatus::Partial: return "IPC_IDENTITIES_PARTIAL";
        case IpcOwnerStatus::Observed: return "CURRENT_IPC_CREATION_NUMBERS_SAME_BOOT";
        default: return "NONBLOCKING_IPC_OWNER_UNSUPPORTED";
    }
}

#if FASTBEE_IPC_OWNER_SUPPORTED
struct IpcOwnerContext {
    IpcTaskOwnerSlot slot;
    TaskStatus_t scratch{};
    const uint32_t core;
    explicit IpcOwnerContext(uint32_t value) : core(value) {}
};
static DRAM_ATTR IpcOwnerContext ipcContexts[2]{IpcOwnerContext{0}, IpcOwnerContext{1}};
static void captureCurrentIpcIdentity(void* argument) {
    auto& context = *static_cast<IpcOwnerContext*>(argument);
    // Current-task lifetime is guaranteed throughout the call. Do not scan its
    // stack or read any borrowed name, stack pointer, or captured handle.
    vTaskGetInfo(nullptr, &context.scratch, pdFALSE, eRunning);
    context.slot.publish(static_cast<uint32_t>(context.scratch.xTaskNumber),
        static_cast<uint32_t>(xPortGetCoreID()) == context.core, esp_timer_get_time());
}
#endif

IpcTaskOwnerAttribution sampleIpcTaskOwners(int64_t captureUs, bool explicitRequest) {
    IpcTaskOwnerAttribution out;
#if FASTBEE_IPC_OWNER_SUPPORTED
    out.fixedRamBytes = sizeof(ipcContexts);
#if CONFIG_ESP_IPC_USES_CALLERS_PRIORITY
    out.callerMayChangeSdkPriority = true;
#endif
    if (captureUs < 0) { out.status = IpcOwnerStatus::ClockUnavailable; return out; }
    for (uint32_t core = 0; core < 2; ++core) {
        auto& context = ipcContexts[core];
        if (explicitRequest && context.slot.begin(captureUs)) {
            const auto result = esp_ipc_call_nonblocking(core, captureCurrentIpcIdentity, &context);
            if (result != ESP_OK) context.slot.reject(result == ESP_FAIL);
        }
        out.cores[core] = context.slot.read(captureUs);
    }
    out.status = IpcOwnerStatus::Partial;
    if (out.cores[0].status == IpcOwnerStatus::Observed && out.cores[1].status == IpcOwnerStatus::Observed) {
        if (out.cores[0].taskNumber == out.cores[1].taskNumber) out.status = IpcOwnerStatus::InvalidIdentity;
        else out.status = IpcOwnerStatus::Observed;
    }
#else
    (void)captureUs;
    (void)explicitRequest;
#endif
    return out;
}
} // namespace FastBee
