#pragma once
#include <ArduinoJson.h>
#include "core/PendingReportQueue.h"

namespace FastBee {
inline String pendingReportStatusJson(const PendingReportQueue::Status& status) {
    JsonDocument doc;
    doc["available"] = status.available;
    doc["deliveryVerified"] = false;
    doc["bindingScope"] = "mqtt_instance";
    if (status.available) {
        doc["depth"] = status.depth;
        doc["capacity"] = PendingReportQueue::Capacity;
        doc["maxPayloadBytes"] = PendingReportQueue::MaxBytes;
        doc["retryIntervalMs"] = PendingReportQueue::RetryMs;
        doc["inFlight"] = status.inFlight;
        doc["oldestAttempts"] = status.oldestAttempts;
        doc["retryDelayMs"] = status.retryDelayMs;
        doc["retryState"] = !status.depth ? "empty" : status.inFlight ? "in_flight" :
            status.retryDelayMs ? "cooldown" : "ready";
        doc["lastRetryResult"] = status.lastRetry == PendingReportQueue::LastRetry::Accepted ? "accepted" :
            status.lastRetry == PendingReportQueue::LastRetry::Rejected ? "rejected" :
            status.lastRetry == PendingReportQueue::LastRetry::InstanceChanged ? "instance_changed" : "none";
        if (status.depth) doc["oldestAgeMs"] = status.oldestAgeMs;
        doc["admitted"] = status.admitted;
        doc["evicted"] = status.evicted;
        doc["rejectedInvalid"] = status.rejectedInvalid;
        doc["rejectedMemory"] = status.rejectedMemory;
        doc["rejectedFull"] = status.rejectedFull;
        doc["rejectedBusy"] = status.rejectedBusy;
        doc["contention"] = status.contention;
        doc["retryAccepted"] = status.retryAccepted;
        doc["retryRejected"] = status.retryRejected;
        doc["rejectedUnknownInstance"] = status.rejectedUnknownInstance;
        doc["discardedInstanceChanged"] = status.discardedInstanceChanged;
    }
    String json;
    const size_t bytes = measureJson(doc);
    if (doc.overflowed() || !json.reserve(bytes) || serializeJson(doc, json) != bytes || json.length() != bytes)
        return "{\"available\":false,\"deliveryVerified\":false}";
    return json;
}
} // namespace FastBee
