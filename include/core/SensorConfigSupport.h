#ifndef FASTBEE_SENSOR_CONFIG_SUPPORT_H
#define FASTBEE_SENSOR_CONFIG_SUPPORT_H

#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"

namespace FastBee {

// The same bounded representation is used by HTTP requests, persisted files,
// and configuration validation. These helpers never initialize hardware.
bool parseSensorConfig(JsonObjectConst params, PeripheralConfig& config, String& error);
void serializeSensorConfig(JsonObject params, const PeripheralConfig& config);
bool validateSensorConfig(const PeripheralConfig& config, String& error);
void buildSensorDriverParams(JsonDocument& params, const PeripheralConfig& config,
                             uint8_t sda, uint8_t scl, uint32_t frequency);
bool resolveSensorChannel(const char* driver, uint8_t mode, const char* requested,
                          String& channel, String& error);
uint32_t sensorSampleIntervalMs(const PeripheralConfig& config);

} // namespace FastBee
#endif
