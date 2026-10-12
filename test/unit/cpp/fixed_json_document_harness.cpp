#include "document_json_probe.h"
#include "network/FixedJsonField.h"
#include <iostream>

namespace DocumentValueProbe {
struct Noncopyable {
    int value;
    bool shouldThrow;
    static inline unsigned conversions = 0;
    static inline const Noncopyable* observed = nullptr;
    explicit Noncopyable(int value, bool shouldThrow = false)
        : value(value), shouldThrow(shouldThrow) {}
    Noncopyable(const Noncopyable&) = delete;
    Noncopyable(Noncopyable&&) = delete;
};
void convertToJson(const Noncopyable& source, JsonVariant output) {
    ++Noncopyable::conversions;
    Noncopyable::observed = &source;
    if (source.shouldThrow) throw std::runtime_error("document-converter");
    output.set(source.value);
}
struct BoolResult { bool result; };
}
namespace ArduinoJson {
template <> struct Converter<DocumentValueProbe::BoolResult> {
    static bool toJson(const DocumentValueProbe::BoolResult& value, JsonVariant output) {
        output.set(17);
        return value.result;
    }
};
}

constexpr size_t NoFailure = std::numeric_limits<size_t>::max();
static const char BorrowedKey[] = "borrowedStaticDocumentKey";
static const char BorrowedText[] = "borrowed static value with retained storage";
static const char* const Roots[] = {
    nullptr, "null", "{}", "{\"field\":null,\"retained\":7}",
    "{\"field\":\"old owned value\",\"retained\":{\"child\":17},\"tail\":[3,4]}",
    "{\"field\":{\"child\":17},\"tail\":[3,4]}",
    "{\"field\":[1,2],\"tail\":7}", "[]", "[1,{\"nested\":2}]",
    "true", "false", "-123", "456", "-0.125", "\"owned scalar root value\""
};

struct Pair {
    DocumentProbeAllocator before, after;
    JsonDocument expected, actual;
    explicit Pair(size_t failAt = NoFailure, bool persistent = false)
        : before(failAt, persistent), after(failAt, persistent),
          expected(&before), actual(&after) {}
    void check() { compareDocuments(expected, actual, before, after); }
    template<size_t N, typename Value>
    void put(const char (&key)[N], Value&& value) {
        // Independent oracle is the actual document member proxy. No helper,
        // root cast, or pre-created member is used by the reference.
        const bool was = expected[key].set(std::forward<Value>(value));
        const bool now = FastBee::setFixedJsonField(actual, key, std::forward<Value>(value));
        assert(was == now);
        check();
    }
    void seed(size_t index) {
        if (Roots[index]) {
            auto a = deserializeJson(expected, Roots[index]);
            auto b = deserializeJson(actual, Roots[index]);
            assert(a == b);
        }
        check();
    }
};

void populate(Pair& pair) {
    pair.put("field", int8_t(-128));
    pair.put("u16", uint16_t(65535));
    pair.put("i64", std::numeric_limits<int64_t>::min());
    pair.put("u64", std::numeric_limits<uint64_t>::max());
    pair.put("float", float(-0.125));
    pair.put("double", 0.000000000123);
    pair.put("flag", true);
    pair.put("null", nullptr);
    const char* nullPointer = nullptr;
    pair.put("nullPointer", nullPointer);
    pair.put(BorrowedKey, BorrowedText);
    char owned[] = "mutable char array must be copied before mutation";
    const char* pointer = owned;
    pair.put("mutableArray", owned);
    pair.put("constPointer", pointer);
    pair.put("mutablePointer", static_cast<char*>(owned));
    pair.put("copiedJsonString", JsonString(owned, false));
    pair.put("linkedJsonString", JsonString(BorrowedText, true));
    pair.put("arduinoString", String(std::string(113, 'q')));
    pair.put("stdString", std::string(129, 'r'));
    pair.put("overwrite", std::string(96, 'a'));
    pair.put("overwrite", std::string(160, 'b'));
    char nulValue[] = {'a', '\0', 'b', 'c', 'd', 'e'};
    pair.put("nulBytes", JsonString(nulValue, sizeof(nulValue), false));
    DocumentValueProbe::Noncopyable value(37);
    DocumentValueProbe::Noncopyable::conversions = 0;
    pair.put("noncopyable", value);
    assert(DocumentValueProbe::Noncopyable::conversions == 2);
    assert(DocumentValueProbe::Noncopyable::observed == &value);
    pair.put("rvalueNoncopyable", DocumentValueProbe::Noncopyable(39));
    pair.put("converterFalse", DocumentValueProbe::BoolResult{false});
    pair.put("converterTrue", DocumentValueProbe::BoolResult{true});
    owned[0] = '!';
    nulValue[0] = '!';
    pair.check();
    if (!pair.actual.overflowed() && pair.actual.is<JsonObject>()) {
        assert(pair.actual["i64"].as<int64_t>() == std::numeric_limits<int64_t>::min());
        assert(pair.actual["u64"].as<uint64_t>() == std::numeric_limits<uint64_t>::max());
        assert(pair.actual[BorrowedKey].as<JsonString>().c_str() == BorrowedText);
        assert(pair.actual[BorrowedKey].as<JsonString>().isStatic());
        for (JsonPair item : pair.actual.as<JsonObject>()) {
            if (item.key() == BorrowedKey)
                assert(item.key().isStatic() && item.key().c_str() == BorrowedKey);
        }
        for (const char* key : {"mutableArray", "constPointer", "mutablePointer", "copiedJsonString"}) {
            assert(pair.actual[key] == "mutable char array must be copied before mutation");
            assert(!pair.actual[key].as<JsonString>().isStatic());
        }
        assert(pair.actual["nulBytes"].as<JsonString>().size() == sizeof(nulValue));
        assert(pair.actual["nulBytes"].as<JsonString>().c_str()[0] == 'a');
    }
}

void checkRoots(bool inject) {
    size_t verifiedFailures = 0;
    for (size_t root = 0; root < sizeof(Roots) / sizeof(*Roots); ++root) {
        size_t allocationCount = 0;
        {
            Pair reference;
            reference.seed(root);
            const auto original = documentBytes(reference.actual);
            populate(reference);
            assert(!reference.actual.overflowed());
            if (root >= 7) assert(documentBytes(reference.actual) == original);
            allocationCount = reference.before.eligible;
        }
        if (!inject) continue;
        for (bool persistent : {false, true}) {
            for (size_t fault = 0; fault < allocationCount; ++fault) {
                Pair pair(fault, persistent);
                pair.seed(root);
                populate(pair);
                assert(pair.before.failures > 0);
                ++verifiedFailures;
            }
        }
    }
    if (inject) assert(verifiedFailures > 0);
}

void checkLookup() {
    for (size_t root = 0; root < sizeof(Roots) / sizeof(*Roots); ++root) {
        DocumentProbeAllocator allocator;
        JsonDocument document(&allocator);
        if (Roots[root]) assert(!deserializeJson(document, Roots[root]));
        const auto original = documentBytes(document);
        const auto oldField = document["field"].as<JsonVariant>();
        const bool existed = !oldField.isUnbound();
        const bool objectOrNull = document.is<JsonObject>() || document.isNull();
        const auto events = allocator.events;
        JsonVariant field = FastBee::fixedJsonFieldForWrite(document, "field");
        assert(field.isUnbound() == !objectOrNull);
        if (existed || !objectOrNull) {
            assert(documentBytes(document) == original);
            assert(allocator.events == events);
        } else {
            assert(!field.isUnbound() && field.isNull());
            assert(document.is<JsonObject>());
        }
        if (!objectOrNull) {
            // AJ void converters retain a ResourceManager even when the
            // member is unbound. Preserve their exact bool result too.
            assert(!field.set(1));
            assert(field.set(nullptr));
            assert(documentBytes(document) == original);
        }
    }
}

void checkRhs() {
    for (bool persistent : {false, true}) {
        for (size_t fault = 0; fault < 12; ++fault) {
            Pair pair(fault);
            pair.before.persistent = pair.after.persistent = persistent;
            auto rhs = [](JsonDocument& document) {
                document["rhsMarker"] = std::string(99, 'm');
                return String(std::string(120, 'r'));
            };
            pair.expected["rhsValue"] = rhs(pair.expected);
            FastBee::setFixedJsonField(pair.actual, "rhsValue", rhs(pair.actual));
            pair.check();
            if (!pair.actual.overflowed()) {
                const auto bytes = documentBytes(pair.actual);
                assert(bytes.find("\"rhsMarker\"") < bytes.find("\"rhsValue\""));
            }
        }
    }
    Pair throwing;
    auto rhs = []() -> int { throw std::runtime_error("rhs-before-member"); };
    unsigned caught = 0;
    try { throwing.expected["absent"] = rhs(); }
    catch (const std::runtime_error&) { ++caught; }
    try { FastBee::setFixedJsonField(throwing.actual, "absent", rhs()); }
    catch (const std::runtime_error&) { ++caught; }
    assert(caught == 2);
    throwing.check();
    assert(throwing.actual.isNull() && throwing.after.events.empty());
}

void checkExceptions() {
    for (size_t root = 0; root < sizeof(Roots) / sizeof(*Roots); ++root) {
        Pair pair;
        pair.seed(root);
        DocumentValueProbe::Noncopyable value(43, true);
        unsigned caught = 0;
        const auto original = documentBytes(pair.actual);
        const bool existed = !pair.actual["field"].as<JsonVariant>().isUnbound();
        try { pair.expected["field"].set(value); }
        catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == "document-converter"); ++caught;
        }
        try { FastBee::setFixedJsonField(pair.actual, "field", value); }
        catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == "document-converter"); ++caught;
        }
        assert(caught == 2);
        pair.check();
        if (existed || root >= 7) assert(documentBytes(pair.actual) == original);
    }
}

void checkAliasing() {
    for (size_t fault = 0; fault < 12; ++fault) {
        Pair pair(fault);
        pair.put("source", std::string(85, 's'));
        pair.put("destination", std::string(85, 'd'));
        const bool a = pair.expected["destination"].set(pair.expected["source"].as<JsonVariant>());
        const bool b = FastBee::setFixedJsonField(pair.actual, "destination", pair.actual["source"].as<JsonVariant>());
        assert(a == b); pair.check();
        const bool c = pair.expected["source"].set(pair.expected["source"].as<JsonVariantConst>());
        const bool d = FastBee::setFixedJsonField(pair.actual, "source", pair.actual["source"].as<JsonVariantConst>());
        assert(c == d); pair.check();
    }
}

#include "fixed_json_scalar_cases.inc"

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "roots") checkRoots(false);
    else if (mode == "allocation-faults") checkRoots(true);
    else if (mode == "lookup") checkLookup();
    else if (mode == "rhs") checkRhs();
    else if (mode == "exceptions") checkExceptions();
    else if (mode == "aliasing") checkAliasing();
    else if (mode == "scalar-widths") checkScalarWidths(false);
    else if (mode == "scalar-allocation-faults") checkScalarWidths(true);
    else assert(false);
    std::cout << "PASS " << mode << '\n';
}
