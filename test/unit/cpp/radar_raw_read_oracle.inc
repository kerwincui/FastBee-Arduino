// Original pre-fix production read, retained only as a regression negative.
bool PeripheralManager::readRadarState(const String& id, bool& detected) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || !config->enabled || config->type != PeripheralType::RADAR_SENSOR) {
        return false;
    }
    if (config->pinCount < 1 || config->pins[0] == 255) {
        return false;
    }
    bool physicalHigh = digitalRead(config->pins[0]) == HIGH;
    detected = config->params.radar.activeHigh ? physicalHigh : !physicalHigh;
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.gpio.currentState = detected ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }
    return true;
}
