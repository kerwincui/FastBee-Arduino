#include "utils/SharedJsonSerializer.h"

namespace FastBee {
namespace {
struct CallbackWriter {
    JsonWriteCallbacks callbacks;
    size_t write(uint8_t byte) {
        return callbacks.writeByte(callbacks.context, byte);
    }
    size_t write(const uint8_t* bytes, size_t size) {
        return callbacks.writeSpan(callbacks.context, bytes, size);
    }
};
}

// One compiled ArduinoJson writer/formatter/visitor instantiation. Do not let
// LTO specialize it for individual sink callbacks and recreate the same code.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
size_t serializeJsonShared(JsonVariantConst source, JsonWriteCallbacks callbacks) {
    CallbackWriter writer{callbacks};
    // No buffering, allocation, failure latch, retry or output transformation.
    // ArduinoJson receives every original write result and continues normally.
    return serializeJson(source, writer);
}
} // namespace FastBee
