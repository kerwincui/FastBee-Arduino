#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/AsyncExecTypes.h"
#include "core/PointHistory.h"
#define private public
#include "core/ScreenPanelManager.h"
#undef private

inline void convertFromJson(JsonVariantConst value, String& output) {
    output = value.as<std::string>();
}
#include "snapshot_observation_types.inc"
struct MotorControlState { bool known=false, pending=false; uint16_t speed=0,pulse=0;uint8_t precision=0;uint32_t updatedMs=0; char error[64]{},lastAction[16]{}; };
struct MotorControlService { static MotorControlService& instance(){ static MotorControlService s;return s; } bool state(const String&,MotorControlState&){return false;} };


static unsigned poolAllocations = 0;
static bool failPoolAllocation = false;
static std::map<void*, size_t> pools;
enum { MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4 };
[[maybe_unused]] static void* heap_caps_malloc(size_t bytes, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++poolAllocations;
    if (failPoolAllocation) return nullptr;
    void* result = malloc(bytes);
    if (result) pools[result] = bytes;
    return result;
}
[[maybe_unused]] static void* heap_caps_realloc(void* memory, size_t bytes, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++poolAllocations;
    if (failPoolAllocation) return nullptr;
    void* result = realloc(memory, bytes);
    if (result) { pools.erase(memory); pools[result] = bytes; }
    return result;
}
[[maybe_unused]] static void heap_caps_free(void* memory) { pools.erase(memory); free(memory); }
static size_t heap_caps_get_free_size(uint32_t) { return 32000; }
static int64_t esp_timer_get_time() { return 60000000; }
enum class NetworkStatus { CONNECTED, DISCONNECTED, CONNECTING, AP_MODE, CONNECTION_FAILED };
struct NetworkInfo { NetworkStatus status = NetworkStatus::CONNECTED; };
struct FBNetworkManager { NetworkInfo getStatusInfo() const { return {}; } };
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { static FastBeeFramework value; return &value; }
    FBNetworkManager* getNetworkManager() { static FBNetworkManager value; return &value; }
};
constexpr int WL_CONNECTED = 3;
struct SnapshotWifi { int status() const { return WL_CONNECTED; } int RSSI() const { return -42; } } WiFi;
struct SystemRebooter {
    static bool scheduled;
    static bool isScheduled() { return scheduled; }
};
bool SystemRebooter::scheduled = false;

struct PeriphExecManager {
    std::map<std::string, ObservationSnapshot> observations;
    std::vector<AsyncExecResult> results;
    unsigned observationReads = 0, resultReads = 0;
    static PeriphExecManager& getInstance() { static PeriphExecManager value; return value; }
    bool getObservation(const String& ref, ObservationSnapshot& out) {
        ++observationReads;
        const auto found = observations.find(ref.c_str());
        if (found == observations.end()) return false;
        out = found->second;
        return true;
    }
    std::vector<ObservationSnapshot> getObservationSnapshots(const std::vector<const char*>& sources) {
        ++observationReads;
        std::vector<ObservationSnapshot> copied;
        for (const char* source : sources) {
            const auto found = observations.find(source);
            if (found == observations.end()) continue;
            copied.push_back(found->second);
            copied.back().sourceRef = source;
        }
        return copied;
    }
    std::vector<AsyncExecResult> getRecentResults() { ++resultReads; return results; }
};
namespace FastBee {
struct RemoteResourceManager {
    std::map<std::string, JsonDocument> values;
    unsigned reads = 0;
    bool changeSensorPairOnRead = false;
    static RemoteResourceManager& instance() { static RemoteResourceManager value; return value; }
    bool getValue(const String& ref, JsonObject out) {
        ++reads;
        if (changeSensorPairOnRead) {
            changeSensorPairOnRead = false;
            for (const char* source : {"sensor.temp", "sensor.humidity"}) {
                auto& sample = PeriphExecManager::getInstance().observations[source];
                sample.value = "new"; sample.sampleTimestamp = 456;
            }
        }
        const auto found = values.find(ref.c_str());
        if (found == values.end()) return false;
        out.set(found->second.as<JsonObjectConst>());
        return true;
    }
};
static std::map<std::string, PointHistorySeriesSnapshot> histories;
static unsigned historyReads = 0;
PointHistory& PointHistory::instance() { static PointHistory value; return value; }
bool PointHistory::getSeries(const String& ref, PointHistorySeriesSnapshot& out) const {
    ++historyReads;
    const auto found = histories.find(ref.c_str());
    if (found == histories.end()) return false;
    out = found->second;
    return true;
}
}

#include "screen_snapshot_production.inc"

struct Fixture {
    ScreenPanelManager manager;
    JsonDocument layout;
    std::unique_ptr<ScreenPanelSnapshot> out{new ScreenPanelSnapshot};
    ScreenPanelSnapshotCache cache;
    Fixture() {
        manager._initialized = true;
        manager._mutex = reinterpret_cast<void*>(1);
        manager._activePageId = "home";
        assert(!deserializeJson(layout, R"json({"settings":{"brightnessPercent":70,"idleSleepSeconds":0,"themeId":"dark","background":{"presetId":"solid"}},"pages":[
          {"id":"home","title":"Home","logicalWidth":480,"logicalHeight":480,"cards":[
            {"id":"temp","title":"Temperature","kind":"data","visualType":"value","sourceRef":"sensor.temp","unit":"override","rect":{"x":10,"y":20,"w":120,"h":80}},
            {"id":"link","kind":"page-link","visualType":"navigation","pageId":"hidden"}]},
          {"id":"hidden","title":"Hidden","cards":[
            {"id":"weather","kind":"weather","visualType":"weather","sourceRef":"weather.city"},
            {"id":"trend","kind":"chart","visualType":"line","sourceRef":"sensor.temp","historyMode":"recent10","sourceRefs":["sensor.temp","remote.lamp"]},
            {"id":"switch","kind":"control","visualType":"switch","sourceRef":"remote.lamp","controlRef":"rule.lamp"},
            {"id":"restart","kind":"system","visualType":"action","requireConfirmation":true}]}]})json"));
        publish();
        auto& observation = PeriphExecManager::getInstance().observations["sensor.temp"];
        observation.value = "23.5"; observation.unit = "C";
        observation.label = "Temperature"; observation.quality = ObservationQuality::GOOD;
        observation.sampleTimestamp = 123;
        auto& remote = FastBee::RemoteResourceManager::instance().values["remote.lamp"];
        remote["quality"] = "good"; remote["value"] = true; remote["pending"] = false;
        auto& weather = FastBee::RemoteResourceManager::instance().values["weather.city"];
        assert(!deserializeJson(weather, R"json({"quality":"good","updatedMs":123,"unit":"C","weather":{"condition":"sunny","city":"Shanghai","temperature":23.5,"forecast":[{"date":"2026-09-20","condition":"rain","low":19.0,"high":25.0}]}})json"));
        auto& history = FastBee::histories["sensor.temp"];
        history.count = 2; history.samples[0].value = 21; history.samples[1].value = 22;
        history.samples[0].sequence = 1; history.samples[1].sequence = 2;
    }
    void publish() {
        manager._configJson = "";
        serializeJson(layout, manager._configJson);
        ++manager._revision;
    }
    void refresh() { assert(manager.getActiveSnapshot(*out, cache)); }
    void hidden() { manager._activePageId = "hidden"; refresh(); }
};

static void reuse() {
    Fixture f; f.refresh();
    assert(f.cache.rebuildCount() == 1 && f.cache.refreshedCardCount() == 2);
    const auto allocations = poolAllocations;
    const auto hiddenBefore = f.out->cards[2];
    // Same revision must not parse/copy this source again.
    f.manager._configJson = "this would fail deserialization";
    for (unsigned i = 0; i < 100; ++i) f.refresh();
    assert(f.cache.rebuildCount() == 1 && poolAllocations == allocations);
    assert(FastBee::historyReads == 0 && FastBee::RemoteResourceManager::instance().reads == 0);
    assert(PeriphExecManager::getInstance().resultReads == 0);
    assert(memcmp(&hiddenBefore, &f.out->cards[2], sizeof(hiddenBefore)) == 0);
    assert(strcmp(f.out->cards[0].value, "23.5") == 0);
    assert(f.out->pageCount == 2 && f.out->cardCount == 6);
}

static void pages() {
    Fixture f;
    for (int i = 2; i < 8; ++i) {
        auto page = f.layout["pages"].add<JsonObject>();
        page["id"] = std::string("page") + std::to_string(i); page["title"] = "Next";
        auto card = page["cards"].add<JsonObject>(); card["id"] = std::string("clock") + std::to_string(i);
        card["kind"] = "clock"; card["visualType"] = "clock";
    }
    f.publish(); f.refresh();
    for (int i = 0; i < 8; ++i) {
        f.manager._activePageId = f.layout["pages"][i]["id"].as<String>(); f.refresh();
        assert(f.out->activePageIndex == i && f.out->pageCount == 8);
        assert(f.out->pages[i].cardCount == (i == 0 ? 2 : i == 1 ? 4 : 1));
        assert(strcmp(f.out->pages[i].id, f.manager._activePageId.c_str()) == 0);
    }
    assert(f.cache.rebuildCount() == 1);
    assert(strcmp(f.out->cards[1].targetPageId, "hidden") == 0);
}

static void sensor() {
    Fixture f; f.refresh(); auto& data = PeriphExecManager::getInstance().observations["sensor.temp"];
    data.quality = ObservationQuality::STALE; data.error = "long old error"; data.value = "100.55"; f.refresh();
    assert(f.out->cards[0].quality == ScreenValueQuality::STALE);
    data.quality = ObservationQuality::ERROR; data.error = "E"; data.value = "1"; f.refresh();
    assert(strcmp(f.out->cards[0].error, "E") == 0 && f.out->cards[0].error[2] == 0);
    assert(f.out->cards[0].quality == ScreenValueQuality::ERROR);
    assert(strcmp(f.out->cards[0].unit, "override") == 0);
    PeriphExecManager::getInstance().observations.clear(); f.refresh();
    assert(f.out->cards[0].quality == ScreenValueQuality::UNKNOWN && !f.out->cards[0].value[0]);
    assert(!f.out->cards[0].error[0] && f.out->cards[0].updatedMs == 0);
    assert(strcmp(f.out->cards[0].title, "Temperature") == 0 && f.out->cards[0].rect.x == 10);
    f.layout["pages"][0]["cards"][0]["unit"] = ""; f.publish();
    auto& restored = PeriphExecManager::getInstance().observations["sensor.temp"];
    restored.value = "4"; restored.unit = "F"; f.refresh();
    assert(strcmp(f.out->cards[0].unit, "F") == 0);
}

static void weather() {
    Fixture f; f.hidden(); auto& manager = FastBee::RemoteResourceManager::instance();
    assert(f.out->cards[2].hasWeatherTemperature);
    assert(strcmp(f.out->cards[2].weatherCity, "Shanghai") == 0);
    manager.values["remote.lamp"]["pending"] = true; manager.values["remote.lamp"]["quality"] = "stale"; f.refresh();
    assert(f.out->cards[4].pending && f.out->cards[4].quality == ScreenValueQuality::STALE);
    manager.values["weather.city"].clear(); manager.values["weather.city"]["quality"] = "error";
    manager.values["weather.city"]["error"] = "timeout";
    manager.values["remote.lamp"]["pending"] = false; f.refresh();
    const auto& out = f.out->cards[2];
    assert(!out.hasWeatherTemperature && !out.weatherCity[0] && !out.value[0]);
    assert(out.quality == ScreenValueQuality::ERROR);
    assert(!f.out->cards[4].pending);
    manager.values.erase("weather.city"); f.refresh();
    assert(f.out->cards[2].quality == ScreenValueQuality::UNKNOWN && !f.out->cards[2].error[0]);
}

static void history() {
    Fixture f; f.hidden(); assert(f.out->cards[3].historyCount == 2 && f.out->cards[3].seriesCount == 2);
    auto& value = FastBee::histories["sensor.temp"];
    value.count = 1; value.stale = true; value.staleReason = "remapped"; value.samples[0].restored = true; f.refresh();
    const auto& output = f.out->cards[3];
    assert(output.historyCount == 1 && output.history[0].restored && output.history[1].value == 0);
    assert(output.historyStale && strcmp(output.historyError, "remapped") == 0);
    assert(strcmp(output.series[0].label, "Temperature") == 0 && strcmp(output.series[1].value, "true") == 0);
    FastBee::histories.clear(); PeriphExecManager::getInstance().observations.clear(); f.refresh();
    assert(!output.historyCount && !output.historyStale && !output.historyError[0]);
    assert(!output.series[0].value[0] && output.series[0].quality == ScreenValueQuality::UNKNOWN);
}

static void operations() {
    Fixture f; auto& exec = PeriphExecManager::getInstance();
    f.manager._operationIds[0] = 42; f.manager._operationCardIds[0] = "switch";
    f.refresh(); assert(exec.resultReads == 0);
    AsyncExecResult result; result.operationId = 42; result.status = AsyncExecStatus::RUNNING;
    exec.results.push_back(result); f.hidden(); assert(f.out->cards[4].pending && exec.resultReads == 1);
    exec.results[0].status = AsyncExecStatus::FAILED; exec.results[0].errorCode = AsyncExecError::ACTION_FAILED; f.refresh();
    assert(!f.out->cards[4].pending && strcmp(f.out->cards[4].error, "action_failed") == 0);
    const auto readCount = exec.resultReads; exec.results.clear(); f.refresh();
    assert(exec.resultReads == readCount && strcmp(f.out->cards[4].error, "action_failed") == 0);
    f.manager._operationIds[0] = 43; f.refresh(); assert(f.out->cards[4].pending && !f.out->cards[4].error[0]);
    f.manager._operationIds[0] = 0;
    f.manager._operationIds[1] = 0x80000001UL; f.manager._operationCardIds[1] = "restart";
    SystemRebooter::scheduled = true; f.refresh(); assert(f.out->cards[5].pending);
    const auto beforeRestart = exec.resultReads;
    SystemRebooter::scheduled = false; f.manager._restartSaveFailed = true; f.refresh();
    assert(exec.resultReads == beforeRestart && !f.out->cards[5].pending);
    assert(strcmp(f.out->cards[5].error, "history_save_failed") == 0);
}

static void revision() {
    Fixture f; f.hidden(); f.layout["pages"].as<JsonArray>().remove(1);
    f.layout["settings"]["brightnessPercent"] = 35; f.layout["pages"][0]["title"] = "Changed"; f.publish(); f.refresh();
    assert(f.cache.rebuildCount() == 2 && f.out->pageCount == 1 && f.out->cardCount == 2);
    assert(f.out->activePageIndex == 0 && f.out->brightnessPercent == 35 && !f.out->cards[2].id[0]);
    assert(!f.out->pages[1].id[0] && strcmp(f.out->pages[0].title, "Changed") == 0);
    auto other = std::make_unique<ScreenPanelSnapshot>();
    assert(f.manager.getActiveSnapshot(*other, f.cache) && f.cache.rebuildCount() == 3);
    assert(other->cardCount == 2 && strcmp(other->cards[0].value, "23.5") == 0);
    f.cache.invalidate(); assert(f.manager.getActiveSnapshot(*other, f.cache) && f.cache.rebuildCount() == 4);
}

static void failure() {
    Fixture f; f.refresh(); const auto previous = *f.out;
    f.manager._configJson = "{"; ++f.manager._revision;
    assert(!f.manager.getActiveSnapshot(*f.out, f.cache));
    assert(memcmp(f.out.get(), &previous, sizeof(previous)) == 0);
    f.publish();
#if FASTBEE_USE_PSRAM
    failPoolAllocation = true;
    assert(!f.manager.getActiveSnapshot(*f.out, f.cache));
    assert(memcmp(f.out.get(), &previous, sizeof(previous)) == 0);
    failPoolAllocation = false;
#endif
    f.refresh(); assert(f.cache.rebuildCount() == 2);
}

static void consistent() {
    Fixture f;
    auto cards = f.layout["pages"][0]["cards"].to<JsonArray>();
    for (const char* source : {"sensor.temp", "remote.lamp", "sensor.humidity"}) {
        auto card = cards.add<JsonObject>();
        card["id"] = source; card["kind"] = "data"; card["sourceRef"] = source;
    }
    auto chart = cards.add<JsonObject>();
    chart["id"] = "pair_chart"; chart["kind"] = "chart"; chart["sourceRef"] = "sensor.temp";
    chart["sourceRefs"].to<JsonArray>().add("sensor.temp");
    chart["sourceRefs"].as<JsonArray>().add("sensor.humidity");
    for (const char* source : {"sensor.temp", "sensor.humidity"}) {
        auto& sample = PeriphExecManager::getInstance().observations[source];
        sample.value = "old"; sample.sampleTimestamp = 123;
    }
    f.publish();
    FastBee::RemoteResourceManager::instance().changeSensorPairOnRead = true;
    f.refresh(); // Atomic publisher advances between the first and third card.
    assert(strcmp(f.out->cards[0].value, "old") == 0);
    assert(strcmp(f.out->cards[2].value, "old") == 0);
    assert(f.out->cards[0].updatedMs == 123 && f.out->cards[2].updatedMs == 123);
    assert(f.out->cards[3].series[0].updatedMs == 123 && f.out->cards[3].series[1].updatedMs == 123);
    f.refresh();
    assert(strcmp(f.out->cards[0].value, "new") == 0 && strcmp(f.out->cards[2].value, "new") == 0);
    assert(f.out->cards[3].series[0].updatedMs == 456 && f.out->cards[3].series[1].updatedMs == 456);
}

static void web() {
    Fixture f; f.refresh(); const auto hiddenBefore = f.out->cards[2];
    auto all = std::make_unique<ScreenPanelSnapshot>(); const auto allocations = poolAllocations;
    assert(f.manager.getSnapshot(*all));
    assert(all->cards[2].hasWeatherTemperature && all->cards[3].historyCount == 2);
    assert(all->pageCount == 2 && all->cardCount == 6 && all->activePageIndex == 0);
    assert(memcmp(&all->cards[0], &f.out->cards[0], sizeof(ScreenPanelCardSnapshot)) == 0);
    assert(memcmp(&hiddenBefore, &f.out->cards[2], sizeof(hiddenBefore)) == 0);
    assert(poolAllocations == allocations && f.cache.rebuildCount() == 1);
    f.hidden(); assert(memcmp(&all->cards[2], &f.out->cards[2], sizeof(ScreenPanelCardSnapshot)) == 0);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::map<std::string, void(*)()> scenarios = {
        {"reuse", reuse}, {"pages", pages}, {"sensor", sensor}, {"weather", weather},
        {"history", history}, {"operations", operations}, {"revision", revision},
        {"failure", failure}, {"web", web}, {"consistent", consistent}
    };
    scenarios.at(argv[1])();
    assert(pools.empty());
    printf("%s passed; cache=%zu card=%zu snapshot=%zu\n", argv[1],
           sizeof(ScreenPanelSnapshotCache), sizeof(ScreenPanelCardSnapshot), sizeof(ScreenPanelSnapshot));
}
