#ifndef FASTBEE_PERIPHERAL_ROUTE_MAPPER_H
#define FASTBEE_PERIPHERAL_ROUTE_MAPPER_H

#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"

namespace FastBee {

// Strict hex parsing shared by route handlers.
bool parseStrictHex(const String& input, uint8_t* output, size_t capacity,
                    size_t& length, String& errorMsg);

constexpr uint32_t SENSOR_DEFAULT_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t SENSOR_MIN_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t SENSOR_MAX_SAMPLE_INTERVAL_MS = 86400000UL;

// Calendar helpers for the DS1302 route (Sakamoto weekday, Monday=1..Sunday=7).
int daysInMonth(int year, int month);
uint8_t calculateDayOfWeek(int year, int month, int day);

String bytesToHex(const uint8_t* data, size_t length);
uint32_t normalizeSensorSampleInterval(int value);

bool isAnalogInputPeripheral(PeripheralType type);
bool isDigitalInputPeripheral(PeripheralType type);
const char* userConfigurablePeripheralTypeError(PeripheralType type);

// Type-specific params serialization shared by list/detail/preview routes.
void addDigitalInputParams(JsonObject params, const PeripheralConfig& config);
void addAnalogInputParams(JsonObject params, const PeripheralConfig& config, bool includeAdvanced = true);
void addServoParams(JsonObject params, const PeripheralConfig& config);
void addUartParams(JsonObject params, const PeripheralConfig& config);
void addEncoderParams(JsonObject params, const PeripheralConfig& config);
void addGenericPeripheralFields(JsonObject object, const PeripheralConfig& config);
void addP1PeripheralParams(JsonObject params, const PeripheralConfig& config);
void addDisplayParams(JsonObject params, const PeripheralConfig& config);
void addPeripheralDiagnosticCapability(JsonObject target, const PeripheralConfig& config);

int mutationErrorStatus(const String& errorMsg);
bool parsePeripheralPins(const String& value, uint8_t* pins, uint8_t& count,
                         String& errorMsg);
String escapePeriphJsonString(const String& value);

// 无源蜂鸣器标识序列化：GPIO数字输出 + passive 时写入 passive=true。
// 前端据此将动作类型映射为“设置PWM”（无源蜂鸣器由 PWM 占空比驱动）。
void addPassiveBuzzerFlag(JsonVariant doc, const PeripheralConfig& config);

// Compact list item serialization (identity block + common params block).
void addPeripheralListIdentity(JsonDocument& doc, const PeripheralConfig& config);
void addPeripheralListParams(JsonDocument& doc, const PeripheralConfig& config);

// List memory gates (DRAM-only; PSRAM must not keep full list responses alive).
bool isListMemoryCriticallyLow();
bool shouldForceCompactList();

} // namespace FastBee

#endif // FASTBEE_PERIPHERAL_ROUTE_MAPPER_H
