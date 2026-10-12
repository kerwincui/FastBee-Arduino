#pragma once
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "systems/TaskStackSnapshot.h"

namespace FastBee {
inline void writeTaskStackSnapshotJson(JsonObject data, const TaskStackSnapshot& sample) {
    const bool observed = sample.status == TaskStackCaptureStatus::Observed && sample.entries &&
        sample.count > 0 && sample.count <= TaskStackSnapshot::CAPACITY;
    FastBee::setFixedJsonField(data, "status", observed ? "observed" : "unknown");
    FastBee::setFixedJsonField(data, "reason", taskStackCaptureReason(sample.status));
    FastBee::setFixedJsonField(data, "completeAtCapture", observed);
    FastBee::setFixedJsonField(data, "capacity", TaskStackSnapshot::CAPACITY);
    FastBee::setFixedJsonField(data, "stackUnit", "bytes");
    FastBee::setFixedJsonField(data, "nameStatus", "unavailable");
    FastBee::setFixedJsonField(data, "permanentOwnerScope", "sdk-idle-timer-and-observed-ipc-only");
    FastBee::setFixedJsonField(data, "permanentOwnerBasis", "idle-timer-handle-equality;ipc-current-task-creation-number-same-boot");
    JsonObject ipc = data["ipcAttribution"].to<JsonObject>();
    FastBee::setFixedJsonField(ipc, "status", sample.ipcAttribution.status == IpcOwnerStatus::Observed ? "observed" : "unknown");
    FastBee::setFixedJsonField(ipc, "reason", ipcOwnerReason(sample.ipcAttribution.status));
    FastBee::setFixedJsonField(ipc, "fixedRamBytes", sample.ipcAttribution.fixedRamBytes);
    FastBee::setFixedJsonField(ipc, "callerMayChangeSdkPriority", sample.ipcAttribution.callerMayChangeSdkPriority);
    FastBee::setFixedJsonField(ipc, "identityOnlyNoStackScan", true);
    FastBee::setFixedJsonField(ipc, "callerWaitsForCallback", false);
    JsonArray ipcCores = ipc["cores"].to<JsonArray>();
    for (uint32_t core = 0; core < 2; ++core) {
        const auto& evidence = sample.ipcAttribution.cores[core];
        JsonObject item = ipcCores.add<JsonObject>();
        FastBee::setFixedJsonField(item, "core", core);
        FastBee::setFixedJsonField(item, "reason", ipcOwnerReason(evidence.status));
        FastBee::setFixedJsonField(item, "generation", evidence.generation);
        FastBee::setFixedJsonField(item, "identityPublishedLate", evidence.publishedLate);
        FastBee::setFixedJsonField(item, "requestedAtUptimeMs", evidence.requestedAtUptimeMs);
        FastBee::setFixedJsonField(item, "publishedAtUptimeMs", evidence.publishedAtUptimeMs);
        if (evidence.status == IpcOwnerStatus::Observed) FastBee::setFixedJsonField(item, "taskNumber", evidence.taskNumber);
        else FastBee::setFixedJsonField(item, "taskNumber", nullptr);
    }
    FastBee::setFixedJsonField(data, "atomicSnapshot", false);
    FastBee::setFixedJsonField(data, "schedulerPausePossible", true);
    FastBee::setFixedJsonField(data, "hardDeadlineEnforced", false);
    FastBee::setFixedJsonField(data, "captureScratchBytes", sample.scratchBytes);
    FastBee::setFixedJsonField(data, "scalarCapacityBytes", sizeof(TaskStackEntry) * TaskStackSnapshot::CAPACITY);
    FastBee::setFixedJsonField(data, "retryAfterMs", sample.retryAfterMs);
    if (sample.beforeAvailable) FastBee::setFixedJsonField(data, "countBefore", sample.countBefore);
    else FastBee::setFixedJsonField(data, "countBefore", nullptr);
    if (sample.afterAvailable) FastBee::setFixedJsonField(data, "countAfter", sample.countAfter);
    else FastBee::setFixedJsonField(data, "countAfter", nullptr);
    if (sample.timeAvailable) {
        FastBee::setFixedJsonField(data, "sampledAtUptimeMs", sample.sampledAtUptimeMs);
        FastBee::setFixedJsonField(data, "captureDurationUs", sample.captureDurationUs);
    } else {
        FastBee::setFixedJsonField(data, "sampledAtUptimeMs", nullptr);
        FastBee::setFixedJsonField(data, "captureDurationUs", nullptr);
    }
    if (!observed) {
        FastBee::setFixedJsonField(data, "observedCount", nullptr);
        FastBee::setFixedJsonField(data, "tasks", nullptr);
        return;
    }
    FastBee::setFixedJsonField(data, "observedCount", sample.count);
    FastBee::setFixedJsonField(data, "countChangedAroundCapture", sample.countBefore != sample.count || sample.countAfter != sample.count);
    JsonArray tasks = data["tasks"].to<JsonArray>();
    for (uint32_t i = 0; i < sample.count; ++i) {
        JsonObject row = tasks.add<JsonObject>();
        FastBee::setFixedJsonField(row, "taskNumber", sample.entries[i].taskNumber);
        FastBee::setFixedJsonField(row, "name", nullptr);
        switch (sample.entries[i].permanentOwner) {
            case TaskStackPermanentOwner::IdleCore0: FastBee::setFixedJsonField(row, "sdkOwner", "idle-core0"); break;
            case TaskStackPermanentOwner::IdleCore1: FastBee::setFixedJsonField(row, "sdkOwner", "idle-core1"); break;
            case TaskStackPermanentOwner::TimerDaemon: FastBee::setFixedJsonField(row, "sdkOwner", "timer-daemon"); break;
            case TaskStackPermanentOwner::IpcCore0: FastBee::setFixedJsonField(row, "sdkOwner", "ipc-core0"); break;
            case TaskStackPermanentOwner::IpcCore1: FastBee::setFixedJsonField(row, "sdkOwner", "ipc-core1"); break;
            default: FastBee::setFixedJsonField(row, "sdkOwner", nullptr); break;
        }
        FastBee::setFixedJsonField(row, "priority", sample.entries[i].priority);
        FastBee::setFixedJsonField(row, "highWaterBytes", sample.entries[i].highWaterBytes);
        FastBee::setFixedJsonField(row, "stateCode", sample.entries[i].state);
    }
}
} // namespace FastBee
