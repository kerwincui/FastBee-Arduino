#pragma once

#include "core/PeripheralManager.h"
#include <ArduinoJson.h>

// Shared read/status serialization and bounded UART payload size. Kept private
// to the peripheral HTTP routes so extraction does not duplicate these rules.
namespace FastBee {
namespace PeripheralRouteReadDetail {

constexpr size_t UART_API_MAX_BYTES = 128;

inline void addUartRuntime(JsonObject target, const PeripheralRuntimeState& runtime, bool enabled) {
    target["bytesSent"] = runtime.state.comm.bytesSent;
    target["bytesReceived"] = runtime.state.comm.bytesReceived;
    target["connected"] = runtime.state.comm.isConnected;
    // Keep the legacy field, but explicitly scope it to the local UART lease.
    target["localReady"] = enabled && runtime.state.comm.isConnected && !runtime.memorySuspended;
    target["peerVerified"] = false;
    target["counterScope"] = "local_driver";
    target["port"] = runtime.state.comm.port == 255
        ? -1 : static_cast<int>(runtime.state.comm.port);
    target["rxAvailable"] = runtime.state.comm.rxAvailable;
}

inline void addEncoderReading(JsonObject target, const EncoderReading& reading) {
    target["count"] = reading.count;
    target["delta"] = reading.delta;
    target["direction"] = reading.direction;
    target["frequencyHz"] = reading.frequencyHz;
    target["rpm"] = reading.rpm;
}

inline bool isAnalogInputConfigType(PeripheralType type) {
    return type == PeripheralType::ADC || type == PeripheralType::GPIO_ANALOG_INPUT;
}

} // namespace PeripheralRouteReadDetail
} // namespace FastBee
