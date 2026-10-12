#include "document_json_probe.h"
#include "network/FixedJsonField.h"
#include <iostream>

namespace NestedProbe {
struct Value {
    int number;
    bool throws;
    static inline unsigned conversions = 0;
    static inline const Value* observed = nullptr;
    explicit Value(int n, bool t = false) : number(n), throws(t) {}
    Value(const Value&) = delete;
    Value(Value&&) = delete;
};
inline void convertToJson(const Value& value, JsonVariant target) {
    ++Value::conversions;
    Value::observed = &value;
    if (value.throws) throw std::runtime_error("nested-converter");
    target.set(value.number);
}
struct BoolValue { bool result; };
}
namespace ArduinoJson {
template <> struct Converter<NestedProbe::BoolValue> {
    static bool toJson(const NestedProbe::BoolValue& value, JsonVariant target) {
        target.set(73);
        return value.result;
    }
};
}

static const char ParentKey[] = "parent";
static const char BranchKey[] = "branch";
static const char ChildKey[] = "field";
static const char StaticValue[] = "static string retained without copying";
static const char PaddedValue[64] = "pcf8563";
static const char EmbeddedValue[] = {'x', '\0', 'y', 'z', '\0'};
static const char EmptyValue[33] = "";
constexpr size_t NoFailure = std::numeric_limits<size_t>::max();

template<bool Object> decltype(auto) destination(JsonDocument& doc) {
    if constexpr (Object) return doc.template as<JsonObject>();
    else return (doc);
}
template<unsigned Depth, typename Root, size_t P, size_t C, typename Value>
bool original(Root&& root, const char (&parent)[P], const char (&child)[C], Value&& value) {
    if constexpr (Depth == 2) return root[parent][child].set(std::forward<Value>(value));
    else return root[parent][BranchKey][child].set(std::forward<Value>(value));
}
template<unsigned Depth, typename Root, size_t P, size_t C, typename Value>
bool current(Root&& root, const char (&parent)[P], const char (&child)[C], Value&& value) {
    if constexpr (Depth == 2) return FastBee::setFixedJsonField(root, parent, child, std::forward<Value>(value));
    else return FastBee::setFixedJsonField(root, parent, BranchKey, child, std::forward<Value>(value));
}
template<unsigned Depth, typename Root>
JsonVariant lookup(Root&& root) {
    if constexpr (Depth == 2) return FastBee::fixedNestedJsonFieldForWrite(root, ParentKey, ChildKey);
    else return FastBee::fixedNestedJsonFieldForWrite(root, ParentKey, BranchKey, ChildKey);
}
template<unsigned Depth>
JsonVariant field(JsonDocument& doc, const char* child = ChildKey) {
    if constexpr (Depth == 2) return doc[ParentKey][child].template as<JsonVariant>();
    else return doc[ParentKey][BranchKey][child].template as<JsonVariant>();
}
template<unsigned Depth>
std::vector<std::string> roots() {
    std::vector<std::string> result = {"", "null", "{}", "[]", "[1,2]", "true", "false", "-123", "0.25", "\"owned root\""};
    const char* states[] = {
        "null", "{}", "{\"field\":null,\"retained\":7}",
        "{\"field\":\"old owned child value\",\"retained\":{\"object\":1},\"tail\":[3,4]}",
        "{\"field\":{\"old\":19},\"tail\":[3,4]}", "{\"field\":[1,2],\"tail\":9}",
        "[]", "[1,2]", "true", "false", "-42", "4.25", "\"owned parent\""
    };
    for (const char* state : states) {
        result.push_back(std::string("{\"parent\":") + state + ",\"outside\":11}");
        if constexpr (Depth == 3)
            result.push_back(std::string("{\"parent\":{\"branch\":") + state + ",\"middleSibling\":17},\"outside\":11}");
    }
    return result;
}
template<unsigned Depth, bool Object> struct Pair {
    DocumentProbeAllocator before, after;
    JsonDocument expected, actual;
    Pair(size_t failure = NoFailure, bool persistent = false)
        : before(failure, persistent), after(failure, persistent), expected(&before), actual(&after) {}
    void check() { compareDocuments(expected, actual, before, after); }
    void seed(const std::string& text) {
        if (!text.empty()) assert(deserializeJson(expected, text) == deserializeJson(actual, text));
        check();
    }
    template<size_t P, size_t C, typename Value>
    void put(const char (&parent)[P], const char (&child)[C], Value&& value) {
        const bool a = original<Depth>(destination<Object>(expected), parent, child, std::forward<Value>(value));
        const bool b = current<Depth>(destination<Object>(actual), parent, child, std::forward<Value>(value));
        assert(a == b);
        check();
    }
};

template<unsigned Depth, bool Object> void populate(Pair<Depth, Object>& pair) {
    pair.put(ParentKey, ChildKey, int8_t(-128));
    pair.put(ParentKey, "u16", uint16_t(65535));
    pair.put(ParentKey, "i64", std::numeric_limits<int64_t>::min());
    pair.put(ParentKey, "u64", std::numeric_limits<uint64_t>::max());
    pair.put(ParentKey, "float", -0.125f);
    pair.put(ParentKey, "double", 1.23e-10);
    pair.put(ParentKey, "bool", false);
    pair.put(ParentKey, "null", nullptr);
    const char* nullPointer = nullptr;
    pair.put(ParentKey, "nullPointer", nullPointer);
    pair.put(ParentKey, "linkedArray", StaticValue);
    pair.put(ParentKey, "paddedArray", PaddedValue);
    pair.put(ParentKey, "embeddedArray", EmbeddedValue);
    pair.put(ParentKey, "emptyArray", EmptyValue);
    pair.put(ParentKey, "rvalueArray", std::move(PaddedValue));
    pair.put(ParentKey, "arrayAfterOwned", std::string(140, 'p'));
    pair.put(ParentKey, "arrayAfterOwned", PaddedValue);
    pair.put(ParentKey, "ownedAfterArray", PaddedValue);
    pair.put(ParentKey, "ownedAfterArray", std::string(130, 'o'));
    pair.put(ParentKey, "explicitPaddedCopy", JsonString(PaddedValue, false));
    pair.put(ParentKey, "explicitEmbeddedCopy", JsonString(EmbeddedValue, sizeof(EmbeddedValue), false));
    char mutableText[] = "mutable source copied before buffer mutation";
    const char* pointer = mutableText;
    const char* const constantPointer = mutableText;
    pair.put(ParentKey, "mutableArray", mutableText);
    pair.put(ParentKey, "constPointer", pointer);
    pair.put(ParentKey, "constantPointer", constantPointer);
    pair.put(ParentKey, "mutablePointer", static_cast<char*>(mutableText));
    pair.put(ParentKey, "copiedJsonString", JsonString(mutableText, false));
    pair.put(ParentKey, "linkedJsonString", JsonString(StaticValue, true));
    pair.put(ParentKey, "arduinoString", String(std::string(113, 'q')));
    pair.put(ParentKey, "stdString", std::string(129, 'r'));
    pair.put(ParentKey, "overwrite", std::string(96, 'a'));
    pair.put(ParentKey, "overwrite", std::string(160, 'b'));
    char embeddedNul[] = {'a', '\0', 'b', 'c', 'd', 'e'};
    pair.put(ParentKey, "binary", JsonString(embeddedNul, sizeof(embeddedNul), false));
    NestedProbe::Value value(37);
    NestedProbe::Value::conversions = 0;
    pair.put(ParentKey, "noncopyable", value);
    assert(NestedProbe::Value::conversions == 2 && NestedProbe::Value::observed == &value);
    pair.put(ParentKey, "rvalueNoncopyable", NestedProbe::Value(39));
    pair.put(ParentKey, "converterFalse", NestedProbe::BoolValue{false});
    pair.put(ParentKey, "converterTrue", NestedProbe::BoolValue{true});
    pair.put("", "", 17);
    pair.put("same", "same", 21);
    pair.put("secondParent", "field", 29);
    mutableText[0] = '!';
    embeddedNul[0] = '!';
    pair.check();
    if (!pair.actual.overflowed() && !field<Depth>(pair.actual, "mutableArray").isUnbound()) {
        assert(field<Depth>(pair.actual, "linkedArray").template as<JsonString>().isStatic());
        assert(field<Depth>(pair.actual, "linkedArray").template as<JsonString>().c_str() == StaticValue);
        for (const char* key : {"paddedArray", "rvalueArray", "arrayAfterOwned"}) {
            const auto value = field<Depth>(pair.actual, key).template as<JsonString>();
            assert(value.isStatic() && value.c_str() == PaddedValue && value.size() == 7);
        }
        const auto embedded = field<Depth>(pair.actual, "embeddedArray").template as<JsonString>();
        assert(embedded.isStatic() && embedded.c_str() == EmbeddedValue && embedded.size() == 1);
        const auto empty = field<Depth>(pair.actual, "emptyArray").template as<JsonString>();
        assert(empty.isStatic() && empty.c_str() == EmptyValue && empty.size() == 0);
        assert(!field<Depth>(pair.actual, "explicitPaddedCopy").template as<JsonString>().isStatic());
        const auto copied = field<Depth>(pair.actual, "explicitEmbeddedCopy").template as<JsonString>();
        assert(!copied.isStatic() && copied.size() == sizeof(EmbeddedValue));
        for (const char* key : {"mutableArray", "constPointer", "constantPointer", "mutablePointer", "copiedJsonString"}) {
            const auto value = field<Depth>(pair.actual, key).template as<JsonString>();
            assert(!value.isStatic() && std::strcmp(value.c_str(), "mutable source copied before buffer mutation") == 0);
        }
        assert(field<Depth>(pair.actual, "binary").template as<JsonString>().size() == sizeof(embeddedNul));
        assert(field<Depth>(pair.actual, "binary").template as<JsonString>().c_str()[0] == 'a');
    }
}
template<unsigned Depth, bool Object> void checkRoots(bool faults) {
    size_t failureCases = 0;
    for (const auto& seed : roots<Depth>()) {
        size_t allocations;
        {
            Pair<Depth, Object> reference;
            reference.seed(seed);
            populate(reference);
            assert(!reference.actual.overflowed());
            allocations = reference.before.eligible;
        }
        if (!faults) continue;
        for (bool persistent : {false, true}) for (size_t failure = 0; failure < allocations; ++failure) {
            Pair<Depth, Object> pair(failure, persistent);
            pair.seed(seed);
            populate(pair);
            assert(pair.before.failures > 0);
            ++failureCases;
        }
    }
    if (faults) assert(failureCases > 0);
}

static const char* keyPointer(JsonObjectConst object, const char* key, bool expectedStatic) {
    for (auto item : object) if (item.key() == key) {
        assert(item.key().isStatic() == expectedStatic);
        return item.key().c_str();
    }
    assert(false);
    return nullptr;
}
template<unsigned Depth, bool Object> void checkOwnership() {
    for (bool owned : {false, true}) {
        Pair<Depth, Object> pair;
        pair.seed(owned ? (Depth == 2 ? "{\"parent\":{\"field\":123,\"sibling\":9},\"tail\":7}"
                                     : "{\"parent\":{\"branch\":{\"field\":123,\"sibling\":9},\"other\":4},\"tail\":7}")
                        : "{}");
        const char *oldParent = nullptr, *oldBranch = nullptr, *oldChild = nullptr;
        if (owned) {
            oldParent = keyPointer(pair.actual.template as<JsonObjectConst>(), ParentKey, false);
            if constexpr (Depth == 3)
                oldBranch = keyPointer(pair.actual[ParentKey].template as<JsonObjectConst>(), BranchKey, false);
            auto parent = Depth == 2 ? pair.actual[ParentKey].template as<JsonObjectConst>()
                                     : pair.actual[ParentKey][BranchKey].template as<JsonObjectConst>();
            oldChild = keyPointer(parent, ChildKey, false);
        }
        pair.put(ParentKey, ChildKey, StaticValue);
        assert(keyPointer(pair.actual.template as<JsonObjectConst>(), ParentKey, !owned) == (owned ? oldParent : ParentKey));
        if constexpr (Depth == 3)
            assert(keyPointer(pair.actual[ParentKey].template as<JsonObjectConst>(), BranchKey, !owned) == (owned ? oldBranch : BranchKey));
        auto parent = Depth == 2 ? pair.actual[ParentKey].template as<JsonObjectConst>()
                                 : pair.actual[ParentKey][BranchKey].template as<JsonObjectConst>();
        assert(keyPointer(parent, ChildKey, !owned) == (owned ? oldChild : ChildKey));
        assert(field<Depth>(pair.actual).template as<JsonString>().c_str() == StaticValue);
        const auto oldBytes = documentBytes(pair.actual);
        const auto oldEvents = pair.after.events;
        const auto value = lookup<Depth>(destination<Object>(pair.actual));
        assert(!value.isUnbound() && documentBytes(pair.actual) == oldBytes && pair.after.events == oldEvents);
    }
    for (const auto& seed : roots<Depth>()) {
        Pair<Depth, Object> pair;
        pair.seed(seed);
        const bool existed = !field<Depth>(pair.actual).isUnbound();
        const auto oldBytes = documentBytes(pair.actual);
        const auto oldEvents = pair.after.events;
        auto value = lookup<Depth>(destination<Object>(pair.actual));
        if (existed || value.isUnbound()) {
            assert(documentBytes(pair.actual) == oldBytes);
            assert(pair.after.events == oldEvents);
        } else assert(value.isNull());
    }
}

template<unsigned Depth, typename Root, typename Rhs> void originalAssign(Root&& root, Rhs&& rhs) {
    if constexpr (Depth == 2) root[ParentKey][ChildKey] = rhs();
    else root[ParentKey][BranchKey][ChildKey] = rhs();
}
template<unsigned Depth, typename Root, typename Rhs> void currentAssign(Root&& root, Rhs&& rhs) {
    if constexpr (Depth == 2) FastBee::setFixedJsonField(root, ParentKey, ChildKey, rhs());
    else FastBee::setFixedJsonField(root, ParentKey, BranchKey, ChildKey, rhs());
}
template<unsigned Depth, bool Object> void checkRhs() {
    for (bool persistent : {false, true}) for (size_t failure = 0; failure < 14; ++failure) {
        Pair<Depth, Object> pair;
        pair.seed("{}");
        pair.before.failAt = pair.before.eligible + failure;
        pair.after.failAt = pair.after.eligible + failure;
        pair.before.persistent = pair.after.persistent = persistent;
        unsigned beforeCalls = 0, afterCalls = 0;
        auto rhs = [](JsonDocument& doc, unsigned& calls) {
            ++calls;
            doc["rhsMarker"] = std::string(99, 'm');
            return String(std::string(120, 'r'));
        };
        // Bind the original root handle first; the authorized caller root is a
        // side-effect-free JsonDocument lvalue or an existing JsonObject.
        auto&& expectedRoot = destination<Object>(pair.expected);
        auto&& actualRoot = destination<Object>(pair.actual);
        originalAssign<Depth>(expectedRoot, [&] { return rhs(pair.expected, beforeCalls); });
        currentAssign<Depth>(actualRoot, [&] { return rhs(pair.actual, afterCalls); });
        assert(beforeCalls == 1 && afterCalls == 1);
        pair.check();
        if (!pair.actual.overflowed()) {
            const auto bytes = documentBytes(pair.actual);
            assert(bytes.find("\"rhsMarker\"") < bytes.find("\"parent\""));
        }
    }
    for (const auto& seed : roots<Depth>()) {
        Pair<Depth, Object> pair;
        pair.seed(seed);
        const auto beforeBytes = documentBytes(pair.actual);
        const auto events = pair.after.events;
        unsigned caught = 0;
        auto rhs = []() -> int { throw std::runtime_error("rhs-before-nested"); };
        auto&& a = destination<Object>(pair.expected);
        auto&& b = destination<Object>(pair.actual);
        try { originalAssign<Depth>(a, rhs); } catch (const std::runtime_error&) { ++caught; }
        try { currentAssign<Depth>(b, rhs); } catch (const std::runtime_error&) { ++caught; }
        assert(caught == 2);
        pair.check();
        assert(documentBytes(pair.actual) == beforeBytes && pair.after.events == events);
    }
}
template<unsigned Depth, bool Object> void checkExceptions() {
    for (const auto& seed : roots<Depth>()) {
        Pair<Depth, Object> pair;
        pair.seed(seed);
        const bool existed = !field<Depth>(pair.actual).isUnbound();
        const auto beforeBytes = documentBytes(pair.actual);
        NestedProbe::Value value(43, true);
        unsigned caught = 0;
        try { original<Depth>(destination<Object>(pair.expected), ParentKey, ChildKey, value); }
        catch (const std::runtime_error& e) { assert(std::string(e.what()) == "nested-converter"); ++caught; }
        try { current<Depth>(destination<Object>(pair.actual), ParentKey, ChildKey, value); }
        catch (const std::runtime_error& e) { assert(std::string(e.what()) == "nested-converter"); ++caught; }
        assert(caught == 2);
        pair.check();
        if (existed) assert(documentBytes(pair.actual) == beforeBytes);
    }
}
template<unsigned Depth, bool Object> void checkAliasing() {
    for (bool persistent : {false, true}) for (size_t fault = 0; fault < 16; ++fault) {
        Pair<Depth, Object> pair(fault, persistent);
        pair.seed("{}");
        pair.put(ParentKey, "source", std::string(85, 's'));
        pair.put(ParentKey, "destination", std::string(85, 'd'));
        const bool a = original<Depth>(destination<Object>(pair.expected), ParentKey, "destination", field<Depth>(pair.expected, "source"));
        const bool b = current<Depth>(destination<Object>(pair.actual), ParentKey, "destination", field<Depth>(pair.actual, "source"));
        assert(a == b); pair.check();
        const bool c = original<Depth>(destination<Object>(pair.expected), ParentKey, "source", field<Depth>(pair.expected, "source").template as<JsonVariantConst>());
        const bool d = current<Depth>(destination<Object>(pair.actual), ParentKey, "source", field<Depth>(pair.actual, "source").template as<JsonVariantConst>());
        assert(c == d); pair.check();
    }
}

template<unsigned Depth, bool Object> void checkConstArrayView() {
    Pair<Depth, Object> pair;
    pair.seed("{}");
    char backing[64] = "sensor-before-mutation";
    const char (&view)[64] = backing;
    pair.put(ParentKey, "view", view);
    pair.put(ParentKey, "copy", backing);
    const auto linked = field<Depth>(pair.actual, "view").template as<JsonString>();
    const auto copied = field<Depth>(pair.actual, "copy").template as<JsonString>();
    assert(linked.isStatic() && linked.c_str() == backing);
    assert(!copied.isStatic() && copied.c_str() != backing);
    backing[0] = '!';
    pair.check();
    assert(field<Depth>(pair.actual, "view").template as<JsonString>().c_str()[0] == '!');
    assert(field<Depth>(pair.actual, "copy").template as<JsonString>().c_str()[0] == 's');
}

template<unsigned Depth, bool Object>
size_t deferredTermination(const std::string& seed, size_t fault = NoFailure, bool persistent = false) {
    Pair<Depth, Object> pair(fault, persistent);
    pair.seed(seed);
    char backing[64];
    std::memset(backing, 'q', sizeof(backing));
    const char (&view)[64] = backing;
    // The const-array adapter stores a pointer and extent without inspecting
    // any value bytes. Do not read, measure, serialize or compare until NUL.
    const bool a = original<Depth>(destination<Object>(pair.expected), ParentKey, "deferred", view);
    const bool b = current<Depth>(destination<Object>(pair.actual), ParentKey, "deferred", view);
    backing[7] = '\0';
    assert(a == b);
    pair.check();
    const auto stored = field<Depth>(pair.actual, "deferred");
    if (!pair.actual.overflowed() && !stored.isUnbound()) {
        const auto value = stored.template as<JsonString>();
        assert(value.isStatic() && value.c_str() == backing && value.size() == 7);
    }
    if (fault != NoFailure) assert(pair.before.failures > 0);
    return pair.before.eligible;
}
template<unsigned Depth, bool Object> void checkDeferredTermination() {
    for (const auto& seed : roots<Depth>()) {
        const auto allocations = deferredTermination<Depth, Object>(seed);
        for (bool persistent : {false, true}) for (size_t fault = 0; fault < allocations; ++fault)
            deferredTermination<Depth, Object>(seed, fault, persistent);
    }
}

template<unsigned Depth, bool Object> void checkConstArrayRhs() {
    for (bool persistent : {false, true}) for (size_t fault = 0; fault < 14; ++fault) {
        Pair<Depth, Object> pair;
        pair.seed("{}");
        pair.before.failAt = pair.before.eligible + fault;
        pair.after.failAt = pair.after.eligible + fault;
        pair.before.persistent = pair.after.persistent = persistent;
        unsigned beforeCalls = 0, afterCalls = 0;
        auto rhs = [](JsonDocument& doc, unsigned& calls) -> const char (&)[64] {
            ++calls;
            doc["rhsMarker"] = std::string(99, 'm');
            return PaddedValue;
        };
        auto&& a = destination<Object>(pair.expected);
        auto&& b = destination<Object>(pair.actual);
        originalAssign<Depth>(a, [&]() -> const char (&)[64] { return rhs(pair.expected, beforeCalls); });
        currentAssign<Depth>(b, [&]() -> const char (&)[64] { return rhs(pair.actual, afterCalls); });
        assert(beforeCalls == 1 && afterCalls == 1);
        pair.check();
        if (!pair.actual.overflowed()) {
            const auto bytes = documentBytes(pair.actual);
            assert(bytes.find("\"rhsMarker\"") < bytes.find("\"parent\""));
            const auto value = field<Depth>(pair.actual).template as<JsonString>();
            assert(value.isStatic() && value.c_str() == PaddedValue && value.size() == 7);
        }
    }
    for (const auto& seed : roots<Depth>()) {
        Pair<Depth, Object> pair;
        pair.seed(seed);
        const auto bytes = documentBytes(pair.actual);
        const auto events = pair.after.events;
        unsigned caught = 0;
        auto rhs = []() -> const char (&)[64] { throw std::runtime_error("array-rhs-before-parent"); };
        auto&& a = destination<Object>(pair.expected);
        auto&& b = destination<Object>(pair.actual);
        try { originalAssign<Depth>(a, rhs); } catch (const std::runtime_error&) { ++caught; }
        try { currentAssign<Depth>(b, rhs); } catch (const std::runtime_error&) { ++caught; }
        assert(caught == 2);
        pair.check();
        assert(documentBytes(pair.actual) == bytes && pair.after.events == events);
    }
}

template<unsigned Depth, bool Object> void run(const std::string& mode) {
    if (mode == "roots") checkRoots<Depth, Object>(false);
    else if (mode == "allocation-faults") checkRoots<Depth, Object>(true);
    else if (mode == "ownership-lookup") checkOwnership<Depth, Object>();
    else if (mode == "rhs") checkRhs<Depth, Object>();
    else if (mode == "exceptions") checkExceptions<Depth, Object>();
    else if (mode == "aliasing") checkAliasing<Depth, Object>();
    else if (mode == "const-array-view") checkConstArrayView<Depth, Object>();
    else if (mode == "deferred-termination") checkDeferredTermination<Depth, Object>();
    else if (mode == "const-array-rhs") checkConstArrayRhs<Depth, Object>();
    else assert(false);
}
int main(int argc, char** argv) {
    assert(argc == 4);
    const std::string mode = argv[1], kind = argv[2], depth = argv[3];
    if (kind == "document" && depth == "2") run<2, false>(mode);
    else if (kind == "document" && depth == "3") run<3, false>(mode);
    else if (kind == "object" && depth == "2") run<2, true>(mode);
    else if (kind == "object" && depth == "3") run<3, true>(mode);
    else assert(false);
    std::cout << "PASS " << mode << " " << kind << " " << depth << '\n';
}
