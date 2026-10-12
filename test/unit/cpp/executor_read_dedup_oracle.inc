// Frozen executable blocks from r14 PeriphExecExecutor.cpp; only names,
// indentation and the parse-success return used by the caller are adapted.
bool loadSensorReadSnapshotOracle(PeripheralManager& pm, const char* periphId, PeripheralConfig& cfg) {
    if (!pm.getPeripheralSnapshot(String(periphId), cfg)) {
        LOGGER.warningf("[PeriphExec] Sensor read: peripheral '%s' not found", periphId);
        return false;
    }
    return true;
}
void invalidateSensorReadDisplayOracle(const char* periphId, const char* dataField) {
    if (LCDManager::getInstance().isInitialized()) {
        LCDManager::getInstance().invalidateSensorEntry(String(periphId) + "_" + String(dataField));
    }
}
bool appendModbusPollReadingsOracle(JsonDocument& taskDoc, const String& encoded, JsonArray resultsArr) {
    if (!deserializeJson(taskDoc, encoded.c_str(), encoded.length())) {
        JsonArray taskArr = taskDoc.as<JsonArray>();
        for (JsonVariant item : taskArr) {
            resultsArr.add(item);
        }
    } else {
        return false;
    }
    return true;
}

