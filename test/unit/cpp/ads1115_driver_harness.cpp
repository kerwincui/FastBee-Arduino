#include "peripherals/drivers/ADS1115Driver.h"
#include "core/DriverRegistry.h"
#include "core/SensorConfigSupport.h"
#include "core/SensorDriverCache.h"
#include "core/RuntimeFeatureManager.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <functional>
#include <esp_timer.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #x); std::exit(86); } } while (false)
using namespace FastBee;
static void failed(const SensorReading& reading) {
    CHECK(!reading.success && reading.channelCount == 0 && reading.timestamp == 0);
    for (float value : reading.values) CHECK(std::isnan(value));
}
static std::function<void()> scanHook;
static void runScanHook() { CHECK(adsTestCacheLockDepth == 0 && sensorTestBusLockDepth == 0); scanHook(); }
class ScanPeerSensor final : public ISensorDriver {
public:
    const char* getName() const override { return "ScanPeer"; }
    uint8_t getChannelCount() const override { return 1; }
    const char* getChannelName(uint8_t) const override { return "value"; }
    const char* getChannelUnit(uint8_t) const override { return "unit"; }
    bool init(uint8_t, const char*) override { return true; }
    bool read(SensorReading& value) override {
        value.success = true; value.channelCount = 1; value.values[0] = 42; value.timestamp = millis(); return true;
    }
    void deinit() override {}
};
static ISensorDriver* makeScanPeer() { return new ScanPeerSensor; }
class ScanPendingSensor final : public ISensorDriver {
public:
    const char* getName() const override { return "ScanPending"; }
    uint8_t getChannelCount() const override { return 1; }
    const char* getChannelName(uint8_t) const override { return "value"; }
    const char* getChannelUnit(uint8_t) const override { return "unit"; }
    bool init(uint8_t, const char*) override { return true; }
    bool read(SensorReading&) override { return false; }
    uint32_t cooperativeReadBudgetMs() const override { return 1000; }
    SensorReadStep readStep(SensorReading&) override { return SensorReadStep::Pending; }
    void deinit() override {}
};
static ISensorDriver* makeScanPending() { return new ScanPendingSensor; }
#include "ads1115_scan_cases.inc"
int main(int argc, char** argv) {
    CHECK(argc == 2 || argc == 3);
    const std::string mode = argv[1];
    if (mode == "metadata") {
        const auto* meta = DriverRegistry::getInstance().getMetadata("ADS1115");
        CHECK(meta);
        JsonDocument document;
        document["name"] = meta->name;
        document["minIntervalMs"] = meta->minIntervalMs;
        auto channels = document["channels"].to<JsonArray>();
        for (uint8_t i = 0; i < meta->channelCount; ++i) {
            auto channel = channels.add<JsonObject>();
            channel["name"] = meta->channels[i].name; channel["unit"] = meta->channels[i].unit;
            if (meta->channels[i].scanMux >= 0) channel["scanMux"] = meta->channels[i].scanMux;
            if (meta->channels[i].singleMuxOnly) channel["singleMuxOnly"] = true;
        }
        auto params = document["params"].to<JsonArray>();
        for (uint8_t i = 0; i < meta->paramCount; ++i) {
            const auto& item = meta->params[i]; auto param = params.add<JsonObject>();
            param["key"] = item.name; param["type"] = item.type; param["unit"] = item.unit;
            param["default"] = item.defaultValue; param["min"] = item.minValue; param["max"] = item.maxValue;
            if (item.choiceCount) { auto choices = param["choices"].to<JsonArray>();
                for (uint8_t c = 0; c < item.choiceCount; ++c) choices.add(item.choices[c]); }
        }
        std::string output; serializeJson(document, output); std::puts(output.c_str()); return 0;
    }
    ADS1115Driver driver;
    SensorReading reading;
    if (mode.rfind("scan_", 0) == 0 && mode != "scan_example") {
        scanCase(mode);
    } else if (mode == "example" || mode == "scan_example") {
        CHECK(argc == 3);
        std::ifstream stream(argv[2]); CHECK(stream.good());
        JsonDocument bundle; CHECK(deserializeJson(bundle, stream) == DeserializationError::Ok);
        CHECK(bundle["peripherals"].size() == 1 && bundle["rules"].size() == 1);
        const auto peripheral = bundle["peripherals"][0];
        CHECK(!peripheral["enabled"].as<bool>() && peripheral["type"].as<int>() == 38);
        PeripheralConfig config; initializeSensorDefaults(config);
        config.pinCount = 2; config.pins[0] = peripheral["pins"][0].as<uint8_t>(); config.pins[1] = peripheral["pins"][1].as<uint8_t>();
        String error, channel;
        CHECK(parseSensorConfig(peripheral["params"].as<JsonObjectConst>(), config, error));
        CHECK(validateSensorConfig(config, error));
        const auto rule = bundle["rules"][0];
        CHECK(!rule["enabled"].as<bool>() && !rule["reportAfterExec"].as<bool>());
        const auto action = rule["actions"][0];
        CHECK(action["actionType"].as<int>() == 19);
        CHECK(!std::strcmp(action["targetPeriphId"].as<const char*>(), peripheral["id"].as<const char*>()));
        JsonDocument details; CHECK(deserializeJson(details, action["actionValue"].as<const char*>()) == DeserializationError::Ok);
        CHECK(resolveSensorChannel(config.params.sensor.driver, 0, details["channelField"].as<const char*>(), channel, error, config.params.sensor.scanMask));
        if (mode == "scan_example") {
            CHECK(config.params.sensor.scanMask == 240 && rule["actions"].size() == 4);
            const JsonArrayConst scanActions = rule["actions"].as<JsonArrayConst>();
            for (JsonObjectConst item : scanActions) {
                JsonDocument parsed; CHECK(deserializeJson(parsed,item["actionValue"].as<const char*>()) == DeserializationError::Ok);
                CHECK(resolveSensorChannel("ADS1115",0,parsed["channelField"].as<const char*>(),channel,error,240));
            }
        }
        CHECK(details["scaleFactor"].as<float>() == 1 && details["offset"].as<float>() == 0);
        CHECK(Wire.transmissions == 0);
    } else if (mode == "registration") {
        auto& registry = DriverRegistry::getInstance();
        CHECK(registry.getMetadata("ads1115"));
        std::unique_ptr<ISensorDriver> instance(registry.createDriver("ADS1115"));
        CHECK(instance && instance->getChannelCount() == 2);
        CHECK(!std::strcmp(instance->getChannelName(0), "voltage"));
        CHECK(!std::strcmp(instance->getChannelName(1), "rawCode"));
    } else if (mode == "config") {
        PeripheralConfig config;
        initializeSensorDefaults(config);
        config.pinCount = 2; config.pins[0] = 21; config.pins[1] = 22;
        JsonDocument input; String error;
        CHECK(deserializeJson(input, R"({"driver":"ads1115","mux":3,"dataRate":8,"fullScaleMv":2048,"calibrationGain":2,"calibrationOffset":-1})") == DeserializationError::Ok);
        CHECK(parseSensorConfig(input.as<JsonObjectConst>(), config, error));
        CHECK(validateSensorConfig(config, error));
        JsonDocument output; serializeSensorConfig(output.to<JsonObject>(), config);
        CHECK(output["mux"].as<int>() == 3 && output["dataRate"].as<int>() == 8);
        CHECK(output["calibrationOffset"].as<float>() == -1);
        JsonDocument params; buildSensorDriverParams(params, config, 21, 22, 100000);
        CHECK(params["mux"].as<int>() == 3 && params["calibrationGain"].as<float>() == 2);
        CHECK(sensorSampleIntervalMs(config) >= 300);
        config.params.sensor.sampleInterval = 100;
        CHECK(!validateSensorConfig(config, error));
        config.params.sensor.sampleInterval = 1000;
        const auto before = config.params.sensor;
        input["dataRate"] = 129;
        CHECK(!parseSensorConfig(input.as<JsonObjectConst>(), config, error));
        CHECK(config.params.sensor.dataRate == before.dataRate && config.params.sensor.mux == before.mux);
        String channel;
        CHECK(resolveSensorChannel("ADS1115", 0, "rawCode", channel, error));
        CHECK(!resolveSensorChannel("ADS1115", 0, "temperature", channel, error));
    } else if (mode == "schema_source") {
        const auto* meta = sensorDriverMetadata("ADS1115");
        CHECK(meta && meta->paramCount == 6 && meta->getParameter("missing") == nullptr);
        PeripheralConfig config;
        initializeSensorDefaults(config);
        config.pinCount = 2; config.pins[0] = 21; config.pins[1] = 22;
        JsonDocument input; input["driver"] = "ADS1115";
        String error;
        CHECK(parseSensorConfig(input.as<JsonObjectConst>(), config, error));
        CHECK(validateSensorConfig(config, error));
        JsonDocument serialized; serializeSensorConfig(serialized.to<JsonObject>(), config);
        for (unsigned i = 0; i < meta->paramCount; ++i) {
            const auto& field = meta->params[i];
            CHECK(field.acceptsNumber(field.defaultValue));
            CHECK(serialized[field.name].as<double>() == field.defaultValue);
            for (double bad : {field.minValue - 1, field.maxValue + 1, double(NAN), double(INFINITY)}) {
                auto next = config;
                JsonDocument patch; patch[field.name] = bad;
                CHECK(!parseSensorConfig(patch.as<JsonObjectConst>(), next, error));
                CHECK(next.params.sensor.mux == config.params.sensor.mux &&
                      next.params.sensor.fullScaleMv == config.params.sensor.fullScaleMv &&
                      next.params.sensor.dataRate == config.params.sensor.dataRate &&
                      next.params.sensor.calibrationGain == config.params.sensor.calibrationGain &&
                      next.params.sensor.calibrationOffset == config.params.sensor.calibrationOffset);
            }
            for (unsigned n = 0; n < field.choiceCount; ++n) {
                auto next = config;
                JsonDocument patch; patch[field.name] = field.choices[n];
                CHECK(parseSensorConfig(patch.as<JsonObjectConst>(), next, error));
            }
        }
        // A partial update preserves the configured value, not the descriptor
        // default. New driver selections reset only via the existing policy.
        input.clear(); input["mux"] = 3; input["calibrationGain"] = 2;
        CHECK(parseSensorConfig(input.as<JsonObjectConst>(), config, error));
        input.clear(); input["calibrationOffset"] = -1;
        CHECK(parseSensorConfig(input.as<JsonObjectConst>(), config, error));
        CHECK(config.params.sensor.mux == 3 && config.params.sensor.calibrationGain == 2);
        input.clear(); input["calibrationGain"] = meta->getParameter("calibrationGain")->minValue;
        CHECK(parseSensorConfig(input.as<JsonObjectConst>(), config, error));
        CHECK(validateSensorConfig(config, error));
        const auto& minimumGain = *meta->getParameter("calibrationGain");
        CHECK(config.params.sensor.calibrationGain == static_cast<float>(minimumGain.minValue));
        // The serializer must export the decimal endpoint, not the slightly
        // lower double obtained by widening its stored float representation.
        serialized.clear(); serializeSensorConfig(serialized.to<JsonObject>(), config);
        CHECK(serialized["calibrationGain"].as<double>() == minimumGain.minValue);
        std::string saved; serializeJson(serialized, saved);
        JsonDocument imported; CHECK(deserializeJson(imported, saved) == DeserializationError::Ok);
        auto restored = config;
        CHECK(parseSensorConfig(imported.as<JsonObjectConst>(), restored, error));
        CHECK(validateSensorConfig(restored, error));
        CHECK(restored.params.sensor.calibrationGain == config.params.sensor.calibrationGain);
        // Actual user JSON tokens, not only direct assignment to JsonDocument.
        // Short endpoint decimals are stored as float by ArduinoJson 7.4.
        for (const char* decimal : {"{\"calibrationGain\":0.000001}",
                                   "{\"calibrationGain\":1e-6}",
                                   "{\"calibrationGain\":0.0000010000000000000000}"}) {
            imported.clear(); CHECK(deserializeJson(imported, decimal) == DeserializationError::Ok);
            CHECK(parseSensorConfig(imported.as<JsonObjectConst>(), restored, error));
            CHECK(validateSensorConfig(restored, error));
            CHECK(restored.params.sensor.calibrationGain == config.params.sensor.calibrationGain);
        }
        for (const char* decimal : {"{\"calibrationGain\":0.00000099999999}",
                                   "{\"calibrationGain\":9.9999999e-7}",
                                   "{\"calibrationGain\":\"0.0000009999999974752427\"}"}) {
            imported.clear(); CHECK(deserializeJson(imported, decimal) == DeserializationError::Ok);
            const auto previous = restored;
            CHECK(!parseSensorConfig(imported.as<JsonObjectConst>(), restored, error));
            CHECK(restored.params.sensor.calibrationGain == previous.params.sensor.calibrationGain);
        }
        imported.clear(); imported["calibrationGain"] = minimumGain.minValue * (1.0 - 1e-8);
        CHECK(!parseSensorConfig(imported.as<JsonObjectConst>(), restored, error));
        imported.clear(); imported["calibrationGain"] =
            std::nextafterf(static_cast<float>(minimumGain.minValue), 0.0f);
        CHECK(!parseSensorConfig(imported.as<JsonObjectConst>(), restored, error));
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            serialized.clear(); serializeSensorConfig(serialized.to<JsonObject>(), restored);
            CHECK(serialized["calibrationGain"].is<double>());
            saved.clear(); serializeJson(serialized, saved);
            imported.clear(); CHECK(deserializeJson(imported, saved) == DeserializationError::Ok);
            CHECK(parseSensorConfig(imported.as<JsonObjectConst>(), restored, error));
            CHECK(validateSensorConfig(restored, error));
            CHECK(restored.params.sensor.calibrationGain == config.params.sensor.calibrationGain);
        }
        // Unrelated partial updates retain that exact valid stored endpoint.
        for (const char* key : {"mux", "fullScaleMv", "dataRate", "calibrationOffset"}) {
            JsonDocument partial; partial[key] = meta->getParameter(key)->defaultValue;
            CHECK(parseSensorConfig(partial.as<JsonObjectConst>(), restored, error));
            CHECK(restored.params.sensor.calibrationGain == static_cast<float>(minimumGain.minValue));
        }
        // Explicit below-minimum values remain rejected even when narrowing
        // would round them to the same float as the legal minimum.
        input.clear(); input["calibrationGain"] = minimumGain.minValue * (1.0 - 1e-8);
        CHECK(!parseSensorConfig(input.as<JsonObjectConst>(), restored, error));
        input["calibrationGain"] = "0.0000009999999974752427";
        CHECK(!parseSensorConfig(input.as<JsonObjectConst>(), restored, error));
        restored.params.sensor.calibrationGain = std::nextafterf(static_cast<float>(minimumGain.minValue), 0.0f);
        input.clear(); input["mux"] = 4;
        CHECK(!parseSensorConfig(input.as<JsonObjectConst>(), restored, error));
        CHECK(Wire.transmissions == 0);
    } else if (mode == "all_configs") {
        for (unsigned mux = 0; mux < 8; ++mux) for (unsigned range : {6144, 4096, 2048, 1024, 512, 256})
        for (unsigned rate : {8, 16, 32, 64, 128, 250, 475, 860}) {
            JsonDocument params; params["mux"] = mux; params["fullScaleMv"] = range; params["dataRate"] = rate;
            std::string json; serializeJson(params, json);
            CHECK(driver.init(0, json.c_str())); CHECK(driver.read(reading));
            CHECK(std::fabs(reading.values[0] - range / 4000.0f) < 0.00001f);
            driver.deinit();
        }
    } else if (mode == "invalid") {
        for (const char* params : {R"({"mux":8})", R"({"mux":true})", R"({"dataRate":129})",
             R"({"fullScaleMv":2049})", R"({"calibrationGain":0})", R"({"calibrationOffset":"x"})", "[]"}) {
            CHECK(!driver.init(0, params)); CHECK(Wire.transmissions == 0);
        }
    } else if (mode == "disabled") {
        RuntimeFeatureManager::instance().enabled = false;
        CHECK(!driver.init(0, "{}") && Wire.transmissions == 0);
    } else if (mode == "cache") {
        auto& cache = SensorDriverCache::instance();
        float value; String unit, error;
        CHECK(cache.readChannel("adc", "ADS1115", 0, "{}", "voltage", value, unit, error, 100));
        CHECK(std::fabs(value - 1.024f) < 0.00001f && Wire.starts == 1);
        CHECK(cache.readChannel("adc", "ADS1115", 0, "{}", "rawCode", value, unit, error, 100));
        CHECK(value == 8192 && Wire.starts == 1);
        sensorTestNow += 200; Wire.ack = false;
        CHECK(!cache.readChannel("adc", "ADS1115", 0, "{}", "voltage", value, unit, error, 100));
        CHECK(std::isnan(value));
        SensorReading snapshot; bool stale = false;
        CHECK(cache.getSnapshot("adc", snapshot, stale, error) && stale && !snapshot.success);
        Wire.ack = true; sensorTestNow += 200;
        CHECK(cache.readChannel("adc", "ADS1115", 0, "{}", "voltage", value, unit, error, 100));
        cache.releaseAll();
    } else if (mode == "duplicate") {
        ADS1115Driver peer;
        CHECK(driver.init(0, "{}"));
        const auto count = Wire.transmissions;
        CHECK(!peer.init(0, "{}") && Wire.transmissions == count);
        CHECK(driver.read(reading));
        driver.deinit(); CHECK(peer.init(0, "{}")); peer.deinit();
    } else if (mode == "faults") {
        CHECK(driver.init(0, "{}"));
        unsigned before = Wire.transmissions;
        CHECK(driver.read(reading)); const unsigned transfers = Wire.transmissions - before;
        driver.deinit();
        for (unsigned transfer = 1; transfer <= transfers; ++transfer) {
            Wire = TwoWire(); CHECK(driver.init(0, "{}"));
            Wire.failTxAt = Wire.transmissions + transfer;
            CHECK(!driver.read(reading)); failed(reading); driver.deinit();
        }
    } else {
        if (mode == "continuous") Wire.config = 0x0483;
        const char* params = mode == "calibration" ? R"({"calibrationGain":2,"calibrationOffset":-1})" : "{}";
        CHECK(driver.init(0, params));
        if (mode == "wrap") sensorTestNow = 0xfffffffe;
        if (mode == "negative") Wire.sample = 0xe000;
        if (mode == "rail") Wire.sample = 0x8000;
        if (mode == "timeout") Wire.stuck = true;
        if (mode == "missed") Wire.missedBusy = true;
        if (mode == "lost") Wire.configLost = true;
        if (mode == "short") Wire.shortRead = true;
        if (mode == "negative_read") Wire.negativeRead = true;
        if (mode == "short_write") Wire.writeLimit = 0;
        if (mode == "closed") driver.deinit();
        if (mode == "lost_lease") CHECK(I2CBusManager::instance().release(&driver));
        const bool reject = mode == "rail" || mode == "timeout" || mode == "missed" || mode == "lost" ||
            mode == "short" || mode == "negative_read" || mode == "short_write" || mode == "closed" || mode == "lost_lease";
        const bool ok = driver.read(reading);
        if (reject) { CHECK(!ok); failed(reading); CHECK(driver.getLastError()[0]); }
        else {
            CHECK(ok && reading.success && reading.channelCount == 2);
            const float expected = mode == "negative" ? -1.024f : mode == "calibration" ? 1.048f : 1.024f;
            CHECK(std::fabs(reading.values[0] - expected) < 0.00001f && Wire.starts == 1);
            if (mode == "continuous") CHECK(Wire.idleWrites == 1);
        }
    }
    driver.deinit();
    CHECK(!sensorTestWaitedWithBusLock);
    CHECK(I2CBusManager::instance().clientCount() == 0);
    std::printf("ADS driver %s passed\n", argv[1]);
}
