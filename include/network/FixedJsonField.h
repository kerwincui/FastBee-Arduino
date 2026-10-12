#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace FastBee {
// The key must have static lifetime. Existing values are preserved; an absent
// member is created as null. This shared lookup keeps fixed-field writers small.
JsonVariant fixedJsonFieldForWrite(JsonObject object, const char* staticKey);
// Use the document's own member proxy: null roots may become objects while
// array/scalar roots retain ArduinoJson's original rejection semantics.
JsonVariant fixedJsonFieldForWrite(JsonDocument& document, const char* staticKey);
// Only const character arrays select this path. Pointers, mutable arrays and
// explicit JsonString ownership retain ArduinoJson's original converters.
bool setFixedJsonLinkedString(JsonVariant field, const char* value, size_t sourceCapacityMinusOne);

bool setFixedJsonScalar(JsonVariant field, bool value);
bool setFixedJsonScalar(JsonVariant field, int32_t value);
bool setFixedJsonScalar(JsonVariant field, uint32_t value);
#if ARDUINOJSON_USE_LONG_LONG
bool setFixedJsonScalar(JsonVariant field, int64_t value);
bool setFixedJsonScalar(JsonVariant field, uint64_t value);
#endif
bool setFixedJsonScalar(JsonVariant field, float value);
bool setFixedJsonScalar(JsonVariant field, double value);
bool setFixedJsonScalar(JsonVariant field, std::nullptr_t value);

// Arithmetic writes share the actual AJ converter after the lookup. Preserve
// signedness, float width and the original non-arithmetic forwarding path.
// Enums/custom converters and unsupported character types are not coerced.
template <typename Value>
inline bool setFixedJsonValue(JsonVariant field, Value&& value) {
    using Scalar = typename std::remove_cv<typename std::remove_reference<Value>::type>::type;
    if constexpr (std::is_same<Scalar, bool>::value ||
                  std::is_same<Scalar, float>::value ||
                  std::is_same<Scalar, double>::value ||
                  std::is_same<Scalar, std::nullptr_t>::value) {
        return setFixedJsonScalar(field, value);
    } else if constexpr (
            std::is_same<Scalar, signed char>::value || std::is_same<Scalar, unsigned char>::value ||
            std::is_same<Scalar, short>::value || std::is_same<Scalar, unsigned short>::value ||
            std::is_same<Scalar, int>::value || std::is_same<Scalar, unsigned int>::value ||
            std::is_same<Scalar, long>::value || std::is_same<Scalar, unsigned long>::value ||
            std::is_same<Scalar, long long>::value || std::is_same<Scalar, unsigned long long>::value) {
        if constexpr (sizeof(Scalar) <= sizeof(uint32_t)) {
            using Target = typename std::conditional<std::is_signed<Scalar>::value, int32_t, uint32_t>::type;
            return setFixedJsonScalar(field, static_cast<Target>(value));
#if ARDUINOJSON_USE_LONG_LONG
        } else if constexpr (sizeof(Scalar) <= sizeof(uint64_t)) {
            using Target = typename std::conditional<std::is_signed<Scalar>::value, int64_t, uint64_t>::type;
            return setFixedJsonScalar(field, static_cast<Target>(value));
#endif
        } else {
            return field.set(std::forward<Value>(value));
        }
    } else {
        return field.set(std::forward<Value>(value));
    }
}

// Evaluate the RHS before looking up/allocating the member, as proxy assignment
// does. Forwarding preserves mutable arrays (copied), literal arrays (linked),
// pointer values, signedness and JsonString ownership in ArduinoJson::set().
template <size_t N, typename Value>
inline bool setFixedJsonField(JsonObject object, const char (&staticKey)[N], Value&& value) {
    using Source = typename std::remove_reference<Value>::type;
    if constexpr (std::is_array<Source>::value &&
                  std::is_same<typename std::remove_extent<Source>::type, const char>::value) {
        return setFixedJsonLinkedString(fixedJsonFieldForWrite(object, staticKey), value,
                                        std::extent<Source>::value - 1);
    } else {
        return setFixedJsonValue(fixedJsonFieldForWrite(object, staticKey), std::forward<Value>(value));
    }
}

template <size_t N, typename Value>
inline bool setFixedJsonField(JsonDocument& document, const char (&staticKey)[N], Value&& value) {
    using Source = typename std::remove_reference<Value>::type;
    if constexpr (std::is_array<Source>::value &&
                  std::is_same<typename std::remove_extent<Source>::type, const char>::value) {
        return setFixedJsonLinkedString(fixedJsonFieldForWrite(document, staticKey), value,
                                        std::extent<Source>::value - 1);
    } else {
        return setFixedJsonValue(fixedJsonFieldForWrite(document, staticKey), std::forward<Value>(value));
    }
}
// Two fixed-key levels retain the original nested MemberProxy creation/rejection
// rules. Both NUL-terminated keys must have static lifetime.
JsonVariant fixedNestedJsonFieldForWrite(JsonDocument& document,
                                        const char* staticParent,
                                        const char* staticChild);

template <size_t ParentN, size_t ChildN, typename Value>
inline bool setFixedJsonField(JsonDocument& document,
                              const char (&staticParent)[ParentN],
                              const char (&staticChild)[ChildN], Value&& value) {
    using Source = typename std::remove_reference<Value>::type;
    if constexpr (std::is_array<Source>::value &&
                  std::is_same<typename std::remove_extent<Source>::type, const char>::value) {
        return setFixedJsonLinkedString(fixedNestedJsonFieldForWrite(document, staticParent, staticChild), value,
                                        std::extent<Source>::value - 1);
    } else {
        return setFixedJsonValue(fixedNestedJsonFieldForWrite(document, staticParent, staticChild), std::forward<Value>(value));
    }
}

JsonVariant fixedNestedJsonFieldForWrite(JsonDocument& document,
                                        const char* staticParent,
                                        const char* staticBranch,
                                        const char* staticChild);

template <size_t ParentN, size_t BranchN, size_t ChildN, typename Value>
inline bool setFixedJsonField(JsonDocument& document,
                              const char (&staticParent)[ParentN],
                              const char (&staticBranch)[BranchN],
                              const char (&staticChild)[ChildN], Value&& value) {
    using Source = typename std::remove_reference<Value>::type;
    if constexpr (std::is_array<Source>::value &&
                  std::is_same<typename std::remove_extent<Source>::type, const char>::value) {
        return setFixedJsonLinkedString(fixedNestedJsonFieldForWrite(document, staticParent, staticBranch, staticChild), value,
                                        std::extent<Source>::value - 1);
    } else {
        return setFixedJsonValue(fixedNestedJsonFieldForWrite(document, staticParent, staticBranch, staticChild), std::forward<Value>(value));
    }
}

JsonVariant fixedNestedJsonFieldForWrite(JsonObject object,
                                        const char* staticParent,
                                        const char* staticChild);

template <size_t ParentN, size_t ChildN, typename Value>
inline bool setFixedJsonField(JsonObject object,
                              const char (&staticParent)[ParentN],
                              const char (&staticChild)[ChildN], Value&& value) {
    using Source = typename std::remove_reference<Value>::type;
    if constexpr (std::is_array<Source>::value &&
                  std::is_same<typename std::remove_extent<Source>::type, const char>::value) {
        return setFixedJsonLinkedString(fixedNestedJsonFieldForWrite(object, staticParent, staticChild), value,
                                        std::extent<Source>::value - 1);
    } else {
        return setFixedJsonValue(fixedNestedJsonFieldForWrite(object, staticParent, staticChild), std::forward<Value>(value));
    }
}

JsonVariant fixedNestedJsonFieldForWrite(JsonObject object,
                                        const char* staticParent,
                                        const char* staticBranch,
                                        const char* staticChild);

template <size_t ParentN, size_t BranchN, size_t ChildN, typename Value>
inline bool setFixedJsonField(JsonObject object,
                              const char (&staticParent)[ParentN],
                              const char (&staticBranch)[BranchN],
                              const char (&staticChild)[ChildN], Value&& value) {
    using Source = typename std::remove_reference<Value>::type;
    if constexpr (std::is_array<Source>::value &&
                  std::is_same<typename std::remove_extent<Source>::type, const char>::value) {
        return setFixedJsonLinkedString(fixedNestedJsonFieldForWrite(object, staticParent, staticBranch, staticChild), value,
                                        std::extent<Source>::value - 1);
    } else {
        return setFixedJsonValue(fixedNestedJsonFieldForWrite(object, staticParent, staticBranch, staticChild), std::forward<Value>(value));
    }
}
} // namespace FastBee
