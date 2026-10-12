#include "document_json_probe.h"
#include "network/FixedJsonField.h"
#include "fixed_json_const_array_before.inc"
#include <iostream>

namespace ConstArrayConverter {
struct Value {
    static inline unsigned calls = 0;
    static inline const Value* identity = nullptr;
    bool shouldThrow;
    explicit Value(bool shouldThrow = false) : shouldThrow(shouldThrow) {}
    Value(const Value&) = delete;
    Value(Value&&) = delete;
};
void convertToJson(const Value& value, JsonVariant output) {
    ++Value::calls;
    Value::identity = &value;
    if (value.shouldThrow) throw std::runtime_error("const-array-unrelated-converter");
    output.set(37);
}
}

constexpr size_t NoFault = std::numeric_limits<size_t>::max();
static const char Padded[64] = "pcf8563";
static const char Embedded[] = {'x', '\0', 'y', 'z', '\0'};
static const char Empty[33] = "";
static const char StaticLong[] = "literal with stable storage and original borrowing";
static const char* const Roots[] = {
    nullptr, "null", "{}", "{\"field\":null,\"retained\":7}",
    "{\"field\":\"old owned content\",\"retained\":{\"child\":17}}",
    "{\"field\":{\"child\":17},\"tail\":[3,4]}",
    "{\"field\":[1,2],\"tail\":7}", "[]", "[1,{\"nested\":2}]",
    "true", "false", "-123", "456", "-0.125", "\"owned scalar root\""
};

struct Triple {
    DocumentProbeAllocator aa, ab, ac;
    JsonDocument direct, before, current;
    bool object;
    Triple(bool object, size_t fault = NoFault, bool persistent = false)
        : aa(fault, persistent), ab(fault, persistent), ac(fault, persistent),
          direct(&aa), before(&ab), current(&ac), object(object) {}
    void check() {
        compareDocuments(direct, before, aa, ab);
        compareDocuments(before, current, ab, ac);
    }
    void seed(size_t root) {
        if (Roots[root]) {
            auto a = deserializeJson(direct, Roots[root]);
            auto b = deserializeJson(before, Roots[root]);
            auto c = deserializeJson(current, Roots[root]);
            assert(a == b && b == c);
        }
        check();
    }
    template<size_t N, typename Value>
    void put(const char (&key)[N], Value&& value) {
        bool a, b, c;
        if (object) {
            a = direct.as<JsonObject>()[key].set(std::forward<Value>(value));
            b = FixedJsonConstArrayBefore::put(before.as<JsonObject>(), key, std::forward<Value>(value));
            c = FastBee::setFixedJsonField(current.as<JsonObject>(), key, std::forward<Value>(value));
        } else {
            a = direct[key].set(std::forward<Value>(value));
            b = FixedJsonConstArrayBefore::put(before, key, std::forward<Value>(value));
            c = FastBee::setFixedJsonField(current, key, std::forward<Value>(value));
        }
        assert(a == b && b == c);
        check();
    }
};

void populate(Triple& triple) {
    triple.put("field", Padded);
    triple.put("embedded", Embedded);
    triple.put("empty", Empty);
    triple.put("long", StaticLong);
    triple.put("literal", "ordinary literal");
    triple.put("rvalueArray", std::move(Padded));
    triple.put("copiedFirst", std::string(160, 'q'));
    triple.put("copiedFirst", Padded);
    triple.put("borrowedFirst", Padded);
    triple.put("borrowedFirst", std::string(170, 'r'));
    char mutableText[] = "mutable array and pointer must retain copied ownership";
    const char* pointer = mutableText;
    char* mutablePointer = mutableText;
    const char* const constantPointer = mutableText;
    triple.put("mutableArray", mutableText);
    triple.put("constPointer", pointer);
    triple.put("mutablePointer", mutablePointer);
    triple.put("constantPointer", constantPointer);
    triple.put("explicitCopy", JsonString(mutableText, false));
    triple.put("explicitLink", JsonString(StaticLong, true));
    triple.put("copiedPadded", JsonString(Padded, false));
    triple.put("copiedEmbedded", JsonString(Embedded, sizeof(Embedded), false));
    triple.put("nullPointer", static_cast<const char*>(nullptr));
    triple.put("arduinoString", String(std::string(140, 's')));
    triple.put("signed", std::numeric_limits<int64_t>::min());
    triple.put("unsigned", std::numeric_limits<uint64_t>::max());
    triple.put("float", -0.125f);
    triple.put("flag", true);
    triple.put("null", nullptr);
    ConstArrayConverter::Value value;
    ConstArrayConverter::Value::calls = 0;
    triple.put("converter", value);
    assert(ConstArrayConverter::Value::calls == 3);
    assert(ConstArrayConverter::Value::identity == &value);
    mutableText[0] = '!';
    triple.check();
    if (!triple.current.overflowed() && triple.current.is<JsonObject>()) {
        const auto field = triple.current["field"].as<JsonString>();
        assert(field.isStatic() && field.c_str() == Padded && field.size() == 7);
        const auto embedded = triple.current["embedded"].as<JsonString>();
        assert(embedded.isStatic() && embedded.c_str() == Embedded && embedded.size() == 1);
        assert(triple.current["empty"].as<JsonString>().size() == 0);
        assert(triple.current["copiedEmbedded"].as<JsonString>().size() == sizeof(Embedded));
        assert(!triple.current["copiedEmbedded"].as<JsonString>().isStatic());
        for (const char* key : {"mutableArray", "constPointer", "mutablePointer", "constantPointer", "explicitCopy"}) {
            assert(triple.current[key] == "mutable array and pointer must retain copied ownership");
            assert(!triple.current[key].as<JsonString>().isStatic());
        }
    }
}

void checkMatrix(bool faults) {
    size_t failed = 0;
    for (bool object : {false, true}) {
        for (size_t root = 0; root < sizeof(Roots) / sizeof(*Roots); ++root) {
            size_t allocations;
            {
                Triple triple(object);
                triple.seed(root);
                const auto seed = documentBytes(triple.current);
                populate(triple);
                assert(!triple.current.overflowed());
                if (root >= 7 || (object && root <= 1))
                    assert(documentBytes(triple.current) == seed);
                allocations = triple.aa.eligible;
            }
            if (!faults) continue;
            for (bool persistent : {false, true}) {
                for (size_t fault = 0; fault < allocations; ++fault) {
                    Triple triple(object, fault, persistent);
                    triple.seed(root);
                    populate(triple);
                    assert(triple.aa.failures > 0);
                    ++failed;
                }
            }
        }
    }
    if (faults) assert(failed > 0);
}

void checkConstView() {
    // Legal mutation of originally mutable storage through its mutable name;
    // binding a const array view must retain the original AJ borrowing policy.
    for (bool object : {false, true}) {
        Triple triple(object);
        triple.seed(2);
        char backing[64] = "sensor-driver-before-return";
        const char (&view)[64] = backing;
        triple.put("view", view);
        triple.put("copy", backing);
        assert(triple.current["view"].as<JsonString>().isStatic());
        assert(triple.current["view"].as<JsonString>().c_str() == backing);
        assert(!triple.current["copy"].as<JsonString>().isStatic());
        backing[0] = '!';
        triple.check();
        assert(triple.current["view"].as<JsonString>().c_str()[0] == '!');
        assert(triple.current["copy"].as<JsonString>().c_str()[0] == 's');
    }
}


void checkDeferredTermination() {
    // All storage is alive and legally mutable. In the non-terminated phase
    // only const-array binding is performed: no String/JsonString conversion,
    // length lookup, serialization, or Triple::check() may read the value.
    // This is valid for the original adapter, which stores the linked pointer
    // without inspecting its contents. Terminate before any read of that value.
    for (bool object : {false, true}) {
        for (bool persistent : {false, true}) {
            for (size_t fault = 0; fault < 4; ++fault) {
                Triple triple(object, fault, persistent);
                triple.seed(2);
                char backing[64];
                std::memset(backing, 'q', sizeof(backing));
                const char (&view)[64] = backing;
                bool a, b, c;
                if (object) {
                    a = triple.direct.as<JsonObject>()["deferred"].set(view);
                    b = FixedJsonConstArrayBefore::put(triple.before.as<JsonObject>(), "deferred", view);
                    c = FastBee::setFixedJsonField(triple.current.as<JsonObject>(), "deferred", view);
                } else {
                    a = triple.direct["deferred"].set(view);
                    b = FixedJsonConstArrayBefore::put(triple.before, "deferred", view);
                    c = FastBee::setFixedJsonField(triple.current, "deferred", view);
                }
                // The original adapter and r2 are not permitted to scan the
                // unterminated array above. All observations start only here.
                backing[7] = '\0';
                assert(a == b && b == c);
                triple.check();
                if (!triple.current.overflowed()) {
                    JsonString value = triple.current["deferred"].as<JsonString>();
                    assert(value.isStatic() && value.c_str() == backing);
                    assert(value.size() == 7);
                }
            }
        }
    }
}

void checkRhsAndExceptions() {
    for (bool object : {false, true}) {
        for (bool persistent : {false, true}) {
            for (size_t fault = 0; fault < 12; ++fault) {
                Triple triple(object, fault, persistent);
                triple.seed(2);
                auto rhs = [](JsonDocument& doc) -> const char (&)[64] {
                    doc["rhsMarker"] = std::string(130, 'm');
                    return Padded;
                };
                if (object) {
                    triple.direct.as<JsonObject>()["rhs"] = rhs(triple.direct);
                    FixedJsonConstArrayBefore::put(triple.before.as<JsonObject>(), "rhs", rhs(triple.before));
                    FastBee::setFixedJsonField(triple.current.as<JsonObject>(), "rhs", rhs(triple.current));
                } else {
                    triple.direct["rhs"] = rhs(triple.direct);
                    FixedJsonConstArrayBefore::put(triple.before, "rhs", rhs(triple.before));
                    FastBee::setFixedJsonField(triple.current, "rhs", rhs(triple.current));
                }
                triple.check();
                if (!triple.current.overflowed()) {
                    auto bytes = documentBytes(triple.current);
                    assert(bytes.find("rhsMarker") < bytes.find("\"rhs\""));
                }
            }
        }
        Triple triple(object);
        triple.seed(2);
        auto throwingRhs = []() -> const char (&)[64] { throw std::runtime_error("rhs-before-member"); };
        unsigned caught = 0;
        if (object) {
            try { triple.direct.as<JsonObject>()["absent"] = throwingRhs(); } catch (const std::runtime_error&) { ++caught; }
            try { FixedJsonConstArrayBefore::put(triple.before.as<JsonObject>(), "absent", throwingRhs()); } catch (const std::runtime_error&) { ++caught; }
            try { FastBee::setFixedJsonField(triple.current.as<JsonObject>(), "absent", throwingRhs()); } catch (const std::runtime_error&) { ++caught; }
        } else {
            try { triple.direct["absent"] = throwingRhs(); } catch (const std::runtime_error&) { ++caught; }
            try { FixedJsonConstArrayBefore::put(triple.before, "absent", throwingRhs()); } catch (const std::runtime_error&) { ++caught; }
            try { FastBee::setFixedJsonField(triple.current, "absent", throwingRhs()); } catch (const std::runtime_error&) { ++caught; }
        }
        assert(caught == 3);
        triple.check();
        assert(triple.current.size() == 0);
        ConstArrayConverter::Value throwingValue(true);
        caught = 0;
        if (object) {
            try { triple.direct.as<JsonObject>()["converter"].set(throwingValue); } catch (const std::runtime_error&) { ++caught; }
            try { FixedJsonConstArrayBefore::put(triple.before.as<JsonObject>(), "converter", throwingValue); } catch (const std::runtime_error&) { ++caught; }
            try { FastBee::setFixedJsonField(triple.current.as<JsonObject>(), "converter", throwingValue); } catch (const std::runtime_error&) { ++caught; }
        } else {
            try { triple.direct["converter"].set(throwingValue); } catch (const std::runtime_error&) { ++caught; }
            try { FixedJsonConstArrayBefore::put(triple.before, "converter", throwingValue); } catch (const std::runtime_error&) { ++caught; }
            try { FastBee::setFixedJsonField(triple.current, "converter", throwingValue); } catch (const std::runtime_error&) { ++caught; }
        }
        assert(caught == 3);
        triple.check();
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "roots-and-values") checkMatrix(false);
    else if (mode == "all-allocation-faults") checkMatrix(true);
    else if (mode == "const-view-ownership") checkConstView();
    else if (mode == "rhs-and-exceptions") checkRhsAndExceptions();
    else if (mode == "deferred-termination") checkDeferredTermination();
    else assert(false);
    std::cout << "PASS " << mode << '\n';
}
