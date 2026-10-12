#pragma once
#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Faults affect only ArduinoJson's allocator. Fixture STL/String allocations
// and hardware/transport are outside this boundary.
class DocumentProbeAllocator : public ArduinoJson::Allocator {
public:
    using Event = std::tuple<char, size_t, size_t, unsigned, bool>;
    struct Block { size_t bytes; unsigned id; };
    explicit DocumentProbeAllocator(size_t failAt = std::numeric_limits<size_t>::max(),
                                    bool persistent = false)
        : failAt(failAt), persistent(persistent) {}
    std::vector<Event> events;
    size_t eligible = 0, failures = 0, failAt;
    bool persistent;
    std::map<void*, Block> blocks;
    unsigned nextId = 1;
    ~DocumentProbeAllocator() { assert(blocks.empty()); }
    bool fails() {
        const size_t index = eligible++;
        const bool rejected = index == failAt || (persistent && index >= failAt);
        if (rejected) ++failures;
        return rejected;
    }
    void* allocate(size_t bytes) override {
        const bool rejected = fails();
        if (rejected) { events.emplace_back('A', bytes, 0, 0, false); return nullptr; }
        void* pointer = std::malloc(bytes);
        assert(pointer);
        const unsigned id = nextId++;
        blocks.emplace(pointer, Block{bytes, id});
        events.emplace_back('A', bytes, 0, id, true);
        return pointer;
    }
    void deallocate(void* pointer) override {
        if (!pointer) { events.emplace_back('D', 0, 0, 0, true); return; }
        const auto found = blocks.find(pointer);
        assert(found != blocks.end());
        events.emplace_back('D', found->second.bytes, 0, found->second.id, true);
        blocks.erase(found);
        std::free(pointer);
    }
    void* reallocate(void* pointer, size_t bytes) override {
        auto found = blocks.find(pointer);
        assert(!pointer || found != blocks.end());
        const size_t previous = pointer ? found->second.bytes : 0;
        const unsigned oldId = pointer ? found->second.id : 0;
        // ArduinoJson requires shrink realloc to succeed. Keep the allocation
        // in place but record its logical size, never inject failure here.
        if (pointer && bytes <= previous) {
            found->second.bytes = bytes;
            events.emplace_back('S', bytes, previous, oldId, true);
            return pointer;
        }
        if (fails()) {
            events.emplace_back('R', bytes, previous, oldId, false);
            return nullptr;
        }
        void* replacement = std::realloc(pointer, bytes);
        assert(replacement);
        if (pointer) blocks.erase(found);
        const unsigned id = pointer ? oldId : nextId++;
        blocks.emplace(replacement, Block{bytes, id});
        events.emplace_back('R', bytes, previous, id, true);
        return replacement;
    }
};

inline std::string documentBytes(const JsonDocument& document) {
    std::string result;
    serializeJson(document, result);
    return result;
}

inline void compareVariant(JsonVariantConst left, JsonVariantConst right) {
    assert(left.isNull() == right.isNull());
    assert(left.is<bool>() == right.is<bool>());
    assert(left.is<int64_t>() == right.is<int64_t>());
    assert(left.is<uint64_t>() == right.is<uint64_t>());
    assert(left.is<double>() == right.is<double>());
    assert(left.is<const char*>() == right.is<const char*>());
    assert(left.is<JsonObjectConst>() == right.is<JsonObjectConst>());
    assert(left.is<JsonArrayConst>() == right.is<JsonArrayConst>());
    if (left.is<const char*>()) {
        const JsonString a = left.as<JsonString>(), b = right.as<JsonString>();
        assert(a.size() == b.size() && a.isStatic() == b.isStatic());
        assert(std::memcmp(a.c_str(), b.c_str(), a.size()) == 0);
    }
    if (left.is<JsonObjectConst>()) {
        JsonObjectConst a = left.as<JsonObjectConst>(), b = right.as<JsonObjectConst>();
        assert(a.size() == b.size());
        auto ai = a.begin(), bi = b.begin();
        while (ai != a.end()) {
            assert(ai->key() == bi->key());
            assert(ai->key().isStatic() == bi->key().isStatic());
            compareVariant(ai->value(), bi->value());
            ++ai; ++bi;
        }
    } else if (left.is<JsonArrayConst>()) {
        const auto a = left.as<JsonArrayConst>(), b = right.as<JsonArrayConst>();
        assert(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i) compareVariant(a[i], b[i]);
    }
}

inline void compareDocuments(const JsonDocument& expected, const JsonDocument& actual,
                             const DocumentProbeAllocator& before,
                             const DocumentProbeAllocator& after) {
    assert(documentBytes(expected) == documentBytes(actual));
    assert(expected.overflowed() == actual.overflowed());
    assert(expected.size() == actual.size() && expected.nesting() == actual.nesting());
    compareVariant(expected.as<JsonVariantConst>(), actual.as<JsonVariantConst>());
    assert(before.events == after.events);
    assert(before.eligible == after.eligible && before.failures == after.failures);
}
