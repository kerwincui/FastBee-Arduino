inline void addMqttReportQueueStatus(JsonObject data, const MQTTClient::QueueSnapshot* snapshot) {
    JsonObject queue = data["reportQueue"].to<JsonObject>();
    queue["available"] = snapshot != nullptr;
    queue["deliveryVerified"] = false;
    const auto history = MQTTClient::getReportRetirementSnapshot();
    JsonObject retirement = queue["retirement"].to<JsonObject>();
    retirement["scope"] = "boot";
    retirement["persisted"] = false;
    retirement["payloadRetained"] = false;
    retirement["instances"] = history.instances;
    retirement["instancesWithLoss"] = history.instancesWithLoss;
    retirement["discardedReports"] = history.discardedReports;
    retirement["unconfirmedInstances"] = history.unconfirmedInstances;
    retirement["saturated"] = history.saturated;
    retirement["lastLossReason"] = mqttRetirementReasonName(history.lastLossReason);
    retirement["lastLossRingDepth"] = history.lastLossRingDepth;
    retirement["lastLossPendingReport"] = history.lastLossPendingReport;
    JsonObject reasons = retirement["byReason"].to<JsonObject>();
    for (uint8_t i = 0; i < MqttReportRetirement::ReasonCount; ++i)
        reasons[mqttRetirementReasonName(static_cast<MqttRetirementReason>(i))] = history.byReason[i];
    if (!snapshot) return;
    const auto& s = *snapshot;
    queue["capacity"] = MQTT_REPORT_SLOTS + 1; // Ring plus dedicated retained frame.
    queue["ringCapacity"] = MQTT_REPORT_SLOTS;
    queue["depth"] = s.depth;
    queue["ringDepth"] = s.ringDepth;
    queue["pendingReport"] = s.pendingReport;
    queue["consumerActive"] = s.consumerActive;
    queue["pendingAttempts"] = s.pendingAttempts;
    queue["retryDelayMs"] = s.retryDelayMs;
    queue["accepted"] = s.stats.enqueueTotal;
    queue["configuredTopicAccepted"] = s.stats.configuredTopicAccepted;
    queue["publishedLocally"] = s.stats.publishTotal;
    queue["evictedOldest"] = s.stats.evictedOldest;
    queue["rejectedInvalid"] = s.stats.invalidDrops;
    queue["rejectedThrottle"] = s.stats.throttleDrops;
    queue["publishRejected"] = s.stats.publishRejected;
    queue["allocationDeferred"] = s.stats.allocationDeferred;
    queue["rejectedTestMode"] = s.stats.testModeRejected;
}

