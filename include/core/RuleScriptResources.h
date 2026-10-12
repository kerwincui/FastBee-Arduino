#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <initializer_list>
#include <cstdlib>
#include <new>
#include <utility>
#include "core/MemoryBudget.h"

namespace FastBee {
// Match the existing optional-feature reserve. PSRAM never substitutes for
// the internal byte-addressable heap used by WiFi and FreeRTOS.
inline bool ruleScriptMemoryAvailable(size_t bytes, size_t largestAllocation) {
    const size_t free = MemoryBudget::dram8Free();
    const size_t largest = MemoryBudget::dram8Largest();
    return free >= 25600 && bytes <= free - 25600 &&
        largest >= 10000 && largestAllocation <= largest - 10000;
}

class RuleScriptJsonAllocator final : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        return ruleScriptMemoryAvailable(size + 16, size + 16) ? std::malloc(size) : nullptr;
    }
    void deallocate(void* pointer) override { std::free(pointer); }
    void* reallocate(void* pointer, size_t size) override {
        // Conservatively allow the entire replacement beside the old block.
        return ruleScriptMemoryAvailable(size + 16, size + 16) ? std::realloc(pointer, size) : nullptr;
    }
};

// A bounded sorted map with checked backing allocation. This avoids the
// throwing STL node allocator on ESP builds without exceptions. All backing
// and strings belong to the instance and are released on retirement.
template<class Text, class Rule>
class RuleScriptMap {
public:
    using value_type = std::pair<Text, Rule>;
    using iterator = value_type*;
    using const_iterator = const value_type*;
    RuleScriptMap() = default;
    RuleScriptMap(const RuleScriptMap&) = delete;
    RuleScriptMap& operator=(const RuleScriptMap&) = delete;
    ~RuleScriptMap() { clear(); }
    bool reserve(size_t count) {
        if (_size || _rows || count > 64) return false;
        if (!count) return true;
        const size_t bytes = count * sizeof(value_type);
        if (!ruleScriptMemoryAvailable(bytes + 16, bytes + 16)) return false;
        static_assert(alignof(value_type) <= alignof(std::max_align_t), "malloc alignment insufficient");
        _rows = static_cast<value_type*>(std::malloc(bytes));
        if (!_rows) return false;
        _capacity = count;
        return true;
    }
    void clear() {
        for (size_t i = 0; i < _size; ++i) _rows[i].~value_type();
        std::free(_rows);
        _rows = nullptr; _size = _capacity = 0;
    }
    size_t size() const { return _size; }
    bool empty() const { return !_size; }
    iterator begin() { return _rows; }
    iterator end() { return _rows ? _rows + _size : nullptr; }
    const_iterator begin() const { return _rows; }
    const_iterator end() const { return _rows ? _rows + _size : nullptr; }
    iterator find(const Text& id) {
        for (size_t i = 0; i < _size; ++i) if (_rows[i].first == id) return _rows + i;
        return end();
    }
    const_iterator find(const Text& id) const {
        for (size_t i = 0; i < _size; ++i) if (_rows[i].first == id) return _rows + i;
        return end();
    }
    size_t count(const Text& id) const { return find(id) != end(); }
    std::pair<iterator, bool> emplace(Text&& id, Rule&& rule) {
        auto existing = find(id);
        if (existing != end()) return {existing, false};
        if (_size == _capacity) return {end(), false};
        size_t position = 0;
        while (position < _size && _rows[position].first < id) ++position;
        if (position < _size) {
            new (_rows + _size) value_type(std::move(_rows[_size - 1]));
            for (size_t i = _size - 1; i > position; --i) _rows[i] = std::move(_rows[i - 1]);
            _rows[position] = value_type(std::move(id), std::move(rule));
        } else new (_rows + _size) value_type(std::move(id), std::move(rule));
        ++_size;
        return {_rows + position, true};
    }
    void swap(RuleScriptMap& other) {
        std::swap(_rows, other._rows); std::swap(_size, other._size); std::swap(_capacity, other._capacity);
    }
private:
    value_type* _rows = nullptr;
    size_t _size = 0, _capacity = 0;
};

// Include rounded String capacities and allocator bookkeeping, rather than
// treating serialized file size as resident memory. JSON slots are counted
// recursively, including member names. Existing documents/maps are already
// reflected in the actual free internal heap snapshot.
inline size_t ruleScriptJsonMemory(JsonVariantConst value) {
    size_t bytes = 64;
    if (value.is<JsonString>()) bytes += ((value.as<JsonString>().size() + 16) & ~size_t(15)) + 32;
    else if (value.is<JsonObjectConst>()) for (JsonPairConst pair : value.as<JsonObjectConst>())
        bytes += ((pair.key().size() + 16) & ~size_t(15)) + 32 + ruleScriptJsonMemory(pair.value());
    else if (value.is<JsonArrayConst>()) for (JsonVariantConst row : value.as<JsonArrayConst>())
        bytes += ruleScriptJsonMemory(row);
    return bytes;
}
template<class Map>
bool ruleScriptCandidateMemory(JsonVariantConst root, bool diskCommit) {
    const auto rows = root["rules"].as<JsonArrayConst>();
    if (rows.size() > 64) return false;
    size_t strings = 0, largestText = 0;
    for (JsonVariantConst row : rows) for (const char* field : {"id", "id", "name", "scriptContent", "sourceTopic", "targetTopic"}) {
        const size_t bytes = ((row[field].as<JsonString>().size() + 16) & ~size_t(15)) + 16;
        strings += bytes;
        if (bytes > largestText) largestText = bytes;
    }
    const size_t backing = rows.size() * sizeof(typename Map::value_type) + 16;
    const size_t json = diskCommit ? 2048 + 2 * ruleScriptJsonMemory(root) : 0;
    // Save verifies temporary files and atomic replace generations. Allow two
    // DOMs beside the candidate plus bounded filesystem/cache work buffers.
    const size_t largest = backing > largestText ? backing : largestText;
    return ruleScriptMemoryAvailable(backing + strings + json + 2048,
        diskCommit && largest < 2048 ? 2048 : largest);
}
} // namespace FastBee
