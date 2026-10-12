#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

namespace FastBee {
// Synchronous borrowed sink: the source and context must outlive this call.
// Keep both overloads distinct; sinks can have different byte/span semantics.
struct JsonWriteCallbacks {
    void* context;
    size_t (*writeByte)(void*, uint8_t);
    size_t (*writeSpan)(void*, const uint8_t*, size_t);
};

size_t serializeJsonShared(JsonVariantConst source, JsonWriteCallbacks callbacks);

template <class Writer>
size_t serializeJsonShared(JsonVariantConst source, Writer& writer) {
    return serializeJsonShared(source, JsonWriteCallbacks{
        &writer,
        [](void* context, uint8_t byte) -> size_t {
            return static_cast<Writer*>(context)->write(byte);
        },
        [](void* context, const uint8_t* bytes, size_t size) -> size_t {
            return static_cast<Writer*>(context)->write(bytes, size);
        }
    });
}
} // namespace FastBee
