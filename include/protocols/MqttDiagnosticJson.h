#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

namespace FastBee {
// Fixed local diagnostics only. Transport error translation remains with its
// existing producer; this writer does not read or change client/device state.
enum class MqttDiagnostic : uint8_t {
    Ok, ConfigInvalid, ClientUninitialized, ClientStartFailed, LifecycleBusy,
    MemoryDisabled, MemoryStopped, BrokerConfigInvalid, RebuildNotCompleted,
    AesClientIdInvalid, ConfigSaveFailed, Count
};
namespace MqttDiagnosticFields {
constexpr uint8_t LastError = 1;
constexpr uint8_t Error = 2;
constexpr uint8_t LastMessage = 4;
constexpr uint8_t Reason = 8;
constexpr uint8_t CopyMessage = 16;
constexpr uint8_t Status = LastError | LastMessage;
}
// Preserve each caller's exact legacy alias and optional fields. CopyMessage
// keeps the former const-char-pointer copy policy where it existed; otherwise
// all descriptor strings have static storage, as did the replaced literals.
void writeMqttDiagnostic(JsonObject data, MqttDiagnostic diagnostic, uint8_t fields);
// For legacy nested doc["data"][key] writers: retain their per-field parent
// creation attempts (including after a transient allocation failure).
void writeMqttDiagnosticData(JsonDocument& doc, MqttDiagnostic diagnostic, uint8_t fields);
} // namespace FastBee
