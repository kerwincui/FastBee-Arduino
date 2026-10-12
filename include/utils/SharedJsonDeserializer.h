#pragma once
#include <ArduinoJson.h>
#include <cstddef>

namespace FastBee {
// Synchronous borrowed reader. Keep the original reader's state and both
// read operations; no buffering, look-ahead, EOF mapping or retry is added.
struct JsonReadCallbacks {
    void* context;
    int (*readByte)(void*);
    size_t (*readSpan)(void*, char*, size_t);
};

DeserializationError deserializeJsonShared(JsonDocument& destination,
                                           JsonReadCallbacks callbacks);

template <class Reader>
DeserializationError deserializeJsonShared(JsonDocument& destination,
                                           Reader& reader) {
    return deserializeJsonShared(destination, JsonReadCallbacks{
        &reader,
        [](void* context) -> int {
            return static_cast<Reader*>(context)->read();
        },
        [](void* context, char* output, size_t count) -> size_t {
            return static_cast<Reader*>(context)->readBytes(output, count);
        }
    });
}
} // namespace FastBee
