#include "utils/SharedJsonDeserializer.h"

namespace FastBee {
namespace {
struct CallbackReader {
    JsonReadCallbacks callbacks;
    int read() {
        return callbacks.readByte(callbacks.context);
    }
    size_t readBytes(char* output, size_t count) {
        return callbacks.readSpan(callbacks.context, output, count);
    }
};
}

// Share one concrete ArduinoJson parser without LTO recreating it for each
// callback pair. Preserve the existing default parse options and allocator.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
DeserializationError deserializeJsonShared(JsonDocument& destination,
                                           JsonReadCallbacks callbacks) {
    CallbackReader reader{callbacks};
    return deserializeJson(destination, reader);
}
} // namespace FastBee
