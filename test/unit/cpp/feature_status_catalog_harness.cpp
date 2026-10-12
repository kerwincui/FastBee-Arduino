#define main featureSerializerOriginalMain
#include "feature_status_serializer_harness.cpp"
#undef main
#include "network/FeatureStatusCatalog.h"
#include "network/FeatureStatusResponse.h"
#include <cstdlib>
#include <new>
#include <iostream>

#ifndef HEX
#define HEX 16
#endif
static_assert(HEX == 16, "Exercise the Arduino Print.h HEX macro");

ArduinoJson::Allocator* catalogAllocator = nullptr;
static bool rejectArray = false;
static int arrayBudget = -1;
static std::map<void*, size_t> arrayLive;
static size_t arrayAllocations = 0;
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (rejectArray || arrayBudget == 0) return nullptr;
    if (arrayBudget > 0) --arrayBudget;
    void* memory = std::malloc(size);
    if (memory) { arrayLive[memory] = size; ++arrayAllocations; }
    return memory;
}
void* operator new[](std::size_t size) {
    void* memory = std::malloc(size);
    if (!memory) throw std::bad_alloc();
    arrayLive[memory] = size; ++arrayAllocations;
    return memory;
}
void operator delete[](void* memory) noexcept {
    if (!memory) return;
    assert(arrayLive.erase(memory) == 1);
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept { operator delete[](memory); }

static std::string drain(FastBee::FeatureStatusCatalog& catalog, size_t capacity) {
    std::vector<uint8_t> bytes(capacity);
    std::string body;
    size_t written;
    while ((written = catalog.read(bytes.data(), bytes.size())) != 0)
        body.append(reinterpret_cast<const char*>(bytes.data()), written);
    assert(catalog.read(bytes.data(), bytes.size()) == 0);
    return body;
}

static void expectedStatus(FastBee::RuntimeFeatureManager& manager, JsonDocument& document) {
    document["success"] = true;
    auto data = document["data"].to<JsonObject>();
    manager.getStatusSummaryJson(data["summary"].to<JsonObject>());
    auto features = data["features"].to<JsonArray>();
    for (size_t i = 0; i < manager.featureCount(); ++i)
        assert(manager.getFeatureStatusJson(i, features.add<JsonObject>()));
}

static void snapshotCases() {
    for (unsigned key = 0; key < 8; ++key)
        for (unsigned state = 0; state < 12; ++state)
            for (unsigned observer = 0; observer < 8; ++observer)
                for (size_t slice : {size_t(1), size_t(7), size_t(1436), size_t(4096)}) {
                    FastBee::RuntimeFeatureManager manager;
                    configure(manager, key, state);
                    if (state == 4) {
                        manager.features_[0].meta.displayName = "状态 \"quoted\" \\ \x01\b\n\r\t";
                        std::strcpy(manager.features_[0].runtimeReason, "pressure \" \\ \x02\n\t");
                    }
                    selectObservation(observer);
                    JsonDocument expected;
                    expectedStatus(manager, expected);
                    // The legacy AJ object may link runtimeReason. Freeze its
                    // wire semantics before intentionally mutating live state.
                    std::string expectedBytes;
                    serializeJson(expected, expectedBytes);
                    expected.clear();
                    assert(!deserializeJson(expected, expectedBytes));
                    FastBee::FeatureStatusCatalog catalog(manager);
                    assert(catalog.valid());
                    // The response must retain old reason/intent and observations.
                    for (size_t i = 0; i < manager.count_; ++i) {
                        manager.features_[i].userEnabled = !manager.features_[i].userEnabled;
                        manager.features_[i].runtimeDisabled = !manager.features_[i].runtimeDisabled;
                        std::strcpy(manager.features_[i].runtimeReason, "changed after capture");
                        manager.features_[i].meta.displayName = "new metadata pointer";
                    }
                    selectObservation((observer + 1) % 8);
                    reads.clear();
                    const size_t length = catalog.measureLength();
                    const size_t allocated = arrayAllocations;
                    assert(length > 0 && catalog.measureLength() == length);
                    assert(catalog.read(nullptr, 16) == 0);
                    uint8_t byte;
                    assert(catalog.read(&byte, 0) == 0);
                    const std::string body = drain(catalog, slice);
                    assert(body.size() == length && reads.empty());
                    assert(arrayAllocations == allocated && arrayLive.empty());
                    assert(catalog.measureLength() == length); // EOF never rewinds freed snapshots.
                    JsonDocument actual;
                    assert(!deserializeJson(actual, body));
                    if (actual.as<JsonVariantConst>() != expected.as<JsonVariantConst>()) {
                        std::cerr << "case=" << key << ',' << state << ',' << observer << ',' << slice << '\n';
                        std::cerr << "expected=" << expectedBytes << "\nactual=" << body << '\n';
                        assert(false);
                    }
                }
}

static void failureCases() {
    FastBee::RuntimeFeatureManager manager;
    configure(manager, 0, 4);
    selectObservation(0);
    rejectArray = true;
    { FastBee::FeatureStatusCatalog rejected(manager); assert(!rejected.valid() && !rejected.measureLength()); }
    rejectArray = false;
    arrayBudget = 1;
    { FastBee::FeatureStatusCatalog rejectedReason(manager);
      assert(!rejectedReason.valid() && !rejectedReason.measureLength()); }
    arrayBudget = -1;
    {
        FastBee::FeatureStatusResponse response(manager);
        assert(response._sourceValid() && response._code == 200);
        assert(response._sendContentLength && response._contentType == "application/json");
        const size_t length = response._contentLength;
        size_t written = 0, count;
        uint8_t bytes[7];
        reads.clear();
        while ((count = response._fillBuffer(bytes, sizeof(bytes))) != 0) written += count;
        assert(written == length && reads.empty());
    }
    rejectArray = true;
    { FastBee::FeatureStatusResponse rejected(manager); assert(!rejected._sourceValid()); }
    rejectArray = false;
    // An oversized future immutable descriptor must fail before headers.
    const std::string huge(2000, 'x');
    manager.features_[0].meta.displayName = huge.c_str();
    { FastBee::FeatureStatusCatalog oversized(manager);
      assert(oversized.valid() && !oversized.measureLength() && !oversized.valid()); }
    configure(manager, 0, 4);
    // Complete capture or a visible preflight error, never partial JSON success.
    bool failed = false, completed = false;
    for (unsigned limit = 0; limit < 2048; ++limit) {
        SerializerTraceAllocator allocator(limit);
        catalogAllocator = &allocator;
        {
            FastBee::FeatureStatusCatalog catalog(manager);
            const size_t length = catalog.measureLength();
            if (!catalog.valid()) { failed = true; assert(!length); }
            else {
                completed = true;
                assert(length == drain(catalog, 31).size());
            }
        }
        catalogAllocator = nullptr;
        assert(allocator.live.empty() && arrayLive.empty());
        if (completed) break;
    }
    assert(failed && completed);
    // Disconnect at every boundary is destruction, with no more observer reads.
    for (size_t prefix : {size_t(0), size_t(1), size_t(1536), size_t(3072)}) {
        SerializerTraceAllocator allocator(2048);
        catalogAllocator = &allocator;
        {
            FastBee::FeatureStatusCatalog catalog(manager);
            assert(catalog.measureLength() > 0);
            reads.clear();
            std::vector<uint8_t> bytes(prefix);
            if (prefix) catalog.read(bytes.data(), prefix);
            assert(reads.empty());
        }
        catalogAllocator = nullptr;
        assert(allocator.live.empty() && arrayLive.empty());
    }
    configure(manager, 0, 4);
    const std::string longestReason(95, '\x01');
    std::memcpy(manager.features_[0].runtimeReason, longestReason.c_str(), 96);
    for (size_t i = 3; i < FastBee::RuntimeFeatureManager::kMaxFeatures; ++i)
        manager.features_[i] = manager.features_[0];
    manager.count_ = FastBee::RuntimeFeatureManager::kMaxFeatures;
    {
        JsonDocument expected;
        expectedStatus(manager, expected);
        FastBee::FeatureStatusCatalog full(manager);
        const size_t length = full.measureLength();
        assert(full.valid() && length > 12000);
        JsonDocument actual;
        assert(!deserializeJson(actual, drain(full, 7)));
        assert(actual.as<JsonVariantConst>() == expected.as<JsonVariantConst>());
        assert(arrayLive.empty());
    }
    manager.count_ = 0;
    { FastBee::FeatureStatusCatalog empty(manager); const size_t length = empty.measureLength();
      assert(length > 0 && length == drain(empty, 1).size()); }
    manager.count_ = FastBee::RuntimeFeatureManager::kMaxFeatures + 1;
    { FastBee::FeatureStatusCatalog invalid(manager); assert(!invalid.valid() && !invalid.measureLength()); }
}

int main(int argc, char** argv) {
    // Register the same production observer registry fixtures.
    const char* original[] = {argv[0], "summary"};
    featureSerializerOriginalMain(2, const_cast<char**>(original));
    assert(argc == 2);
    if (std::string(argv[1]) == "snapshot") snapshotCases();
    else if (std::string(argv[1]) == "failure") failureCases();
    else assert(false);
    return 0;
}
