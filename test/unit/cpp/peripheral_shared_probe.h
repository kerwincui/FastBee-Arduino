#pragma once
#include "document_json_probe.h"
#include <ArduinoJson/Variant/JsonVariantVisitor.hpp>
#include <functional>
#include <iostream>
#include <type_traits>

struct SharedShape : ArduinoJson::detail::JsonVariantVisitor<std::string> {
    std::string visit(JsonObjectConst value) {
        std::string out = "object{";
        for (JsonPairConst item : value) {
            out += std::string(item.key().c_str(), item.key().size());
            out += item.key().isStatic() ? ":linked=" : ":copied=";
            out += ArduinoJson::detail::accept(item.value(), *this) + ";";
        }
        return out + "}";
    }
    std::string visit(JsonArrayConst value) {
        std::string out = "array[";
        for (JsonVariantConst item : value) out += ArduinoJson::detail::accept(item, *this) + ";";
        return out + "]";
    }
    std::string visit(JsonString value) { return value.isStatic() ? "linked-text" : "copied-text"; }
    template<class T> std::string visit(const T&) {
        if constexpr (std::is_same<T, bool>::value) return "bool";
        if constexpr (std::is_same<T, std::nullptr_t>::value) return "null";
        if constexpr (std::is_floating_point<T>::value) return "float" + std::to_string(sizeof(T));
        if constexpr (std::is_integral<T>::value) return std::is_signed<T>::value ? "signed" : "unsigned";
        assert(false); return "unsupported";
    }
};
struct SharedSnapshot {
    std::string bytes, shape;
    bool overflow;
    bool operator==(const SharedSnapshot& other) const {
        return bytes == other.bytes && shape == other.shape && overflow == other.overflow;
    }
};
inline SharedSnapshot captureShared(const JsonDocument& document) {
    SharedShape shape;
    return {documentBytes(document), ArduinoJson::detail::accept(document.as<JsonVariantConst>(), shape),
            document.overflowed()};
}
struct SharedResult {
    int status = 0;
    unsigned sends = 0;
    std::string bytes;
    std::vector<SharedSnapshot> documents;
    std::vector<std::string> effects;
    std::vector<DocumentProbeAllocator::Event> events;
    size_t allocations = 0, failures = 0;
    bool operator==(const SharedResult& other) const {
        return status == other.status && sends == other.sends && bytes == other.bytes &&
            documents == other.documents && effects == other.effects && events == other.events &&
            allocations == other.allocations && failures == other.failures;
    }
};
static SharedResult* sharedResult;
static DocumentProbeAllocator* sharedAllocator;
static std::function<void()> poisonSharedInputs;
inline void sharedEffect(const std::string& value) { sharedResult->effects.push_back(value); }
struct SharedDocument : JsonDocument {
    SharedDocument() : JsonDocument(sharedAllocator) { assert(sharedAllocator); }
    ~SharedDocument() {
        auto snapshot = captureShared(*this);
        if (poisonSharedInputs) { poisonSharedInputs(); assert(captureShared(*this) == snapshot); }
        sharedResult->documents.push_back(snapshot);
    }
};
