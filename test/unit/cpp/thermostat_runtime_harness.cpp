// Runs the exact production method definitions with deterministic lock, sample
// and GPIO-claim seams. It does not emulate FreeRTOS scheduling or pad hardware.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoJson/Variant/JsonVariantVisitor.hpp>
#include <cstdlib>
#include <limits>
#include <cassert>
#include <memory>
#include <atomic>
#include <cerrno>
#include "core/PeripheralExecution.h"
#include "core/ThermostatRuntime.h"
#include "core/SensorConfigEpoch.h"
#include "core/DriverRegistry.h"
inline void convertToJson(const String& value, JsonVariant out) { out.set(std::string(value)); }
inline void convertFromJson(JsonVariantConst value, String& out) { out = value.as<std::string>(); }
inline size_t serializeJson(const JsonDocument& doc, String& out) {
    std::string text; const auto size = serializeJson(doc, text); out = text; return size;
}
static uint64_t testNow = 1000;
#define millis() static_cast<uint32_t>(testNow)
static int64_t esp_timer_get_time() { return static_cast<int64_t>(testNow * 1000); }
#define pdMS_TO_TICKS(value) (value)
struct TestMutex { bool allowed = true; };
class MutexGuard {
public:
    explicit MutexGuard(TestMutex* mutex, uint32_t = 0) : acquired_(mutex && mutex->allowed) {}
    bool isLocked() const { return acquired_; }
private: bool acquired_;
};
using RecursiveMutexGuard = MutexGuard;
enum class MemoryGuardLevel { NORMAL, WARN, SEVERE, CRITICAL };
struct HealthMonitor {
    MemoryGuardLevel level = MemoryGuardLevel::NORMAL;
    MemoryGuardLevel getMemoryGuardLevel() const { return level; }
};
struct FastBeeFramework {
    HealthMonitor health;
    static FastBeeFramework* getInstance() { static FastBeeFramework instance; return &instance; }
    HealthMonitor* getHealthMonitor() { return &health; }
};
namespace FastBee { namespace MemoryBudget {
constexpr size_t PERIPH_EXEC_MIN_DRAM = 16000;
inline size_t dram8Free() { return 200000; }
}}
// Snapshot/locking remain explicit hardware seams. The production registry
// and channel metadata are real; no driver is instantiated by these tests.
enum class PeripheralType { SENSOR_GENERIC, I2C, GPIO_ANALOG_INPUT };
struct PeripheralConfig {
    bool enabled = true;
    PeripheralType type = PeripheralType::GPIO_ANALOG_INPUT;
    struct { struct { char driver[24] = {}; uint8_t mode = 0; } sensor; } params;
};
struct PeripheralManager {
    TestMutex mutex;
    PeripheralConfig source;
    bool snapshotAvailable = true;
    static PeripheralManager& getInstance() { static PeripheralManager instance; return instance; }
    TestMutex* getMutex() { return &mutex; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& out) {
        if (!snapshotAvailable || !mutex.allowed || id != "room") return false;
        out = source; return true;
    }
};
struct PeriphExecExecutor {
    static String guessSensorDataField(const String&);
    static String resolveChannelField(const String&, const String&, const String&);
};
#include "thermostat_sensor_channels.inc"
enum class ObservationQuality { UNKNOWN, GOOD, STALE, OFFLINE, ERROR };
struct ObservationSnapshot {
    bool hasValue = true, bindingValid = true, stale = false;
    ObservationQuality quality = ObservationQuality::GOOD;
    String value = "20", valueType = "number", sourceRevision = "room:temperature:C", unit = "C";
    uint64_t sampledAtMs = 500;
};
class PeriphExecManager {
public:
    TestMutex rulesMutex;
    TestMutex* _rulesMutex = &rulesMutex;
    std::map<String, PeriphExecRule> rules;
    uint64_t _ruleRuntimeRevision = 1;
    bool _configurationLoadFailed = false, _configurationRestartPending = false;
    std::unique_ptr<FastBee::ThermostatRuntime> _thermostats[4];
    std::atomic<bool> _thermostatShutdown{false};
    uint32_t _lastThermostatCheck = 0;
    ObservationSnapshot observation;
    bool getObservation(const String&, ObservationSnapshot& out) { out = observation; return true; }
    bool thermostatSourceBindingLocked(const String&, String&) const;
    bool legacyThermostatSourceBindingLocked(const String&, String&) const;
    void retireThermostatsLocked();
    void retireSdRecorderLocked() {} // Separate recorder lifecycle has its own full production harness.
    void checkThermostats();
    bool shutdownThermostats();
    bool getThermostatStatus(const PeriphExecRule&, ActionExecResult&);
    String getThermostatDiagnosticsJson() const;
};
#include "thermostat_runtime_methods.inc"
#include "thermostat_source_binding_legacy_generated.inc"

static void setup(PeriphExecManager& manager) {
    PeriphExecRule collector; collector.id = "collect"; collector.enabled = true;
    ExecAction action; action.actionType = 19; action.targetPeriphId = "room";
    action.actionValue = R"({"periphId":"room","channelField":"temperature","unit":"C"})";
    collector.actions.push_back(action); manager.rules[collector.id] = collector;
    PeriphExecRule control; control.id = "control"; control.enabled = true; control.runtimeRevision = 1;
    ExecTrigger trigger; trigger.triggerType = 1; trigger.intervalSec = 1; control.triggers.push_back(trigger);
    action.actionType = 39; action.targetPeriphId = "relay";
    action.actionValue = R"({"sourceRef":"room_temperature","mode":"heat","setpoint":25,"hysteresis":1,"minOnSec":1,"minOffSec":1,"maxAgeSec":30,"recovery":"fresh","startup":"fresh","fault":"off"})";
    control.actions.push_back(action); manager.rules[control.id] = control;
}
static void tick(PeriphExecManager& manager, uint64_t now, bool fresh = true) {
    testNow = now; if (fresh) manager.observation.sampledAtMs = now;
    manager.checkThermostats();
}
static void running(PeriphExecManager& manager) {
    tick(manager, 1000, false); tick(manager, 2000);
    assert(!FastBee::thermostatOutput.high);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    PeriphExecManager manager; setup(manager);
    auto& output = FastBee::thermostatOutput;
    const char* mode = argv[1];
    if (!std::strcmp(mode, "source-mode-parser")) {
        assert(FastBee::DriverRegistry::getInstance().registerDriver("MPU6050",
            +[]() -> ISensorDriver* { return nullptr; }));
        auto& peripheral = PeripheralManager::getInstance().source;
        peripheral.type = PeripheralType::I2C;
        struct ModeCase { const char* json; const char* first; };
        for (const auto& row : {
                ModeCase{"{}", "accelX"}, ModeCase{R"({"mode":null})", "accelX"},
                ModeCase{R"({"mode":0})", "accelX"}, ModeCase{R"({"mode":1})", "gyroX"},
                ModeCase{R"({"mode":"0"})", "accelX"}, ModeCase{R"({"mode":"1"})", "gyroX"},
                ModeCase{R"({"mode":"accel"})", "accelX"}, ModeCase{R"({"mode":"gyro"})", "gyroX"},
                ModeCase{R"({"mode":"ACCEL"})", "accelX"}, ModeCase{R"({"mode":"GyRo"})", "gyroX"},
                ModeCase{R"({"mode":" 1 "})", "gyroX"}}) {
            JsonDocument driver;
            assert(!deserializeJson(driver, row.json));
            for (bool asString : {false, true}) {
                JsonDocument parameters;
                parameters["periphId"] = "room"; parameters["sensorCategory"] = "mpu6050";
                parameters["unit"] = "C"; parameters["dataField"] = "propertyAlias";
                if (asString) parameters["driverParams"] = row.json;
                else parameters["driverParams"].set(driver.as<JsonObjectConst>());
                String field = "propertyAlias";
                assert(!thermostatTemperatureSource(peripheral, parameters.as<JsonObjectConst>(), field));
                assert(field == row.first); // actual cache key despite non-temperature channel
                parameters["dataField"] = row.first[0] == 'g' ? "gyro_x" : "accel_x";
                field = "propertyAlias";
                assert(!thermostatTemperatureSource(peripheral, parameters.as<JsonObjectConst>(), field));
                assert(field == row.first); // real snake-case alias resolution
                parameters["channelField"] = "temperature";
                serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
                String binding;
                assert(manager.thermostatSourceBindingLocked("room_temperature", binding));
                // Both MPU modes still expose the fourth temperature channel.
                auto duplicate = manager.rules["collect"];
                duplicate.id = "duplicate";
                parameters.remove("channelField"); parameters["dataField"] = "temperature";
                serializeJson(parameters, duplicate.actions[0].actionValue);
                manager.rules[duplicate.id] = duplicate;
                for (bool enabled : {true, false}) {
                    manager.rules["duplicate"].enabled = enabled;
                    assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
                }
                manager.rules.erase("duplicate");
            }
        }
        for (const char* invalid : {R"({"mode":true})", R"({"mode":[]})", R"({"mode":{}})",
                R"({"mode":"invalid"})", R"({"mode":2})", R"({"mode":-1})",
                R"({"mode":0.5})", R"({"mode":"1junk"})"}) {
            JsonDocument parameters;
            parameters["periphId"] = "room"; parameters["sensorCategory"] = "mpu6050";
            parameters["channelField"] = "temperature"; parameters["unit"] = "C";
            parameters["driverParams"] = invalid;
            serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
            String binding;
            assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
        }
        // Saved SENSOR_GENERIC configuration overrides legacy action mode.
        peripheral.type = PeripheralType::SENSOR_GENERIC;
        std::strcpy(peripheral.params.sensor.driver, "MPU6050");
        for (uint8_t configuredMode : {0, 1}) {
            peripheral.params.sensor.mode = configuredMode;
            String binding;
            assert(manager.thermostatSourceBindingLocked("room_temperature", binding));
        }
    } else if (!std::strcmp(mode, "source-alias-collision")) {
        auto& registry = FastBee::DriverRegistry::getInstance();
        for (const char* driver : {"SHT31", "BMP280"})
            assert(registry.registerDriver(driver, +[]() -> ISensorDriver* { return nullptr; }));
        auto& peripheral = PeripheralManager::getInstance().source;
        for (const char* driver : {"SHT31", "BMP280"}) for (bool configured : {false, true}) {
            peripheral.type = configured ? PeripheralType::SENSOR_GENERIC : PeripheralType::I2C;
            std::strcpy(peripheral.params.sensor.driver, configured ? driver : "");
            JsonDocument parameters;
            parameters["periphId"] = "room"; parameters["sensorCategory"] = driver;
            parameters["dataField"] = "temperature"; parameters["unit"] = "C";
            serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
            String binding;
            assert(manager.thermostatSourceBindingLocked("room_temperature", binding));
            auto duplicate = manager.rules["collect"]; duplicate.id = "duplicate";
            parameters["dataField"] = "propertyAlias";
            serializeJson(parameters, duplicate.actions[0].actionValue);
            manager.rules[duplicate.id] = duplicate;
            for (bool enabled : {true, false}) {
                manager.rules["duplicate"].enabled = enabled;
                // Old binding missed the alias writer; the real sampler maps
                // both producers onto the same default temperature cache key.
                assert(manager.legacyThermostatSourceBindingLocked("room_temperature", binding));
                assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
            }
            manager.rules.erase("duplicate");
        }
    } else if (!std::strcmp(mode, "source-scaling-types")) {
        for (const char* key : {"scaleFactor", "offset"}) {
            for (const char* invalid : {"null", "true", "\"2\"", "[]", "{}"}) {
                JsonDocument bad; assert(!deserializeJson(bad, invalid));
                JsonDocument parameters;
                parameters["periphId"] = "room"; parameters["channelField"] = "temperature";
                parameters["unit"] = "C"; parameters[key].set(bad.as<JsonVariantConst>());
                const float fallback = !std::strcmp(key, "scaleFactor") ? 1.0f : 0.0f;
                assert((parameters[key] | fallback) == fallback); // real old silent coercion
                serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
                String binding;
                assert(manager.legacyThermostatSourceBindingLocked("room_temperature", binding));
                assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
            }
            for (double valid : {0.0, -2.5, 0.25, 1.0,
                    double(std::numeric_limits<float>::max()),
                    -double(std::numeric_limits<float>::max())}) {
                JsonDocument parameters;
                parameters["periphId"] = "room"; parameters["channelField"] = "temperature";
                parameters["unit"] = "C"; parameters[key] = valid;
                assert(thermostatSourceScaling(parameters.as<JsonObjectConst>()));
                serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
                String binding;
                assert(manager.thermostatSourceBindingLocked("room_temperature", binding));
            }
            for (double invalid : {1e40, -1e40, std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()}) {
                JsonDocument parameters; parameters[key] = invalid;
                assert(!thermostatSourceScaling(parameters.as<JsonObjectConst>()));
            }
        }
        JsonDocument omitted;
        assert(thermostatSourceScaling(omitted.to<JsonObject>()));
        // Explicit null cannot energize a runtime even with a fresh plausible sample.
        manager.rules["collect"].actions[0].actionValue =
            R"({"periphId":"room","channelField":"temperature","unit":"C","scaleFactor":null})";
        tick(manager, 1000, false); tick(manager, 2000); tick(manager, 5000);
        assert(output.high && output.lows == 0);
        assert(manager._thermostats[0]->controller.phase() == FastBee::ThermostatPhase::SourceChanged);
    } else if (!std::strcmp(mode, "source-semantics")) {
        auto& registry = FastBee::DriverRegistry::getInstance();
        for (const char* name : {"SHT31", "BMP280", "MPU6050", "ADS1115"})
            assert(registry.registerDriver(name, +[]() -> ISensorDriver* { return nullptr; }));
        auto check = [&](const char* category, const char* field, const char* driver,
                         bool expected, bool engineering = false) {
            auto& source = PeripheralManager::getInstance().source;
            source.type = driver[0] ? PeripheralType::SENSOR_GENERIC : PeripheralType::I2C;
            std::strcpy(source.params.sensor.driver, driver);
            JsonDocument parameters;
            parameters["periphId"] = "room"; parameters["sensorCategory"] = category;
            parameters["channelField"] = field; parameters["dataField"] = field;
            parameters["unit"] = "℃"; parameters["scaleFactor"] = 0.25;
            parameters["offset"] = -10; parameters["mapToEngineering"] = engineering;
            serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
            String binding;
            const String ref = String("room_") + field;
            assert(manager.legacyThermostatSourceBindingLocked(ref, binding));
            assert(manager.thermostatSourceBindingLocked(ref, binding) == expected);
        };
        check("dht11", "humidity", "", false);
        check(" DHT22 ", "humidity", "", false);
        check("dht11", "temperature", "", true);
        check("dht22", "temperature", "", true);
        check("sht31", "humidity", "", false);
        check("sht31", "temperature", "", true);
        check("analog", "humidity", "SHT31", false); // actual driver overrides spoofed category
        check("analog", "temperature", "SHT31", true);
        check("bmp280", "pressure", "BMP280", false);
        check("bmp280", "temperature", "BMP280", true);
        check("mpu6050", "gyroX", "MPU6050", false);
        check("mpu6050", "temperature", "MPU6050", true);
        check("analog", "voltage", "ADS1115", false); // driver advertises V, not Celsius
        check("ntc", "temperature", "", true);
        check("ds18b20", "temperature", "", true);
        check("analog", "temperature", "", true);
        check("voltage", "temperature", "", true);
        check("current", "temperature", "", true);
        check("current_loop", "temperature", "", false);
        check("current_loop", "temperature", "", true, true);
        check("unknown", "temperature", "", false);
        manager.rules["collect"].actions[0].actionValue =
            R"({"periphId":"room","sensorCategory":"SHT31","dataField":"roomTemp","unit":"℃"})";
        String aliasBinding;
        assert(!manager.legacyThermostatSourceBindingLocked("room_temperature", aliasBinding));
        assert(manager.thermostatSourceBindingLocked("room_temperature", aliasBinding));
        // Explicit physical channel still overrides a property ID named temperature.
        manager.rules["collect"].actions[0].actionValue =
            R"({"periphId":"room","sensorCategory":"SHT31","dataField":"temperature","channelField":"humidity","unit":"℃"})";
        assert(manager.legacyThermostatSourceBindingLocked("room_humidity", aliasBinding));
        assert(!manager.thermostatSourceBindingLocked("room_humidity", aliasBinding));
        // Dedicated single-channel readers keep their existing periphId cache suffix.
        for (const char* category : {"ds18b20", "ntc"}) {
            JsonDocument parameters;
            parameters["periphId"] = "room"; parameters["sensorCategory"] = category;
            parameters["unit"] = "C";
            serializeJson(parameters, manager.rules["collect"].actions[0].actionValue);
            assert(manager.legacyThermostatSourceBindingLocked("room_room", aliasBinding));
            assert(manager.thermostatSourceBindingLocked("room_room", aliasBinding));
        }
    } else if (!std::strcmp(mode, "source-humidity-inert") ||
               !std::strcmp(mode, "source-generic-humidity-inert")) {
        const bool configured = !std::strcmp(mode, "source-generic-humidity-inert");
        if (configured) {
            auto& source = PeripheralManager::getInstance().source;
            source.type = PeripheralType::SENSOR_GENERIC;
            std::strcpy(source.params.sensor.driver, "SHT31");
            assert(FastBee::DriverRegistry::getInstance().registerDriver("SHT31",
                +[]() -> ISensorDriver* { return nullptr; }));
        }
        manager.rules["collect"].actions[0].actionValue = configured
            ? R"({"periphId":"room","sensorCategory":"analog","dataField":"humidity","channelField":"humidity","unit":"℃"})"
            : R"({"periphId":"room","sensorCategory":"dht11","dataField":"humidity","unit":"℃"})";
        JsonDocument control;
        assert(!deserializeJson(control, manager.rules["control"].actions[0].actionValue.c_str()));
        control["sourceRef"] = "room_humidity";
        serializeJson(control, manager.rules["control"].actions[0].actionValue);
        String binding;
        assert(manager.legacyThermostatSourceBindingLocked("room_humidity", binding));
        assert(!manager.thermostatSourceBindingLocked("room_humidity", binding));
        // A plausible 20-percent sample with the old erroneous Celsius unit
        // must never make the actual runtime request an active LOW.
        manager.observation.unit = "℃";
        manager.observation.sourceRevision = "room:humidity:℃";
        tick(manager, 1000, false); tick(manager, 2000); tick(manager, 5000);
        assert(output.high && output.lows == 0);
        assert(manager._thermostats[0]->controller.phase() == FastBee::ThermostatPhase::SourceChanged);
    } else if (!std::strcmp(mode, "source-unavailable")) {
        String binding;
        auto& pm = PeripheralManager::getInstance();
        pm.snapshotAvailable = false;
        assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
        pm.snapshotAvailable = true; pm.source.enabled = false;
        assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
        pm.source.enabled = true; pm.mutex.allowed = false;
        assert(!manager.thermostatSourceBindingLocked("room_temperature", binding));
    } else if (!std::strcmp(mode, "shutdown")) {
        running(manager); output.releaseFails = true;
        assert(!manager.shutdownThermostats());
        const auto lowCount = output.lows, claims = output.claims;
        tick(manager, 3000); assert(output.lows == lowCount && output.claims == claims);
        output.releaseFails = false; tick(manager, 4000);
        assert(output.high && !output.owner && !manager._thermostats[0]);
        assert(manager.shutdownThermostats());
        tick(manager, 5000); assert(output.claims == claims && output.lows == lowCount);
    } else if (!std::strcmp(mode, "shutdown-lock")) {
        running(manager); manager.rulesMutex.allowed = false;
        assert(!manager.shutdownThermostats()); assert(manager._thermostatShutdown.load());
        manager.rulesMutex.allowed = true; tick(manager, 3000);
        assert(output.high && !output.owner);
    } else if (!std::strcmp(mode, "partial-claim")) {
        output.claimFails = true; output.releaseFails = true;
        tick(manager, 1000); assert(output.owner && output.releases == 1 && output.lows == 0);
        output.releaseFails = false; tick(manager, 2000);
        assert(!output.owner && output.high && manager._thermostats[0]);
        assert(!manager._thermostats[0]->admitted && manager._thermostats[0]->outputConfirmed);
        tick(manager, 3000); assert(output.claims == 1 && output.lows == 0);
    } else if (!std::strcmp(mode, "source-epoch")) {
        running(manager); FastBee::SensorConfigEpoch::advance(); tick(manager, 3000);
        assert(output.high && manager._thermostats[0]->controller.phase() == FastBee::ThermostatPhase::SourceChanged);
        tick(manager, 6000); assert(output.high && output.lows == 1);
    } else if (!std::strcmp(mode, "source-calibration")) {
        running(manager);
        manager.rules["collect"].actions[0].actionValue = R"({"periphId":"room","channelField":"temperature","unit":"C","scaleFactor":2})";
        manager._ruleRuntimeRevision = 2; manager.rules["control"].runtimeRevision = 2;
        manager.retireThermostatsLocked(); assert(output.high);
        tick(manager, 3000); assert(output.high && output.lows == 1);
    } else if (!std::strcmp(mode, "memory")) {
        running(manager); FastBeeFramework::getInstance()->health.level = MemoryGuardLevel::SEVERE;
        tick(manager, 3000, false); assert(output.high);
        FastBeeFramework::getInstance()->health.level = MemoryGuardLevel::NORMAL;
        tick(manager, 4000, false); assert(output.high);
        tick(manager, 5000); assert(!output.high);
    } else if (!std::strcmp(mode, "diagnostics")) {
        running(manager);
        JsonDocument doc; const String json = manager.getThermostatDiagnosticsJson();
        assert(!deserializeJson(doc, json.c_str()));
        assert(doc[0]["outputOn"].as<bool>() && doc[0]["padConfirmed"].as<bool>());
        assert(std::strcmp(doc[0]["physicalFeedback"] | "", "NOT_OBSERVED") == 0);
        manager.rulesMutex.allowed = false; assert(manager.getThermostatDiagnosticsJson() == "null");
    } else return 2;
    return 0;
}
