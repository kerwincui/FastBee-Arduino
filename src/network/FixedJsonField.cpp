#include "network/FixedJsonField.h"

namespace FastBee {

#if defined(__GNUC__) && !defined(__clang__)
#define FASTBEE_JSON_SCALAR_SHARED __attribute__((noinline, noclone))
#elif defined(__clang__)
#define FASTBEE_JSON_SCALAR_SHARED __attribute__((noinline))
#else
#define FASTBEE_JSON_SCALAR_SHARED
#endif
#define FASTBEE_JSON_SCALAR(Type) \
    FASTBEE_JSON_SCALAR_SHARED bool setFixedJsonScalar(JsonVariant field, Type value) { \
        return field.set(value); \
    }
FASTBEE_JSON_SCALAR(bool)
FASTBEE_JSON_SCALAR(int32_t)
FASTBEE_JSON_SCALAR(uint32_t)
#if ARDUINOJSON_USE_LONG_LONG
FASTBEE_JSON_SCALAR(int64_t)
FASTBEE_JSON_SCALAR(uint64_t)
#endif
FASTBEE_JSON_SCALAR(float)
FASTBEE_JSON_SCALAR(double)
FASTBEE_JSON_SCALAR(std::nullptr_t)
#undef FASTBEE_JSON_SCALAR
#undef FASTBEE_JSON_SCALAR_SHARED
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
bool setFixedJsonLinkedString(JsonVariant field, const char* value, size_t sourceCapacityMinusOne) {
    // Retain the original const-array RamString adapter metadata without an
    // eager strlen. Linked-string display length is determined later by AJ;
    // sourceCapacityMinusOne is not asserted to be the visible JSON length.
    return field.set(JsonString(value, sourceCapacityMinusOne, true));
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
JsonVariant fixedJsonFieldForWrite(JsonObject object, const char* staticKey) {
    // JsonString retains the original literal-key borrowing policy. A plain
    // const char* key would instead allocate/copy it in ArduinoJson 7.4.
    auto member = object[JsonString(staticKey, true)];
    JsonVariant field = member.as<JsonVariant>();
    // to<JsonVariant>() clears a value: use it only to create an absent member.
    return field.isUnbound() ? member.to<JsonVariant>() : field;
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
JsonVariant fixedJsonFieldForWrite(JsonDocument& document, const char* staticKey) {
    auto member = document[JsonString(staticKey, true)];
    JsonVariant field = member.as<JsonVariant>();
    return field.isUnbound() ? member.to<JsonVariant>() : field;
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
JsonVariant fixedNestedJsonFieldForWrite(JsonDocument& document,
                                        const char* staticParent,
                                        const char* staticChild) {
    auto member = document[JsonString(staticParent, true)][JsonString(staticChild, true)];
    JsonVariant field = member.as<JsonVariant>();
    // Only an unbound child may be created/cleared. The complete original proxy
    // retains parent creation, scalar/array rejection and allocation order.
    return field.isUnbound() ? member.to<JsonVariant>() : field;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
JsonVariant fixedNestedJsonFieldForWrite(JsonDocument& document,
                                        const char* staticParent,
                                        const char* staticBranch,
                                        const char* staticChild) {
    auto member = document[JsonString(staticParent, true)]
                          [JsonString(staticBranch, true)][JsonString(staticChild, true)];
    JsonVariant field = member.as<JsonVariant>();
    return field.isUnbound() ? member.to<JsonVariant>() : field;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
JsonVariant fixedNestedJsonFieldForWrite(JsonObject object,
                                        const char* staticParent,
                                        const char* staticChild) {
    auto member = object[JsonString(staticParent, true)][JsonString(staticChild, true)];
    JsonVariant field = member.as<JsonVariant>();
    // Only an unbound child may be created/cleared. The complete original proxy
    // retains parent creation, scalar/array rejection and allocation order.
    return field.isUnbound() ? member.to<JsonVariant>() : field;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
JsonVariant fixedNestedJsonFieldForWrite(JsonObject object,
                                        const char* staticParent,
                                        const char* staticBranch,
                                        const char* staticChild) {
    auto member = object[JsonString(staticParent, true)]
                          [JsonString(staticBranch, true)][JsonString(staticChild, true)];
    JsonVariant field = member.as<JsonVariant>();
    return field.isUnbound() ? member.to<JsonVariant>() : field;
}
} // namespace FastBee
