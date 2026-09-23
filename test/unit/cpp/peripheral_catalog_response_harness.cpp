#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/JsonFragmentsResponse.h"
#include "core/PeripheralTypes.h"
#include "core/DriverRegistry.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

class TrackingAllocator : public ArduinoJson::Allocator {
public:
    size_t bytes = 0, peak = 0;
    std::unordered_map<void*, size_t> blocks;
    void* allocate(size_t n) override {
        void* p = malloc(n);
        if (p) { blocks[p] = n; bytes += n; peak = std::max(peak, bytes); }
        return p;
    }
    void deallocate(void* p) override {
        auto it = blocks.find(p);
        if (it != blocks.end()) { bytes -= it->second; blocks.erase(it); }
        free(p);
    }
    void* reallocate(void* p, size_t n) override {
        if (!p) return allocate(n);
        size_t old = blocks.at(p);
        void* next = realloc(p, n);
        if (next) { blocks.erase(p); blocks[next] = n; bytes = bytes - old + n; peak = std::max(peak, bytes); }
        return next;
    }
} catalogAllocator;

static uint32_t freeBytes = 50000, largestBytes = 30000;
namespace FastBee {
namespace MemoryBudget {
uint32_t dramFree() { return freeBytes; }
uint32_t dramLargest() { return largestBytes; }
}
namespace ResourceProfile { constexpr size_t MAX_PERIPHERALS = 12; constexpr const char* NAME = "test"; }
JsonDocument makeJsonDocument() { return JsonDocument(&catalogAllocator); }
class RuntimeFeatureManager {
public:
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager self; return self; }
    bool shouldEnable(const char*) const { return false; }
};
}
#define CHIP_NAME "test-chip"
#define CHIP_MAX_GPIO 39
#define CHIP_MAX_PWM_CH 8
#define CHIP_HAS_DAC 1
#define CHIP_HAS_TOUCH 1

struct AsyncWebServerRequest {
    std::unique_ptr<AsyncAbstractResponse> response;
    int error = 0;
};
struct Context {
    bool authenticated = true;
    bool requireAuth(AsyncWebServerRequest* request) {
        if (!authenticated) request->error = 401;
        return authenticated;
    }
};
class PeripheralManager {
public:
    static PeripheralManager& getInstance() { static PeripheralManager self; return self; }
    int getPeripheralCount() const { return 3; }
    bool isReservedPin(uint8_t pin) const { return pin < 3; }
    bool validatePinForType(uint8_t pin, PeripheralType type, String&) const {
        return pin >= 3 && (pin % 3 == int(type) % 3);
    }
};
namespace HandlerUtils {
bool checkLowMemory(AsyncWebServerRequest* request, size_t minimum) {
    if (freeBytes < minimum || largestBytes < 6144) { request->error = 503; return true; }
    return false;
}
void sendJsonError(AsyncWebServerRequest* request, int code, const char*, int) { request->error = code; }
bool psramCanServeJson() { return false; }
void sendWithClose(AsyncWebServerRequest* request, AsyncAbstractResponse* response) { request->response.reset(response); }
}
class PeripheralRouteHandler {
public:
    Context* ctx;
    void handleGetPeripheralTypes(AsyncWebServerRequest*);
};
#include "catalog_under_test.inc"

// Independent legacy-shaped complete document, using the same production
// descriptor serializers so every nested metadata field participates.
JsonDocument expectedCatalog(ArduinoJson::Allocator* allocator = ArduinoJson::detail::DefaultAllocator::instance()) {
    JsonDocument doc(allocator);
    doc["success"] = true;
    auto profile = doc["profile"].to<JsonObject>();
    profile["name"] = "test"; profile["max"] = 12; profile["used"] = 3; profile["remaining"] = 9;
    auto hardware = doc["hardware"].to<JsonObject>();
    hardware["chip"] = CHIP_NAME; hardware["maxGpio"] = CHIP_MAX_GPIO;
    hardware["pwmChannels"] = CHIP_MAX_PWM_CH; hardware["hasDac"] = true; hardware["hasTouch"] = true;
    const char* pinFields[] = {"inputPins", "outputPins", "adcPins", "dacPins", "touchPins", "reservedPins"};
    const PeripheralType pinTypes[] = {PeripheralType::GPIO_DIGITAL_INPUT, PeripheralType::GPIO_DIGITAL_OUTPUT,
        PeripheralType::ADC, PeripheralType::DAC, PeripheralType::GPIO_TOUCH};
    for (size_t i = 0; i < 6; ++i) {
        auto pins = hardware[pinFields[i]].to<JsonArray>();
        for (uint8_t pin = 0; pin <= CHIP_MAX_GPIO; ++pin) {
            String error;
            if (i == 5 ? PeripheralManager::getInstance().isReservedPin(pin)
                : PeripheralManager::getInstance().validatePinForType(pin, pinTypes[i], error)) pins.add(pin);
        }
    }
    auto data = doc["data"].to<JsonObject>();
    const std::pair<const char*, std::vector<int>> groups[] = {
        {"communication", {1,2,3,4,5}}, {"gpio", {11,12,13,14,15,16,17,18,19,20,21}},
        {"analog", {26,27}}, {"special", {36,37,38,39,40,41,42,43,44,45,47,48,49,50,51,52,53,54,55,56,60}}
    };
    for (const auto& group : groups) {
        auto array = data[group.first].to<JsonArray>();
        for (int value : group.second) {
            auto object = array.add<JsonObject>();
            auto type = static_cast<PeripheralType>(value);
            object["value"] = value; object["name"] = getPeripheralTypeName(type);
            object["pinCount"] = getPeripheralPinCount(type);
            object["implemented"] = isPeripheralTypeImplemented(type);
            addPeripheralTypeDescriptorDetails(object, type);
        }
    }
    return doc;
}

std::string consume(AsyncAbstractResponse& response, size_t step) {
    assert(response._sourceValid());
    std::string output;
    uint8_t buffer[1436];
    while (output.size() < response._contentLength) {
        assert(response._fillBuffer(buffer, 0) == 0);
        const size_t count = response._fillBuffer(buffer, step);
        assert(count && count <= step);
        output.append(reinterpret_cast<char*>(buffer), count);
        response._sentLength += count;
    }
    assert(output.size() == response._contentLength);
    assert(response._fillBuffer(buffer, step) == 0);
    return output;
}

int main() {
    for (const char* name : {"SHT30", "SHT31", "BMP280", "MPU6050", "BH1750", "INA226"}) {
        FastBee::DriverRegistry::getInstance().registerDriver(name, []() -> ISensorDriver* { return nullptr; });
    }
    auto expected = expectedCatalog();
    Context context;
    PeripheralRouteHandler handler{&context};
    for (size_t step : {size_t(1), size_t(7), size_t(256), size_t(768), size_t(1436)}) {
        AsyncWebServerRequest request;
        handler.handleGetPeripheralTypes(&request);
        assert(!request.error && request.response);
        assert(catalogAllocator.bytes == 0); // no JSON tree survives the route
        auto actualBytes = consume(*request.response, step);
        JsonDocument actual;
        assert(!deserializeJson(actual, actualBytes));
        std::string reference; serializeJson(expected, reference);
        assert(actualBytes == reference);
        assert(actual["data"]["special"].size() == 21);
        bool found55 = false, found56 = false;
        const JsonArrayConst special = actual["data"]["special"].as<JsonArrayConst>();
        for (JsonObjectConst item : special) {
            if (item["value"] == 55) { found55 = true; assert(item["driverDescriptors"].size() == 1); }
            if (item["value"] == 56) { found56 = true; assert(item["uiRuntimeCompiled"] == bool(FASTBEE_ENABLE_SCREEN_UI)); }
        }
        assert(found55 && found56);
    }
    const size_t boundedPeak = catalogAllocator.peak;
    catalogAllocator.peak = 0;
    { auto legacy = expectedCatalog(&catalogAllocator); assert(!legacy.overflowed()); }
    const size_t legacyPeak = catalogAllocator.peak;
    assert(catalogAllocator.bytes == 0 && boundedPeak < legacyPeak);

    // Disconnect before completing the body releases fragments and the route
    // owns no dangling references to stack documents or request parameters.
    { AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request); uint8_t b[3];
      assert(request.response->_fillBuffer(b, 3) == 3); }
    context.authenticated = false;
    { AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request); assert(request.error == 401 && !request.response); }
    context.authenticated = true;
    freeBytes = 15000;
    { AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request); assert(request.error == 503 && !request.response); }
    freeBytes = 50000;
    for (int failAfter : {0, 1, 2, 12, 30}) {
        catalogReserveFailure = failAfter;
        AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request);
        assert(request.error == 503 && !request.response && catalogAllocator.bytes == 0);
    }
    catalogReserveFailure = -1;
    catalogConcatFailure = true;
    { AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request); assert(request.error == 503 && !request.response); }
    catalogConcatFailure = false;
    std::cout << "catalog 39 types; JSON equivalence, short buffers, auth, OOM, disconnect passed; host JSON pool peak "
              << boundedPeak << " vs legacy " << legacyPeak << " bytes\n";
}
