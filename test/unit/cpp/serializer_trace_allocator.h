#pragma once
#include <ArduinoJson.h>
#include <cassert>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

using JsonAllocationTrace = std::vector<std::tuple<char, size_t, bool>>;
class SerializerTraceAllocator : public ArduinoJson::Allocator {
public:
    explicit SerializerTraceAllocator(unsigned limit) : limit(limit) {}
    unsigned limit, calls = 0;
    JsonAllocationTrace trace;
    std::map<void*, size_t> live;
    void* allocate(size_t bytes) override {
        const bool reject = calls++ >= limit;
        trace.emplace_back('a', bytes, reject);
        if (reject) return nullptr;
        void* result = std::malloc(bytes);
        assert(result);
        live[result] = bytes;
        return result;
    }
    void deallocate(void* pointer) override {
        if (!pointer) { trace.emplace_back('f', 0, false); return; }
        auto found = live.find(pointer);
        assert(found != live.end());
        trace.emplace_back('f', found->second, false);
        live.erase(found);
        std::free(pointer);
    }
    void* reallocate(void* pointer, size_t bytes) override {
        const size_t previous = pointer ? live.at(pointer) : 0;
        const bool reject = bytes > previous && calls++ >= limit;
        trace.emplace_back('r', bytes, reject);
        if (reject) return nullptr;
        // Never fail shrink reallocations; ArduinoJson requires them to succeed.
        if (pointer) live.erase(pointer);
        void* result = std::realloc(pointer, bytes);
        assert(result);
        live[result] = bytes;
        return result;
    }
};
