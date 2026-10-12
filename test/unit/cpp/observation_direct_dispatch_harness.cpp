#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/UartEventOrigin.h"
#include <cassert>
#include <cmath>
#include <string>
#undef malloc
#undef free
enum class ObservationQuality { UNKNOWN, GOOD, ERROR };
struct ObservationUpdate {
    String sourceRef, label, value, unit, valueType, sourceRevision;
    FastBee::UartEventOrigin uartOrigin;
    ObservationQuality quality = ObservationQuality::GOOD;
    bool recordable = true;
    uint32_t sampleIdentity = 13, sampleTimestamp = 17;
};
struct PeripheralManager {
    bool current = true;
    static PeripheralManager& getInstance() { static PeripheralManager pm; return pm; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin&) { return current; }
};
namespace FastBee {
struct PointHistory {
    unsigned count = 0; double value = 0;
    static PointHistory& instance() { static PointHistory history; return history; }
    void recordSample(const String&, double numeric, const String&, const String&, uint32_t id, uint32_t time) {
        assert(id == 13 && time == 17); ++count; value = numeric;
    }
};
}
class PeriphExecManager;
class PeriphExecScheduler {
public:
    PeriphExecManager* _manager = nullptr;
    void handlePollArray(const String&, JsonArray&, const FastBee::UartEventOrigin& = {});
};
class PeriphExecManager {
public:
    PeriphExecScheduler* _scheduler = nullptr;
    unsigned sse = 0, dispatches = 0;
    String seenId, seenValue, seenSource;
    FastBee::UartEventOrigin seenOrigin;
    void notifySensorDataSSE(const String&, const String&, const String&, const String&, const ObservationUpdate*) { ++sse; }
    void publishObservation(const ObservationUpdate&);
    void processPollDataMatch(JsonArray& arr, const String& source, const FastBee::UartEventOrigin& origin) {
        assert(arr.size() == 1 && arr[0].size() == 2);
        assert(arr[0]["id"].is<const char*>() && arr[0]["value"].is<const char*>());
        seenId = arr[0]["id"].as<const char*>(); seenValue = arr[0]["value"].as<const char*>();
        seenSource = source; seenOrigin = origin; ++dispatches;
    }
};
// Any JSON reparse in these production methods is a compile failure.
#define deserializeJson FASTBEE_FORBIDDEN_deserializeJson
#include "production_direct_dispatch.inc"
#undef deserializeJson
int main() {
    PeriphExecManager manager; PeriphExecScheduler scheduler;
    scheduler._manager = &manager; manager._scheduler = &scheduler;
    ObservationUpdate update;
    update.sourceRef = "modbus.温度\"\\";
    update.uartOrigin = FastBee::UartEventOrigin::captured(7,19);
    for (const char* value : {"-12.50", "0", "-0.000000", " 42.25\t", "NaN", "Infinity", "12oops", "", "文本\"\\"}) {
        update.value = value;
        JsonDocument oldDoc, parsed;
        auto oldArr = oldDoc.to<JsonArray>(); auto item = oldArr.add<JsonObject>();
        item["id"] = update.sourceRef; item["value"] = update.value;
        std::string wire; serializeJson(oldDoc, wire);
        assert(!deserializeJson(parsed, wire));
        manager.publishObservation(update);
        assert(manager.seenId == parsed[0]["id"].as<const char*>());
        assert(manager.seenValue == parsed[0]["value"].as<const char*>());
        assert(manager.seenSource == "sensor_cache" && manager.seenOrigin.matches(7,19));
    }
    assert(manager.dispatches == 9 && manager.sse == 9);
    assert(FastBee::PointHistory::instance().count == 4);
    const unsigned dispatches = manager.dispatches;
    for (auto quality : {ObservationQuality::UNKNOWN, ObservationQuality::ERROR}) {
        update.quality = quality; manager.publishObservation(update);
        assert(manager.dispatches == dispatches && manager.sse == dispatches);
    }
    update.quality = ObservationQuality::GOOD;
    PeripheralManager::getInstance().current = false;
    manager.publishObservation(update); assert(manager.dispatches == dispatches);
    PeripheralManager::getInstance().current = true;
    scheduler._manager = nullptr;
    JsonDocument doc; auto arr = doc.to<JsonArray>();
    scheduler.handlePollArray("sensor_cache", arr); assert(manager.dispatches == dispatches);
    std::puts("production direct dispatch: wire, quality, origin and numeric history passed");
}
