namespace StatusSerializerOracle {
using namespace FastBee;
inline void writeTaskStackSnapshotJson(JsonObject data, const TaskStackSnapshot& sample) {
    const bool observed = sample.status == TaskStackCaptureStatus::Observed && sample.entries &&
        sample.count > 0 && sample.count <= TaskStackSnapshot::CAPACITY;
    data["status"] = observed ? "observed" : "unknown";
    data["reason"] = taskStackCaptureReason(sample.status);
    data["completeAtCapture"] = observed;
    data["capacity"] = TaskStackSnapshot::CAPACITY;
    data["stackUnit"] = "bytes";
    data["nameStatus"] = "unavailable";
    data["atomicSnapshot"] = false;
    data["schedulerPausePossible"] = true;
    data["hardDeadlineEnforced"] = false;
    data["captureScratchBytes"] = sample.scratchBytes;
    data["scalarCapacityBytes"] = sizeof(TaskStackEntry) * TaskStackSnapshot::CAPACITY;
    data["retryAfterMs"] = sample.retryAfterMs;
    if (sample.beforeAvailable) data["countBefore"] = sample.countBefore;
    else data["countBefore"] = nullptr;
    if (sample.afterAvailable) data["countAfter"] = sample.countAfter;
    else data["countAfter"] = nullptr;
    if (sample.timeAvailable) {
        data["sampledAtUptimeMs"] = sample.sampledAtUptimeMs;
        data["captureDurationUs"] = sample.captureDurationUs;
    } else {
        data["sampledAtUptimeMs"] = nullptr;
        data["captureDurationUs"] = nullptr;
    }
    if (!observed) {
        data["observedCount"] = nullptr;
        data["tasks"] = nullptr;
        return;
    }
    data["observedCount"] = sample.count;
    data["countChangedAroundCapture"] = sample.countBefore != sample.count || sample.countAfter != sample.count;
    JsonArray tasks = data["tasks"].to<JsonArray>();
    for (uint32_t i = 0; i < sample.count; ++i) {
        JsonObject row = tasks.add<JsonObject>();
        row["taskNumber"] = sample.entries[i].taskNumber;
        row["name"] = nullptr;
        row["priority"] = sample.entries[i].priority;
        row["highWaterBytes"] = sample.entries[i].highWaterBytes;
        row["stateCode"] = sample.entries[i].state;
    }
}
} // namespace StatusSerializerOracle


namespace StatusSerializerOracle {
using namespace FastBee;

// Both SD status endpoints use the same ownership/unknown-value contract.
inline void writeSdCardStatusJson(JsonObject data, const SDCardStatus& status, const String& ownerId) {
    for (const char* key : {"configured", "mounted", "cardType", "cardBytes", "totalBytes",
                            "usedBytes", "capacityAgeMs", "driverError", "recoverySuggested",
                            "ioFailures", "recoveryAttempts", "recoveries", "lastFailureCode"}) data.remove(key);
    data["statusAvailable"] = status.statusAvailable;
    data["configurationAvailable"] = status.statusAvailable || status.configurationAvailable;
    data["capacityQuality"] = "unavailable";
    if (!status.statusAvailable && !status.configurationAvailable) {
        if (!status.lastError.isEmpty()) data["driverError"] = status.lastError.c_str();
        return;
    }
    const bool owns = status.configured && status.ownerId == ownerId;
    data["configured"] = owns;
    if (status.statusAvailable) data["mounted"] = owns && status.mounted;
    if (!owns) {
        if (status.configured) data["driverError"] = "SD_OWNER_MISMATCH: another peripheral owns storage";
        return;
    }
    if (!status.lastError.isEmpty()) data["driverError"] = status.lastError.c_str();
    data["recoverySuggested"] = status.recoverySuggested;
    data["ioFailures"] = status.ioFailures;
    data["recoveryAttempts"] = status.recoveryAttempts;
    data["recoveries"] = status.recoveries;
    data["lastFailureCode"] = status.lastFailureCode.c_str();
    if (!status.statusAvailable || !status.mounted || !status.capacityValid) return;
    data["cardType"] = status.cardType.c_str();
    data["cardBytes"] = status.cardBytes;
    data["totalBytes"] = status.totalBytes;
    data["usedBytes"] = status.usedBytes;
    data["capacityQuality"] = "last_operation_snapshot";
    data["capacityAgeMs"] = status.capacityAgeMs;
}

} // namespace StatusSerializerOracle


namespace StatusSerializerOracle {
using namespace FastBee;
inline void appendSystemTimeStatus(JsonObject data) {
    const auto status = SystemTimeQuality::snapshot();
    data["synced"] = status.synced;
    data["timeValid"] = status.timeValid;
    data["timeSource"] = status.source;
    data["timeQuality"] = status.quality;
    data["timeSourceAuthenticated"] = status.transportAuthenticated;
    data["timeSyncAgeMs"] = status.ageMs;
    data["timeSyncSequence"] = status.sequence;
    auto local = SystemTimeZone::localSnapshot(status.timeValid ? status.epochMs : -1);
    data["activeTimezoneAvailable"] = local.timezoneAvailable;
    // Keep the mutable array overload so ArduinoJson copies stack storage.
    if (local.timezoneAvailable) data["activeTimezone"] = local.timezone;
    else data["activeTimezone"] = "unknown";
    data["timeDisplayAvailable"] = local.calendarAvailable;
    if (status.timeValid) data["timestamp"] = status.epochMs / 1000;
    else data["timestamp"] = nullptr;
    if (local.calendarAvailable) data["datetime"] = local.calendar;
    else data["datetime"] = nullptr;
}
} // namespace StatusSerializerOracle
