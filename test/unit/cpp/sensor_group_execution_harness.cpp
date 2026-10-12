#include <ArduinoJson.h>
#include "core/UartEventOrigin.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#define assert(expression) do { if (!(expression)) { std::fprintf(stderr,"assertion: %d %s\n",__LINE__,#expression); std::exit(2); } } while (0)
struct String : std::string {
    using std::string::string;
    String(const std::string& value) : std::string(value) {}
    bool isEmpty() const { return empty(); }
};
// ESP32 Arduino String does not provide this reverse comparison. The STL
// inheritance used by this small harness must not silently add that API.
bool operator!=(const char*, const String&) = delete;
// Production builds use Arduino String support; this adapter exposes the same
// parsed action parameters to the real ArduinoJson parser in the host harness.
DeserializationError deserializeJson(JsonDocument& doc, const String& text) {
    return deserializeJson(doc, text.c_str());
}
#include "sensor_group_types.inc"
enum class ExecActionType : uint8_t { ACTION_SENSOR_READ = 19, CONTROL = 2 };
enum class PeripheralType { SENSOR_GENERIC, I2C };
struct ExecAction {
    String targetPeriphId = "sensor", actionValue;
    uint8_t actionType = 19, execMode = 0;
    bool useReceivedValue = false;
    uint16_t syncDelayMs = 0;
};
struct PeriphExecRule { std::vector<ExecAction> actions; bool recordHistory = true; };
struct ActionExecResult {
    String targetPeriphId, actualValue, dataField, remark;
    bool success = false;
};
struct PeripheralConfig {
    bool enabled = true;
    PeripheralType type = PeripheralType::SENSOR_GENERIC;
    struct { struct { const char* driver = "SHT30"; uint8_t mode = 0, scanMask = 0; } sensor; } params;
};
struct PeripheralManager {
    bool current = true, busy = false;
    static PeripheralManager& getInstance() { static PeripheralManager instance; return instance; }
    bool getPeripheralSnapshot(const String&, PeripheralConfig&) { return true; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) {
        return origin.unbound() || (current && !busy && origin.matches(7,19));
    }
};
struct Metadata { uint8_t count = 2; uint8_t activeChannelCount(uint8_t, uint8_t = 0) const { return count; } } metadata;
namespace FastBee {
struct DriverRegistry {
    static DriverRegistry& getInstance() { static DriverRegistry instance; return instance; }
    const Metadata* getMetadata(const char*) { return &metadata; }
};
namespace MemoryBudget { constexpr uint32_t PERIPH_EXEC_MIN_DRAM = 4096; }
bool resolveSensorChannel(const char*, uint8_t, const char* input, String& output, String&, uint8_t = 0) {
    if (strcmp(input,"temperature") && strcmp(input,"humidity") && strcmp(input,"pressure")) return false;
    output = input; return true;
}
}
enum { MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2 };
static bool memoryLow = false, deadline = false;
uint32_t heap_caps_get_free_size(int) { return memoryLow ? 0 : 64000; }
bool operationDeadlineReached(unsigned long) { return deadline; }
struct Monitor { bool isMemoryCritical() { return memoryLow; } } monitor;
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { static FastBeeFramework instance; return &instance; }
    Monitor* getHealthMonitor() { return &monitor; }
};
#define FASTBEE_ENABLE_LCD 0
struct PeriphExecManager {
    size_t publications = 0, snapshots = 0;
    bool commitResult = true;
    std::vector<String> selected;
    static PeriphExecManager& getInstance() { static PeriphExecManager instance; return instance; }
    bool upsertSensorObservationBatch(const ObservationUpdate* const* values,size_t count,uint32_t epoch) {
        assert(count == 2 && epoch == 7);
        assert(values[0]->sampleTimestamp == 100 && values[1]->sampleTimestamp == (values[0]->sampleGroup ? 101U : 100U));
        assert(values[0]->maxAgeMs == 10000 && values[1]->maxAgeMs == 10000);
        ++publications; return commitResult;
    }
    std::vector<ObservationSnapshot> getObservationSnapshots(const std::vector<const char*>& sources) {
        ++snapshots; selected.clear();
        for (const auto* source : sources) selected.emplace_back(source);
        return {};
    }
};
struct ScreenPanelSnapshot { static constexpr uint8_t MAX_PAGES = 8, MAX_CARDS = 4; };
class PeriphExecExecutor {
public:
    size_t reads = 0;
    int fault = 0;
    bool executeSensorReadGroup(const PeriphExecRule&,size_t,size_t,uint32_t,unsigned long,std::vector<ActionExecResult>&,
                                const FastBee::UartEventOrigin& = {},bool* = nullptr);
    bool executeSensorReadAction(const ExecAction& action, ActionExecResult& result, bool history,
                                const uint32_t* collection, PreparedSensorRead* prepared,
                                const FastBee::UartEventOrigin& origin, bool* sourceUnconfirmed) {
        if (sourceUnconfirmed) *sourceUnconfirmed = false;
        prepared->observation.uartOrigin = origin;
        assert(prepared && collection && *collection == 90 && history);
        ++reads;
        result.actualValue = "23.5"; result.targetPeriphId = action.targetPeriphId;
        prepared->observation.sampleTimestamp = reads == 2 && (fault == 2 || fault >= 10) ? 101 : 100;
        prepared->observation.sampleGroup = fault >= 10 ? (reads == 2 && fault == 11 ? 0 : reads == 2 && fault == 12 ? 74 : 73) : 0;
        prepared->observation.maxAgeMs = 10000;
        prepared->configVersion = reads == 2 && fault == 3 ? 8 : 7;
        if (reads == 1 && fault == 4) memoryLow = true;
        if (reads == 1 && fault == 5) deadline = true;
        if ((reads == 1 && fault == 7) || (reads == 2 && fault == 8))
            PeripheralManager::getInstance().current = false;
        return !(reads == 2 && fault == 1);
    }
};
#include "sensor_group_methods.inc"
static ExecAction action(const char* channel,const char* mapping = "") {
    ExecAction value;
    value.actionValue = String("{\"periphId\":\"sensor\",\"channelField\":\"") + channel +
        "\",\"dataField\":\"" + mapping + "\"}";
    return value;
}
static void groupingBoundaries() {
    PeriphExecRule rule{{action("temperature"),action("humidity")}};
    assert(sensorReadGroupSize(rule,0) == 2);
    rule.actions[1].syncDelayMs = 1; assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions[1] = action("humidity"); rule.actions[1].useReceivedValue = true;
    assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions[1] = action("humidity"); rule.actions[1].execMode = 1;
    assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions[1] = action("humidity"); rule.actions[1].targetPeriphId = "other";
    assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions[1] = action("humidity"); rule.actions[1].actionType = 2;
    assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions[1] = action("temperature"); assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions = {action("temperature","same"),action("humidity","same")};
    assert(sensorReadGroupSize(rule,0) == 1);
    rule.actions = {action("temperature"),action("humidity"),action("pressure")};
    assert(sensorReadGroupSize(rule,0) == 2); // actual driver has only two channels.
    metadata.count = 1; assert(sensorReadGroupSize(rule,0) == 1); metadata.count = 2;
    rule.actions.resize(1); assert(sensorReadGroupSize(rule,0) == 1);
}
static void groupExecution() {
    PeriphExecRule rule{{action("temperature"),action("humidity")}};
    auto& manager = PeriphExecManager::getInstance();
    for (int fault = 0; fault <= 6; ++fault) {
        PeriphExecExecutor executor; executor.fault = fault;
        memoryLow = deadline = false;
        manager.commitResult = fault != 6;
        const auto before = manager.publications;
        std::vector<ActionExecResult> result;
        const bool ok = executor.executeSensorReadGroup(rule,0,2,90,0,result);
        assert(ok == (fault == 0)); assert(result.size() == 2);
        assert(result[0].success == ok && result[1].success == ok);
        if (!ok) assert(result[0].actualValue.empty() && result[1].actualValue.empty());
        assert(manager.publications - before == static_cast<size_t>(fault == 0 || fault == 6));
        assert(executor.reads == static_cast<size_t>(fault == 4 || fault == 5 ? 1 : 2));
    }
}
static void scanGroupExecution() {
    PeriphExecRule rule{{action("temperature"),action("humidity")}};
    auto& manager=PeriphExecManager::getInstance();
    memoryLow=deadline=false;manager.commitResult=true;
    for(int fault=10;fault<=12;++fault){
        PeriphExecExecutor executor;executor.fault=fault;
        std::vector<ActionExecResult> result;
        const bool ok=executor.executeSensorReadGroup(rule,0,2,90,0,result);
        assert(ok==(fault==10));
    }
}
static void panelSourceBounds() {
    JsonDocument document;
    assert(!deserializeJson(document,R"({"pages":[
        {"cards":[{"sourceRef":"inactive"},{"sourceRef":"system.uptime"}]},
        {"cards":[{"sourceRef":"sensor_temperature","sourceRefs":["sensor_temperature","sensor_humidity","weather.city"]},
                  {"sourceRef":"sensor_humidity"},{"sourceRef":"outside_global_card_limit"}]},
        {"cards":[{"sourceRef":"later_outside_limit"}]}]})"));
    auto& manager = PeriphExecManager::getInstance();
    panelObservationSnapshots(document["pages"].as<JsonArrayConst>(),1,true);
    assert(manager.snapshots == 1);
    assert(manager.selected == std::vector<String>({"sensor_temperature","sensor_humidity"}));
    panelObservationSnapshots(document["pages"].as<JsonArrayConst>(),1,false);
    assert(manager.snapshots == 2);
    assert(manager.selected == std::vector<String>({"inactive","sensor_temperature","sensor_humidity"}));
}
static void groupUartOrigin() {
    PeriphExecRule rule{{action("temperature"),action("humidity")}};
    auto& manager = PeriphExecManager::getInstance();
    auto& pm = PeripheralManager::getInstance();
    for (int scenario = 0; scenario < 7; ++scenario) {
        pm = {}; memoryLow = deadline = false; manager.commitResult = true;
        PeriphExecExecutor executor;
        auto origin = FastBee::UartEventOrigin::captured(7,19);
        if (scenario == 1) pm.current = false;
        if (scenario == 2) origin.required = false;
        if (scenario == 3) executor.fault = 7;
        if (scenario == 4) executor.fault = 8;
        if (scenario == 5) pm.busy = true;
        if (scenario == 6) { origin = {}; pm.current = false; }
        bool rejected = true;
        const size_t before = manager.publications;
        std::vector<ActionExecResult> results;
        const bool ok = executor.executeSensorReadGroup(rule,0,2,90,0,results,origin,&rejected);
        const bool valid = scenario == 0 || scenario == 6;
        assert(ok == valid && rejected != valid);
        assert(manager.publications - before == static_cast<size_t>(valid));
        assert(executor.reads == (valid || scenario == 4 ? 2U : scenario == 3 ? 1U : 0U));
        assert(results.size() == 2);
        for (const auto& result : results) {
            assert(result.success == valid);
            assert(result.remark == (valid ? "success" : "uart_source_unconfirmed"));
            if (!valid) assert(result.actualValue.empty());
        }
    }
}
int main() {
    groupingBoundaries(); groupExecution(); scanGroupExecution(); panelSourceBounds(); groupUartOrigin();
    std::printf("PreparedSensorRead host sizeof=%zu; at most four objects on bounded heap, not worker stack\n",sizeof(PreparedSensorRead));
}
