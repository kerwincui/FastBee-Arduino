#pragma once
#include <limits>
#include <stdexcept>

namespace FixedJsonForwardingTest {
// Freeze the previous helper expression as an independent forwarding oracle.
template <size_t N, typename Value>
bool previousSet(JsonObject object, const char (&key)[N], Value&& value) {
    return FastBee::fixedJsonFieldForWrite(object, key).set(std::forward<Value>(value));
}

struct NoncopyableValue {
    static inline unsigned conversions = 0;
    static inline const NoncopyableValue* converted = nullptr;
    explicit NoncopyableValue(int number, bool shouldThrow = false)
        : number(number), shouldThrow(shouldThrow) {}
    NoncopyableValue(const NoncopyableValue&) = delete;
    NoncopyableValue(NoncopyableValue&&) = delete;
    int number;
    bool shouldThrow;
};

inline void convertToJson(const NoncopyableValue& value, JsonVariant output) {
    ++NoncopyableValue::conversions;
    NoncopyableValue::converted = &value;
    if (value.shouldThrow) throw std::runtime_error("converter failure");
    output.set(value.number);
}

template <size_t N, typename Value>
void compareSet(JsonDocument& expected, JsonDocument& actual,
                const char (&key)[N], Value&& value) {
    const bool before = previousSet(expected.as<JsonObject>(), key, std::forward<Value>(value));
    const bool after = FastBee::setFixedJsonField(actual.as<JsonObject>(), key, std::forward<Value>(value));
    assert(before == after);
    std::string beforeBytes, afterBytes;
    serializeJson(expected, beforeBytes);
    serializeJson(actual, afterBytes);
    assert(beforeBytes == afterBytes && expected.overflowed() == actual.overflowed());
}

inline void run() {
    bool reachedComplete = false, reachedFailure = false;
    for (unsigned limit = 0; limit < 128; ++limit) {
        FieldProbeAllocator previousAllocator(limit), actualAllocator(limit);
        JsonDocument expected(&previousAllocator), actual(&actualAllocator);
        expected.to<JsonObject>(); actual.to<JsonObject>();
        const int8_t narrowSigned = -128;
        const uint16_t narrowUnsigned = 65535;
        const int64_t wideSigned = std::numeric_limits<int64_t>::min();
        const uint64_t wideUnsigned = std::numeric_limits<uint64_t>::max();
        const float fraction = -0.125f;
        int mutableNumber = 7;
        compareSet(expected, actual, "a", narrowSigned);
        compareSet(expected, actual, "longerSignedKey", narrowSigned);
        compareSet(expected, actual, "u16", narrowUnsigned);
        compareSet(expected, actual, "i64", wideSigned);
        compareSet(expected, actual, "u64", wideUnsigned);
        compareSet(expected, actual, "float", fraction);
        compareSet(expected, actual, "mutableNumber", mutableNumber);
        mutableNumber = 9;
        compareSet(expected, actual, "mutableNumber", mutableNumber);
        compareSet(expected, actual, "rvalueNumber", int32_t(-90001));
        compareSet(expected, actual, "boolean", false);
        compareSet(expected, actual, "null", nullptr);
        static const char immutable[] = "long static text without decay";
        char mutableText[] = "mutable text must be copied before local storage changes";
        const char* pointer = mutableText;
        compareSet(expected, actual, "constArray", immutable);
        compareSet(expected, actual, "mutableArray", mutableText);
        compareSet(expected, actual, "pointer", pointer);
        compareSet(expected, actual, "ownedTemporary", String(std::string(80, 't')));
        compareSet(expected, actual, "explicitCopy", JsonString(mutableText, false));
        compareSet(expected, actual, "existingLong", std::string(96, 'x'));
        compareSet(expected, actual, "existingLong", std::string(160, 'y'));
        mutableText[0] = '!';
        NoncopyableValue value(37);
        NoncopyableValue::conversions = 0;
        compareSet(expected, actual, "noncopyable", value);
        assert(NoncopyableValue::conversions == 2);
        assert(NoncopyableValue::converted == &value);
        compareSet(expected, actual, "temporaryNoncopyable", NoncopyableValue(39));
        assert(previousAllocator.calls == actualAllocator.calls);
        if (actual.overflowed()) { reachedFailure = true; continue; }
        assert(actual["i64"].as<int64_t>() == wideSigned);
        assert(actual["u64"].as<uint64_t>() == wideUnsigned);
        assert(actual["constArray"].as<JsonString>().isStatic());
        assert(!actual["mutableArray"].as<JsonString>().isStatic());
        assert(actual["mutableArray"] == "mutable text must be copied before local storage changes");
        assert(actual["noncopyable"] == 37 && actual["temporaryNoncopyable"] == 39);
        reachedComplete = true;
        break;
    }
    assert(reachedComplete && reachedFailure);

    JsonDocument expected, actual;
    expected.to<JsonObject>(); actual.to<JsonObject>();
    NoncopyableValue throwing(1, true);
    unsigned caught = 0;
    try { previousSet(expected.as<JsonObject>(), "throwing", throwing); }
    catch (const std::runtime_error& error) { assert(std::string(error.what()) == "converter failure"); ++caught; }
    try { FastBee::setFixedJsonField(actual.as<JsonObject>(), "throwing", throwing); }
    catch (const std::runtime_error& error) { assert(std::string(error.what()) == "converter failure"); ++caught; }
    assert(caught == 2);
    std::string beforeBytes, afterBytes;
    serializeJson(expected, beforeBytes); serializeJson(actual, afterBytes);
    assert(beforeBytes == afterBytes);
    assert(expected.overflowed() == actual.overflowed());
}
} // namespace FixedJsonForwardingTest
