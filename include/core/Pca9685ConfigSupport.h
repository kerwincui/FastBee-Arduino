#pragma once
#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"
#include "core/FeatureInstanceRegistry.h"

namespace FastBee {
// Caller holds the already-created PeripheralManager mutex. No I/O or allocation.
FeatureInstanceSnapshot pca9685InstanceSnapshotLocked();
struct Pca9685Command { char action[8] = {}; uint32_t channel = 0; double value = 0; };
// Literal-only rule payload: no startup confirmation, target redirection or templates.
bool parsePca9685Command(const String& json, Pca9685Command& output, String& error);
bool validatePca9685Config(const PeripheralConfig& config, String& error);
bool parsePca9685Config(JsonObjectConst params, PeripheralConfig& config, String& error);
void serializePca9685Config(JsonObject params, const PeripheralConfig& config);
}
