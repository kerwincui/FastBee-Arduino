bool PeriphExecManager::thermostatSourceBindingLocked(const String& sourceRef, String& binding) const {
    binding = "";
    bool found = false;
    for (const auto& entry : rules) {
        for (const auto& action : entry.second.actions) {
            if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) continue;
            JsonDocument doc;
            if (deserializeJson(doc, action.actionValue.c_str()) || !doc.is<JsonObject>()) return false;
            String id = doc["periphId"] | "";
            if (id.isEmpty()) id = action.targetPeriphId;
            const String channel = doc["channelField"] | "";
            const String field = channel.isEmpty() ? PeriphExecExecutor::resolveChannelField(
                doc["sensorCategory"] | "analog", id, doc["dataField"] | "") : channel;
            const String ref = id + "_" + field;
            if (ref != sourceRef) continue;
            const String unit = doc["unit"] | "";
            // Disabled rules may still be explicitly executed. A second saved
            // writer of this cache key is ambiguous even when its timer is off.
            if (found || !entry.second.enabled || (unit != "°C" && unit != "℃" && unit != "C") ||
                action.useReceivedValue || id != action.targetPeriphId) return false;
            const size_t length = entry.first.length() + action.targetPeriphId.length() + action.actionValue.length() + 2;
            const String fingerprint = entry.first + ":" + action.targetPeriphId + ":" + action.actionValue;
            if (fingerprint.length() != length) return false;
            binding = fingerprint;
            if (binding != fingerprint) return false;
            found = true;
        }
    }
    return found;
}
