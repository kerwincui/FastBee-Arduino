#pragma once
#include <ArduinoJson.h>
#include "core/PendingReportQueue.h"
#include "network/FixedJsonField.h"

namespace FastBee {
inline String pendingReportStatusJson(const PendingReportQueue::Status& status) {
    JsonDocument doc;
    setFixedJsonField(doc, "available", status.available);
    setFixedJsonField(doc, "deliveryVerified", false);
    setFixedJsonField(doc, "bindingScope", "mqtt_instance");
    if (status.available) {
        setFixedJsonField(doc, "depth", status.depth);
        setFixedJsonField(doc, "capacity", PendingReportQueue::Capacity);
        setFixedJsonField(doc, "maxPayloadBytes", PendingReportQueue::MaxBytes);
        setFixedJsonField(doc, "retryIntervalMs", PendingReportQueue::RetryMs);
        setFixedJsonField(doc, "inFlight", status.inFlight);
        setFixedJsonField(doc, "oldestAttempts", status.oldestAttempts);
        setFixedJsonField(doc, "retryDelayMs", status.retryDelayMs);
        setFixedJsonField(doc, "retryState", !status.depth ? "empty" : status.inFlight ? "in_flight" :
            status.retryDelayMs ? "cooldown" : "ready");
        setFixedJsonField(doc, "lastRetryResult", status.lastRetry == PendingReportQueue::LastRetry::Accepted ? "accepted" :
            status.lastRetry == PendingReportQueue::LastRetry::Rejected ? "rejected" :
            status.lastRetry == PendingReportQueue::LastRetry::InstanceChanged ? "instance_changed" : "none");
        if (status.depth) setFixedJsonField(doc, "oldestAgeMs", status.oldestAgeMs);
        setFixedJsonField(doc, "admitted", status.admitted);
        setFixedJsonField(doc, "evicted", status.evicted);
        setFixedJsonField(doc, "rejectedInvalid", status.rejectedInvalid);
        setFixedJsonField(doc, "rejectedMemory", status.rejectedMemory);
        setFixedJsonField(doc, "rejectedFull", status.rejectedFull);
        setFixedJsonField(doc, "rejectedBusy", status.rejectedBusy);
        setFixedJsonField(doc, "contention", status.contention);
        setFixedJsonField(doc, "retryAccepted", status.retryAccepted);
        setFixedJsonField(doc, "retryRejected", status.retryRejected);
        setFixedJsonField(doc, "rejectedUnknownInstance", status.rejectedUnknownInstance);
        setFixedJsonField(doc, "discardedInstanceChanged", status.discardedInstanceChanged);
    }
    String json;
    const size_t bytes = measureJson(doc);
    if (doc.overflowed() || !json.reserve(bytes) || serializeJson(doc, json) != bytes || json.length() != bytes)
        return "{\"available\":false,\"deliveryVerified\":false}";
    return json;
}
} // namespace FastBee
