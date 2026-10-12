#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/UartTextDiagnostics.h"

namespace FastBee {
inline String uartTextStatusJson() {
    const auto status = UartTextDiagnostics::snapshot();
    JsonDocument doc;
    doc["available"] = true;
    doc["scope"] = "boot";
    doc["unit"] = "events";
    doc["peerVerified"] = false;
    doc["maxFrameBytes"] = 160;
    doc["idleBoundaryMs"] = 500;
    doc["countsAreLowerBounds"] = status.lowerBounds;
    doc["hardwareErrorScope"] = "first_observed_generic_stream_fault";
    doc["hardwareErrorRecovery"] = "explicit_restart";
    doc["hardwareErrorDeliveryConfirmed"] = false;
    static const char* const names[] = {"dispatchAttempts", "oversizedFrames", "invalidByteFrames",
        "readFailures", "sourceChanges", "staleFrames", "serializationFailures",
        "hardwareFifoFaults", "hardwareBufferFaults", "hardwareFrameFaults",
        "hardwareParityFaults", "hardwareBreakFaults"};
    static_assert(sizeof(names) / sizeof(names[0]) == UartTextDiagnostics::Count,
                  "UART diagnostic fields must match counters");
    for (size_t i = 0; i < UartTextDiagnostics::Count; ++i) doc[names[i]] = status.values[i];
    String json;
    const size_t bytes = measureJson(doc);
    if (doc.overflowed() || !json.reserve(bytes) || serializeJson(doc, json) != bytes || json.length() != bytes)
        return "{\"available\":false}";
    return json;
}
} // namespace FastBee
