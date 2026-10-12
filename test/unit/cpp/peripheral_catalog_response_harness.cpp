#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/JsonFragmentsResponse.h"
#include "network/FixedJsonField.h"
#include "core/PeripheralTypes.h"
#include "core/DriverRegistry.h"
#include "core/H1ParameterJson.h"
#include "network/ThermostatConfigJson.h"
#include "network/SdRecorderConfigJson.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include "peripheral_shared_probe.h"

class TrackingAllocator : public ArduinoJson::Allocator {
public:
    size_t bytes = 0, peak = 0;
    bool failAllocations = false;
    std::unordered_map<void*, size_t> blocks;
    void* allocate(size_t n) override {
        if (failAllocations) return nullptr;
        void* p = malloc(n);
        if (p) { blocks[p] = n; bytes += n; peak = std::max(peak, bytes); catalogJsonBytes = bytes; catalogTrackMemory(); }
        return p;
    }
    void deallocate(void* p) override {
        auto it = blocks.find(p);
        if (it != blocks.end()) { bytes -= it->second; blocks.erase(it); }
        catalogJsonBytes = bytes;
        free(p);
    }
    void* reallocate(void* p, size_t n) override {
        if (failAllocations) return nullptr;
        if (!p) return allocate(n);
        size_t old = blocks.at(p);
        void* next = realloc(p, n);
        if (next) { blocks.erase(p); blocks[next] = n; bytes = bytes - old + n; peak = std::max(peak, bytes); catalogJsonBytes = bytes; catalogTrackMemory(); }
        return next;
    }
} catalogAllocator;

static uint32_t freeBytes = 50000, largestBytes = 30000;
static bool accountCatalogStorage = false;
namespace FastBee {
namespace MemoryBudget {
uint32_t dramFree() {
    return !accountCatalogStorage ? freeBytes :
        static_cast<uint32_t>(freeBytes - std::min<size_t>(freeBytes,
            catalogReservedBytes + catalogAllocator.bytes + 42 * 16));
}
uint32_t dramLargest() { return largestBytes; }
}
namespace ResourceProfile { constexpr size_t MAX_PERIPHERALS = 12; constexpr const char* NAME = "test"; }
JsonDocument makeJsonDocument() { return JsonDocument(&catalogAllocator); }
class RuntimeFeatureManager {
public:
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager self; return self; }
    bool enabled = false;
    bool shouldEnable(const char*) const { return enabled; }
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
        {"analog", {26,27}}, {"special", {36,37,38,39,40,41,42,43,44,45,47,48,49,50,51,52,53,54,55,56,57,60}}
    };
    for (const auto& group : groups) {
        auto array = data[group.first].to<JsonArray>();
        for (int value : group.second) {
            auto object = array.add<JsonObject>();
            auto type = static_cast<PeripheralType>(value);
            object["value"] = value; object["name"] = getPeripheralTypeName(type);
            object["pinCount"] = getPeripheralPinCount(type);
            object["implemented"] = isPeripheralTypeImplemented(type);
            CatalogOracle::addPeripheralTypeDescriptorDetails(object, type);
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

// Frozen independent screen schema: do not derive the expected values from the
// production serializer, or table omissions could pass route self-equivalence.
void checkScreenDescriptor() {
    const std::string expected =
        R"json({"minPinCount":0,"maxPinCount":0,"usesNamedPins":true,"featureKey":"screenPanel","uiRuntimeCompiled":)json"
#if FASTBEE_ENABLE_SCREEN_UI
        "true"
#else
        "false"
#endif
        R"json(,"driverOptions":[{"backend":"spi_tft","driver":"st7789","label":"ST7789 SPI TFT","minWidth":128,"maxWidth":320,"minHeight":128,"maxHeight":320},{"backend":"spi_tft","driver":"ili9341","label":"ILI9341 SPI TFT","minWidth":128,"maxWidth":320,"minHeight":128,"maxHeight":320})json"
#if CHIP_IS_ESP32S3
        R"json(,{"backend":"rgb","driver":"st7701","label":"ST7701 RGB565 (type1)","minWidth":480,"maxWidth":480,"minHeight":480,"maxHeight":480,"rgbInitProfile":"st7701_type1","requiresPsram":true})json"
#endif
        R"json(],"touchOptions":[{"backend":"none","label":"No touch"},{"backend":"gt911","label":"GT911 I2C","addresses":[93,20]}],"requiredNamedPins":["mosi","sclk","cs","dc"],"optionalNamedPins":["rst","backlight","touchSda","touchScl","touchInt","touchRst"],"capabilities":{"colorDepth":16,"partialFlush":true,"brightness":true,"sleep":true,"touch":true,"singleActiveDisplay":true},"configLimits":{"widthMin":128,"widthMax":320,"heightMin":128,"heightMax":320,"rotationMin":0,"rotationMax":3,"colorDepth":16,"bufferLinesMin":1,"bufferLinesMax":64,"brightnessMin":0,"brightnessMax":100,"spiFrequencyMin":1000000,"spiFrequencyMax":80000000,"rgbFrequencyMin":1000000,"rgbFrequencyMax":16000000,"i2cFrequencies":[100000,400000]}})json";
    JsonDocument doc;
    addScreenDisplayDescriptor(doc.to<JsonObject>());
    std::string actual;
    serializeJson(doc, actual);
    assert(!doc.overflowed() && actual == expected);
    assert(doc["usesNamedPins"].is<bool>() && doc["uiRuntimeCompiled"].is<bool>());
    const JsonObjectConst limits = doc["configLimits"].as<JsonObjectConst>();
    for (JsonPairConst field : limits) {
        if (field.key() == "i2cFrequencies") continue;
        assert(field.value().is<int>() && !field.value().is<bool>());
    }
    for (JsonObjectConst driver : doc["driverOptions"].as<JsonArrayConst>()) {
        assert(driver["backend"].as<JsonString>().isStatic());
        assert(driver["driver"].as<JsonString>().isStatic());
        assert(driver["label"].as<JsonString>().isStatic());
        assert(driver["minWidth"].is<int>() && driver["maxHeight"].is<int>());
    }
}

int main() {
    checkScreenDescriptor();
    for (const char* name : {"SHT30", "SHT31", "BMP280", "MPU6050", "BH1750", "INA226", "AHT20", "ADS1115"}) {
        FastBee::DriverRegistry::getInstance().registerDriver(name, []() -> ISensorDriver* { return nullptr; });
    }
    // Compare the entire original/current descriptor graph at every eligible
    // ArduinoJson allocation, including scalar types, ownership and OOM order.
    // String/fragment faults remain covered separately by the complete route.
    for (unsigned type = 0; type <= 60; ++type) {
        auto capture = [=](bool oracle, size_t fault, bool persistent) {
            DocumentProbeAllocator allocator(fault, persistent);
            SharedSnapshot snapshot;
            {
                JsonDocument doc(&allocator);
                if (oracle) CatalogOracle::addPeripheralTypeDescriptorDetails(
                    doc.to<JsonObject>(), static_cast<PeripheralType>(type));
                else addPeripheralTypeDescriptorDetails(
                    doc.to<JsonObject>(), static_cast<PeripheralType>(type));
                snapshot = captureShared(doc);
            }
            assert(allocator.blocks.empty());
            return std::make_tuple(snapshot, allocator.events, allocator.eligible, allocator.failures);
        };
        const auto baseline = capture(true, std::numeric_limits<size_t>::max(), false);
        assert(capture(false, std::numeric_limits<size_t>::max(), false) == baseline);
        for (bool persistent : {false, true}) {
            for (size_t fault = 0; fault < std::get<2>(baseline) + 2; ++fault) {
                assert(capture(false, fault, persistent) == capture(true, fault, persistent));
            }
        }
    }
    auto expected = expectedCatalog();
    // Wire equality alone cannot expose a changed numeric storage type or
    // copied metadata string. Compare all eight real drivers' semantic shape
    // with the independent frozen serializer for both captured feature states.
    for (bool enabled : {false, true}) {
        FastBee::RuntimeFeatureManager::instance().enabled = enabled;
        auto oracle = expectedCatalog();
        const auto descriptors = oracle["data"]["special"][2]["driverDescriptors"].as<JsonArrayConst>();
        assert(descriptors.size() == 8);
        for (size_t i = 0; i < FastBee::DriverRegistry::getInstance().getDriverCount(); ++i) {
            const auto* entry = FastBee::DriverRegistry::getInstance().getDriverEntry(i);
            JsonDocument actual;
            streamedSensorDescriptor(actual.to<JsonObject>(), *entry->metadata, enabled);
            SharedShape shape;
            assert(ArduinoJson::detail::accept(actual.as<JsonVariantConst>(), shape) ==
                   ArduinoJson::detail::accept(descriptors[i].as<JsonVariantConst>(), shape));
            std::string actualBytes, expectedBytes;
            serializeJson(actual, actualBytes); serializeJson(descriptors[i], expectedBytes);
            assert(actualBytes == expectedBytes && !actual.overflowed());
        }
    }
    FastBee::RuntimeFeatureManager::instance().enabled = false;
    Context context;
    PeripheralRouteHandler handler{&context};
    catalogAllocator.peak = catalogStringPeak = catalogCombinedPeak = 0;
    {
        AsyncWebServerRequest request;
        beforeR2CatalogHandler(&handler, &request);
        assert(!request.error && request.response);
        const auto body = consume(*request.response, 1436);
        std::string reference; serializeJson(expected, reference);
        assert(body == reference);
    }
    const size_t beforeJsonPeak = catalogAllocator.peak;
    const size_t beforeStringPeak = catalogStringPeak;
    const size_t beforeCombinedPeak = catalogCombinedPeak;
    assert(catalogAllocator.bytes == 0 && catalogReservedBytes == 0);
    catalogAllocator.peak = catalogStringPeak = catalogCombinedPeak = 0;
    size_t catalogBodyBytes = 0;
    for (size_t step : {size_t(1), size_t(7), size_t(256), size_t(768), size_t(1436)}) {
        AsyncWebServerRequest request;
        handler.handleGetPeripheralTypes(&request);
        assert(!request.error && request.response);
        assert(catalogAllocator.bytes == 0); // no JSON tree survives the route
        auto actualBytes = consume(*request.response, step);
        catalogBodyBytes = actualBytes.size();
        JsonDocument actual;
        assert(!deserializeJson(actual, actualBytes));
        std::string reference; serializeJson(expected, reference);
        assert(actualBytes == reference);
        assert(actual["data"]["special"].size() == 22);
        bool found55 = false, found56 = false, found57 = false;
        const JsonArrayConst special = actual["data"]["special"].as<JsonArrayConst>();
        for (JsonObjectConst item : special) {
            if (item["value"] == 55) { found55 = true; assert(item["driverDescriptors"].size() == 2); }
            if (item["value"] == 56) { found56 = true; assert(item["uiRuntimeCompiled"] == bool(FASTBEE_ENABLE_SCREEN_UI)); }
            if (item["value"] == 57) {
                found57 = true;
                assert(item["pinRoles"].size() == 0 && item["maxInstances"] == 1);
                assert(item["requiresOeReference"] == true && item["requiresFreshPowerCycle"] == true);
                assert(item["sharedFrequency"] == true);
            }
        }
        assert(found55 && found56 && found57);
    }
    const size_t boundedPeak = catalogAllocator.peak;
    const size_t boundedStringPeak = catalogStringPeak;
    const size_t boundedCombinedPeak = catalogCombinedPeak;
    assert(boundedPeak < beforeJsonPeak && boundedCombinedPeak < beforeCombinedPeak);
    catalogAllocator.peak = 0;
    { auto legacy = expectedCatalog(&catalogAllocator); assert(!legacy.overflowed()); }
    const size_t legacyPeak = catalogAllocator.peak;
    assert(catalogAllocator.bytes == 0 && boundedPeak < legacyPeak);

    // A captured metadata/feature array is independent of the caller's array.
    // Runtime OOM keeps the same byte cursor and returns TRY_AGAIN, never EOF.
    {
        const FastBee::SensorDriverMetadata* metadata[] = {FastBee::sensorDriverMetadata("SHT30")};
        bool active[] = {false};
        std::unique_ptr<SensorDriverCatalogFragment> reader(new SensorDriverCatalogFragment(
            metadata, active, 1, String(), String()));
        metadata[0] = FastBee::sensorDriverMetadata("SHT31"); active[0] = true;
        FastBee::JsonFragmentsResponse response(1);
        assert(response.setReader(0, std::move(reader)));
        catalogAllocator.failAllocations = true;
        uint8_t out[3];
        assert(response._fillBuffer(out, sizeof(out)) == FastBee::JsonFragmentsResponse::Reader::TryAgain);
        catalogAllocator.failAllocations = false;
        std::string reference;
        serializeJson(expected["data"]["special"][2]["driverDescriptors"][0], reference);
        assert(consume(response, 7) == reference);
    }
    assert(catalogAllocator.bytes == 0 && catalogReservedBytes == 0);

    // The separator may already have reached TCP when the next driver's DOM
    // allocation fails. Repeated OOM must keep that cursor without repeating
    // the separator or dropping the next record after allocation recovers.
    {
        const FastBee::SensorDriverMetadata* metadata[] = {
            FastBee::sensorDriverMetadata("SHT30"), FastBee::sensorDriverMetadata("ADS1115")};
        const bool active[] = {false, false};
        std::unique_ptr<SensorDriverCatalogFragment> reader(new SensorDriverCatalogFragment(
            metadata, active, 2, String(), String()));
        FastBee::JsonFragmentsResponse response(1);
        assert(response.setReader(0, std::move(reader)));
        std::string first, second;
        const auto descriptors = expected["data"]["special"][2]["driverDescriptors"].as<JsonArrayConst>();
        serializeJson(descriptors[0], first);
        serializeJson(descriptors[7], second);
        std::string actual;
        uint8_t buffer[7];
        while (actual.size() < first.size()) {
            const size_t step = std::min(sizeof(buffer), first.size() - actual.size());
            const size_t copied = response._fillBuffer(buffer, step);
            assert(copied == step);
            actual.append(reinterpret_cast<char*>(buffer), copied);
        }
        catalogAllocator.failAllocations = true;
        assert(response._fillBuffer(buffer, sizeof(buffer)) == 1 && buffer[0] == ',');
        actual.push_back(',');
        for (int attempt = 0; attempt < 3; ++attempt) {
            assert(response._fillBuffer(buffer, sizeof(buffer)) == FastBee::JsonFragmentsResponse::Reader::TryAgain);
        }
        catalogAllocator.failAllocations = false;
        while (actual.size() < response._contentLength) {
            const size_t copied = response._fillBuffer(buffer, sizeof(buffer));
            assert(copied && copied <= sizeof(buffer));
            actual.append(reinterpret_cast<char*>(buffer), copied);
        }
        assert(actual == first + ',' + second);
        assert(response._fillBuffer(buffer, sizeof(buffer)) == 0);
    }
    assert(catalogAllocator.bytes == 0 && catalogReservedBytes == 0);

    // Captured feature state cannot change halfway through a response.
    // One-byte reads cross every descriptor and framing boundary.
    {
        AsyncWebServerRequest request;
        handler.handleGetPeripheralTypes(&request);
        FastBee::RuntimeFeatureManager::instance().enabled = true;
        const auto body = consume(*request.response, 1);
        std::string reference; serializeJson(expected, reference);
        assert(body == reference);
        FastBee::RuntimeFeatureManager::instance().enabled = false;
    }
    assert(catalogAllocator.bytes == 0 && catalogReservedBytes == 0);
    // Disconnect while the sensor's one active DOM is retained; both it and
    // the response snapshot must release without a subsequent ACK callback.
    {
        AsyncWebServerRequest request;
        handler.handleGetPeripheralTypes(&request);
        uint8_t byte;
        std::string prefix;
        while (prefix.find("\"driverDescriptors\":[{") == std::string::npos) {
            assert(request.response->_fillBuffer(&byte, 1) == 1);
            prefix.push_back(static_cast<char>(byte));
        }
        assert(catalogAllocator.bytes > 0);
    }
    assert(catalogAllocator.bytes == 0 && catalogReservedBytes == 0);

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
    catalogAllocator.failAllocations = true;
    { AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request);
      assert(request.error == 503 && !request.response); }
    catalogAllocator.failAllocations = false;
    for (int failAfter : {0, 1, 2, 8, 12}) {
        catalogReserveFailure = failAfter;
        AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request);
        assert(request.error == 503 && !request.response && catalogAllocator.bytes == 0);
    }
    catalogReserveFailure = -1;
    catalogConcatFailure = true;
    { AsyncWebServerRequest request; handler.handleGetPeripheralTypes(&request); assert(request.error == 503 && !request.response); }
    catalogConcatFailure = false;
    // Full C3-sized body plus per-String rounding exceeds the same untouched
    // 8KiB guard at the release minimum of 25KiB. Borrowed static records retain every wire field while
    // making this declared normal budget admissible. Dynamic JSON remains OOM
    // checked before any response is sent.
    accountCatalogStorage = true;
    freeBytes = 25600;
    {
        AsyncWebServerRequest request;
        legacyCatalogHandler(&handler, &request);
        std::cerr << "bounded fixture legacy status " << request.error << "; body "
                  << catalogBodyBytes << "; retained " << catalogReservedBytes
                  << "; guard free " << FastBee::MemoryBudget::dramFree() << '\n';
        assert(request.error == 503 && !request.response);
    }
    assert(catalogReservedBytes == 0 && catalogAllocator.bytes == 0);
    {
        AsyncWebServerRequest request;
        handler.handleGetPeripheralTypes(&request);
        assert(!request.error && request.response);
        assert(catalogReservedBytes + 8192 <= freeBytes);
        const auto body = consume(*request.response, 7);
        std::string reference; serializeJson(expected, reference);
        assert(body == reference);
    }
    assert(catalogReservedBytes == 0);
    freeBytes = 16384;
    {
        AsyncWebServerRequest request;
        handler.handleGetPeripheralTypes(&request);
        assert(request.error == 503 && !request.response);
    }
    assert(catalogReservedBytes == 0 && catalogAllocator.bytes == 0);
    accountCatalogStorage = false;
    freeBytes = 50000;
    std::cout << "catalog 40 types; JSON equivalence, short buffers, auth, OOM, disconnect passed; host JSON pool peak "
              << boundedPeak << " vs legacy " << legacyPeak << " bytes; body " << catalogBodyBytes << " bytes\n"
              << "R2 before JSON peak=" << beforeJsonPeak << " String peak=" << beforeStringPeak
              << " combined peak=" << beforeCombinedPeak << "; after JSON peak=" << boundedPeak
              << " String peak=" << boundedStringPeak << " combined peak=" << boundedCombinedPeak << "\n";
}
