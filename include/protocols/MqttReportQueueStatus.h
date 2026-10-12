#pragma once
#include <ArduinoJson.h>
#include "protocols/MQTTClient.h"
#include "network/FixedJsonField.h"

namespace FastBee {
inline void addMqttReportQueueStatus(JsonObject data, const MQTTClient::QueueSnapshot* snapshot) {
    JsonObject queue = data["reportQueue"].to<JsonObject>();
    FastBee::setFixedJsonField(queue, "available", snapshot != nullptr);
    FastBee::setFixedJsonField(queue, "deliveryVerified", false);
    const auto history = MQTTClient::getReportRetirementSnapshot();
    JsonObject retirement = queue["retirement"].to<JsonObject>();
    FastBee::setFixedJsonField(retirement, "scope", "boot");
    FastBee::setFixedJsonField(retirement, "persisted", false);
    FastBee::setFixedJsonField(retirement, "payloadRetained", false);
    FastBee::setFixedJsonField(retirement, "instances", history.instances);
    FastBee::setFixedJsonField(retirement, "instancesWithLoss", history.instancesWithLoss);
    FastBee::setFixedJsonField(retirement, "discardedReports", history.discardedReports);
    FastBee::setFixedJsonField(retirement, "unconfirmedInstances", history.unconfirmedInstances);
    FastBee::setFixedJsonField(retirement, "saturated", history.saturated);
    FastBee::setFixedJsonField(retirement, "lastLossReason", mqttRetirementReasonName(history.lastLossReason));
    FastBee::setFixedJsonField(retirement, "lastLossRingDepth", history.lastLossRingDepth);
    FastBee::setFixedJsonField(retirement, "lastLossPendingReport", history.lastLossPendingReport);
    JsonObject reasons = retirement["byReason"].to<JsonObject>();
    for (uint8_t i = 0; i < MqttReportRetirement::ReasonCount; ++i)
        reasons[mqttRetirementReasonName(static_cast<MqttRetirementReason>(i))] = history.byReason[i];
    if (!snapshot) return;
    const auto& s = *snapshot;
    FastBee::setFixedJsonField(queue, "capacity", MQTT_REPORT_SLOTS + 1); // Ring plus dedicated retained frame.
    FastBee::setFixedJsonField(queue, "ringCapacity", MQTT_REPORT_SLOTS);
    FastBee::setFixedJsonField(queue, "depth", s.depth);
    FastBee::setFixedJsonField(queue, "ringDepth", s.ringDepth);
    FastBee::setFixedJsonField(queue, "pendingReport", s.pendingReport);
    FastBee::setFixedJsonField(queue, "consumerActive", s.consumerActive);
    FastBee::setFixedJsonField(queue, "pendingAttempts", s.pendingAttempts);
    FastBee::setFixedJsonField(queue, "retryDelayMs", s.retryDelayMs);
    FastBee::setFixedJsonField(queue, "accepted", s.stats.enqueueTotal);
    FastBee::setFixedJsonField(queue, "configuredTopicAccepted", s.stats.configuredTopicAccepted);
    FastBee::setFixedJsonField(queue, "publishedLocally", s.stats.publishTotal);
    FastBee::setFixedJsonField(queue, "evictedOldest", s.stats.evictedOldest);
    FastBee::setFixedJsonField(queue, "rejectedInvalid", s.stats.invalidDrops);
    FastBee::setFixedJsonField(queue, "rejectedThrottle", s.stats.throttleDrops);
    FastBee::setFixedJsonField(queue, "publishRejected", s.stats.publishRejected);
    FastBee::setFixedJsonField(queue, "allocationDeferred", s.stats.allocationDeferred);
    FastBee::setFixedJsonField(queue, "rejectedTestMode", s.stats.testModeRejected);
}
} // namespace FastBee
