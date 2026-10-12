// All recorder runtime definitions are compiled unchanged; only SDK, locks,
// observation capture and SD transactions are explicit seams. Disk behavior is
// independently covered with complete production SDCardManager.cpp.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <memory>
#include <map>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include "core/SdRecorderRuntime.h"
#include "core/SensorConfigEpoch.h"
#include "utils/SystemTimeQuality.h"
#include "network/SdRecorderConfigJson.h"
inline void convertToJson(const String& value, JsonVariant out) { out.set(std::string(value)); }
inline void convertFromJson(JsonVariantConst value, String& out) { out = value.as<std::string>(); }
inline size_t serializeJson(const JsonDocument& doc, String& out) {
    std::string text; const auto count = serializeJson(doc, text); out = text; return count;
}
static uint64_t testNow = 1000;
static int64_t esp_timer_get_time() { return static_cast<int64_t>(testNow * 1000); }
static uint32_t esp_random() { static uint32_t token = 1; return token++; }
#define pdMS_TO_TICKS(value) (value)
struct TestMutex { bool allowed = true; };
class MutexGuard {
public:
    explicit MutexGuard(TestMutex* mutex, uint32_t = 0) : held_(mutex && mutex->allowed) {}
    bool isLocked() const { return held_; }
private: bool held_;
};
using RecursiveMutexGuard = MutexGuard;
namespace FastBee { namespace MemoryBudget {
constexpr size_t PERIPH_EXEC_MIN_DRAM = 16000;
inline size_t free = 200000;
inline size_t dram8Free() { return free; }
}}
static FastBee::SystemTimeQuality::Snapshot testTime;
FastBee::SystemTimeQuality::Snapshot FastBee::SystemTimeQuality::snapshot() { return testTime; }
enum class PeripheralType { SDIO, SENSOR_GENERIC };
struct PeripheralConfig {
    bool enabled = true;
    PeripheralType type = PeripheralType::SDIO;
    uint8_t pinCount = 4, pins[4] = {18, 23, 19, 5};
    struct { struct { uint32_t frequency = 1000000; } sdcard; } params;
};
struct PeripheralRuntimeState { bool memorySuspended = false; };
struct PeripheralManager {
    TestMutex mutex;
    PeripheralConfig config;
    PeripheralRuntimeState runtime;
    static PeripheralManager& getInstance() { static PeripheralManager instance; return instance; }
    TestMutex* getMutex() { return &mutex; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& out) {
        if (id != "sd" && id != "room") return false;
        out = config; if (id == "room") out.type = PeripheralType::SENSOR_GENERIC;
        return true;
    }
    bool getRuntimeStateSnapshot(const String&, PeripheralRuntimeState& out) { out = runtime; return true; }
};
struct PeriphExecExecutor {
    static String resolveChannelField(const String&, const String&, const String& field) { return field; }
};
static SDCardStatus sdStatus;
static bool writesOk = true;
static bool writeAttempted = true;
static unsigned writes = 0;
static std::string bytesWritten;
static uint64_t lastToken = 0;
SDCardManager& SDCardManager::getInstance() { static SDCardManager storage; return storage; }
SDCardStatus SDCardManager::getStatus() const { return sdStatus; }
bool SDCardManager::appendRecording(const String& owner, uint64_t generation, SDRecordingSession& session,
        const uint8_t* data, size_t length, uint16_t, uint8_t, String& error, bool* ioAttempted) {
    assert(owner == "sd" && generation == sdStatus.recordingGeneration && length <= 2048);
    if (ioAttempted) *ioAttempted = writeAttempted;
    if (writeAttempted) ++writes;
    lastToken = session.token;
    if (!writesOk) { error = writeAttempted ? "injected_short_write" : "SPI_BUSY"; return false; }
    bytesWritten.append(reinterpret_cast<const char*>(data), length);
    return true;
}
enum class ObservationQuality { UNKNOWN, GOOD, STALE, OFFLINE, ERROR };
struct ObservationSnapshot {
    bool hasValue = true, bindingValid = true, stale = false, recordable = true;
    ObservationQuality quality = ObservationQuality::GOOD;
    String value = "20.5", valueType = "number", sourceRevision = "room:temperature:C", unit = "C";
    uint64_t sampledAtMs = 500;
    uint32_t sampleIdentity = 500;
};
class PeriphExecManager {
public:
    TestMutex rulesMutex;
    TestMutex* _rulesMutex = &rulesMutex;
    std::map<String, PeriphExecRule> rules;
    std::unique_ptr<FastBee::SdRecorderRuntime> _sdRecorder;
    uint64_t _ruleRuntimeRevision = 1;
    bool _configurationLoadFailed = false, _configurationRestartPending = false;
    std::atomic<bool> _thermostatShutdown{false};
    ObservationSnapshot observation;
    bool observationAvailable = true;
    bool getObservation(const String&, ObservationSnapshot& out) {
        if (!observationAvailable) return false;
        out = observation;
        return true;
    }
    bool sdRecorderSourceBindingLocked(const String&, String&) const;
    void retireSdRecorderLocked();
    void checkSdRecorder();
    bool getSdRecorderStatus(const PeriphExecRule&, ActionExecResult&);
    String getSdRecorderDiagnosticsJson() const;
};
#include "sd_recorder_runtime_methods.inc"

static void setup(PeriphExecManager& manager) {
    sdStatus.statusAvailable = sdStatus.configured = sdStatus.mounted = true;
    sdStatus.ownerId = "sd"; sdStatus.sckPin = 18; sdStatus.mosiPin = 23;
    sdStatus.misoPin = 19; sdStatus.csPin = 5; sdStatus.frequency = 1000000;
    sdStatus.recordingGeneration = 1;
    PeriphExecRule collector; collector.id = "collect"; collector.enabled = collector.recordHistory = true;
    ExecAction action; action.actionType = 19; action.targetPeriphId = "room";
    action.actionValue = R"({"periphId":"room","channelField":"temperature","unit":"C"})";
    collector.actions.push_back(action); manager.rules[collector.id] = collector;
    PeriphExecRule record; record.id = "record"; record.enabled = true; record.runtimeRevision = 1;
    ExecTrigger trigger; trigger.triggerType = 1; trigger.intervalSec = 1; record.triggers.push_back(trigger);
    action.actionType = 40; action.targetPeriphId = "sd";
    action.actionValue = R"({"sourceRef":"room_temperature","sampleSec":1,"maxAgeSec":30,"flushSec":3,"fileKiB":4,"fileCount":2})";
    record.actions.push_back(action); manager.rules[record.id] = record;
}
static void tick(PeriphExecManager& manager, uint64_t now, bool fresh = true) {
    testNow = now;
    if (fresh) {
        manager.observation.sampledAtMs = now;
        manager.observation.sampleIdentity = static_cast<uint32_t>(now);
    }
    manager.checkSdRecorder();
}
static JsonDocument status(PeriphExecManager& manager) {
    JsonDocument result;
    assert(!deserializeJson(result, manager.getSdRecorderDiagnosticsJson().c_str())); return result;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    PeriphExecManager manager; setup(manager);
    const std::string mode = argv[1];
    if (mode == "config") {
        const String original = manager.rules["record"].actions[0].actionValue;
        FastBee::SdRecorderConfig out; String error;
        assert(FastBee::parseSdRecorderConfig(original, out, error));
        for (const auto& spec : FastBee::kSdRecorderNumbers) {
            for (int mutation : {0, 1, 2, 3, 4}) {
                JsonDocument doc; assert(!deserializeJson(doc, original.c_str()));
                if (mutation == 0) doc[spec.key] = spec.min - 1;
                if (mutation == 1) doc[spec.key] = spec.max + 1;
                if (mutation == 2) doc[spec.key] = true;
                if (mutation == 3) doc[spec.key] = "1";
                if (mutation == 4) doc[spec.key] = 1.5;
                String text; serializeJson(doc, text);
                assert(!FastBee::parseSdRecorderConfig(text, out, error));
            }
        }
        JsonDocument doc; FastBee::writeSdRecorderConfigJson(doc.to<JsonObject>());
        assert(doc["schema"] == 1 && doc["numbers"].size() == 5 && doc["sourceMaxBytes"] == 96);
        manager.rules["record"].recordHistory = true;
        assert(!FastBee::validateSdRecorderRule(manager.rules["record"], error));
        return 0;
    }
    if (mode == "binding") {
        String binding;
        assert(manager.sdRecorderSourceBindingLocked("room_temperature", binding));
        manager.rules["collect"].recordHistory = false;
        assert(!manager.sdRecorderSourceBindingLocked("room_temperature", binding));
        manager.rules["collect"].recordHistory = true;
        manager.rules["duplicate"] = manager.rules["collect"];
        manager.rules["duplicate"].enabled = false;
        assert(!manager.sdRecorderSourceBindingLocked("room_temperature", binding));
        manager.rules.erase("duplicate");
        assert(!manager.sdRecorderSourceBindingLocked("unknown", binding));
        return 0;
    }
    if (mode == "queue") {
        FastBee::SdRecorderBuffer queue; queue.arm(100);
        for (uint32_t i = 0; i < 20; ++i) {
            FastBee::SdRecordedSample sample; sample.sampledAtMs = sample.recordedAtMs = 101 + i; sample.identity = i + 1;
            assert(queue.enqueue(sample) == (i < 8));
        }
        assert(queue.size() == 8 && queue.dropped == 12 && queue.accepted == 8);
        queue.fail("short"); assert(queue.dropped == 20 && !queue.size());
        queue.arm(1000);
        FastBee::SdRecordedSample old; old.sampledAtMs = old.recordedAtMs = 119; old.identity = 19;
        assert(!queue.enqueue(old));
        old.sampledAtMs = old.recordedAtMs = 1001; old.identity = 1001;
        assert(queue.enqueue(old) && !queue.enqueue(old));
        old.value = std::numeric_limits<double>::quiet_NaN(); assert(!queue.enqueue(old));
        return 0;
    }
    tick(manager, 1000, false);
    assert(manager._sdRecorder && !writes);
    if (mode == "status") {
        ActionExecResult result;
        assert(manager.getSdRecorderStatus(manager.rules["record"], result));
        assert(!writes && status(manager)["phase"] == "WAITING_FRESH_SAMPLE");
        manager.rulesMutex.allowed = false;
        assert(!manager.getSdRecorderStatus(manager.rules["record"], result));
    } else if (mode == "normal" || mode == "wall") {
        if (mode == "wall") {
            testTime.timeValid = testTime.synced = true; testTime.source = "https";
            testTime.quality = "synced_tls"; testTime.transportAuthenticated = true; testTime.epochMs = 1800000000000LL;
        }
        tick(manager, 2000); tick(manager, 3000); tick(manager, 4000);
        assert(writes == 1 && manager._sdRecorder->queue.written == 3);
        const auto line = bytesWritten.substr(0, bytesWritten.find('\n'));
        JsonDocument row; assert(!deserializeJson(row, line));
        assert(row["sourceRef"] == "room_temperature" && row["quality"] == "good");
        assert(row["sampledAtMs"] == 2000 && row["recordedAtMs"] == 2000);
        if (mode == "wall") assert(row["wallEpochMs"] == 1800000000000LL && row["timeAuthenticated"] == true);
        else assert(row["wallEpochMs"].isNull() && row["timeQuality"] == "unknown");
        assert(status(manager)["writeQuality"] == "sdk_write_flush_only");
    } else if (mode == "old" || mode == "stale" || mode == "invalid") {
        if (mode == "stale") manager.observation.stale = true;
        if (mode == "invalid") manager.observation.value = "nan";
        tick(manager, 2000, mode != "old"); tick(manager, 5000, false);
        assert(!writes && !manager._sdRecorder->queue.size());
    } else if (mode == "duplicates") {
        tick(manager, 2000); tick(manager, 3000, false); tick(manager, 4000, false);
        assert(writes == 1 && manager._sdRecorder->queue.written == 1);
    } else if (mode == "busy-retry") {
        writesOk = writeAttempted = false;
        const uint64_t token = manager._sdRecorder->storage.token;
        tick(manager, 2000); tick(manager, 4000);
        assert(!writes && manager._sdRecorder->queue.size() == 2 && !manager._sdRecorder->queue.fault());
        writesOk = writeAttempted = true;
        tick(manager, 5000);
        assert(writes == 1 && lastToken == token && manager._sdRecorder->queue.written == 3);
    } else if (mode == "write-fault") {
        writesOk = false;
        tick(manager, 2000); tick(manager, 4000);
        assert(writes == 1 && manager._sdRecorder->queue.fault() && !manager._sdRecorder->queue.size());
        const uint64_t failedToken = lastToken;
        writesOk = true; tick(manager, 8000); assert(writes == 1);
        ++sdStatus.recordingGeneration; ++sdStatus.recordingRecovery;
        tick(manager, 9000, false);
        assert(status(manager)["phase"] == "WAITING_FRESH_SAMPLE");
        tick(manager, 10000, false); assert(writes == 1);
        tick(manager, 11000); tick(manager, 12000);
        assert(writes == 2 && lastToken != failedToken && manager._sdRecorder->queue.dropped == 2);
    } else if (mode == "unmount") {
        tick(manager, 2000); sdStatus.mounted = false; ++sdStatus.recordingGeneration;
        tick(manager, 3000); sdStatus.mounted = true;
        tick(manager, 7000); assert(!writes && manager._sdRecorder->queue.fault());
    } else if (mode == "source-change" || mode == "graph-change") {
        tick(manager, 2000);
        if (mode == "source-change") manager.observation.sourceRevision = "new_rom";
        else {
            manager.rules["collect"].actions[0].actionValue = R"({"periphId":"room","channelField":"temperature","offset":99})";
            manager.retireSdRecorderLocked();
        }
        tick(manager, 3000); ++sdStatus.recordingRecovery; ++sdStatus.recordingGeneration;
        tick(manager, 7000); assert(!writes && manager._sdRecorder->permanentFault);
    } else if (mode == "disable" || mode == "shutdown") {
        tick(manager, 2000); assert(manager._sdRecorder->queue.size() == 1);
        if (mode == "disable") manager.rules["record"].enabled = false;
        else manager._thermostatShutdown = true;
        manager.retireSdRecorderLocked(); assert(!manager._sdRecorder && !writes);
        tick(manager, 5000); assert(!manager._sdRecorder && !writes);
    } else if (mode == "memory") {
        tick(manager, 2000); FastBee::MemoryBudget::free = 1;
        tick(manager, 3000); FastBee::MemoryBudget::free = 200000;
        tick(manager, 5000); assert(!writes && manager._sdRecorder->queue.fault());
    } else { assert(false); }
    return 0;
}
