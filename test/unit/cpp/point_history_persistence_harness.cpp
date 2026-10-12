#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <LittleFS.h>

inline void convertToJson(const String& value, JsonVariant output) {
    output.set(std::string(value));
}
inline void convertFromJson(JsonVariantConst value, String& output) {
    output = value.as<std::string>();
}
static uint32_t clockMs = 1000;
static uint32_t historyMillis() { return clockMs; }
static SemaphoreHandle_t historyCreateMutex() { return reinterpret_cast<void*>(1); }
static bool readerContention = false;
static unsigned blockingPublications = 0;
static BaseType_t historyTakeMutex(SemaphoreHandle_t, TickType_t timeout) {
    if (!readerContention) return pdTRUE;
    if (timeout != portMAX_DELAY) return pdFALSE;
    readerContention = false; ++blockingPublications; return pdTRUE;
}
#define millis historyMillis
#define xSemaphoreCreateMutex historyCreateMutex
#define xSemaphoreTake historyTakeMutex
#define LOGGER_SYSTEM_H
struct HistoryLogger { void warning(const char*) {} } historyLogger;
#define LOGGER historyLogger
#define private public
#include "core/PointHistory.h"
#undef private
#include "../../../src/core/PointHistory.cpp"
#undef millis
#undef xSemaphoreCreateMutex
#undef xSemaphoreTake

using FastBee::PointHistory;
using FastBee::PointHistorySeriesSnapshot;
static const std::string dataPath = PointHistory::DATA_FILE;
static const std::string configPath = PointHistory::CONFIG_FILE;

static std::unique_ptr<PointHistory> fresh(bool erase = true) {
    readerContention = false;
    if (erase) LittleFS = {};
    auto history = std::unique_ptr<PointHistory>(new PointHistory());
    assert(history->initialize());
    return history;
}
static void enable(PointHistory& history) {
    String error;
    assert(history.configure(true, {"sensor.temp", "sensor.humidity"}, error, true));
}
static void sample(PointHistory& history, uint32_t identity, double value = 22.5) {
    clockMs = 1000 + identity * 1000;
    assert(history.recordSample("sensor.temp", value, "C", "mapping-a", identity,
                                clockMs, identity % 2 ? 0 : 1700000000000ULL + identity));
}
static PointHistorySeriesSnapshot snapshot(PointHistory& history) {
    PointHistorySeriesSnapshot result;
    assert(history.getSeries("sensor.temp", result));
    return result;
}
static JsonDocument parsed(const std::string& path) {
    JsonDocument doc;
    assert(!deserializeJson(doc, LittleFS.files.at(path)));
    return doc;
}
static void roundtrip() {
    auto history = fresh(); enable(*history);
    for (uint32_t i = 1; i <= 12; ++i) sample(*history, i);
    assert(history->flushNow());
    assert(!history->isDirty());
    auto disk = parsed(dataPath);
    assert(disk["series"][0]["samples"].size() == 10);
    assert(disk["series"][0]["samples"][0]["sampleIdentity"] == 3);
    assert(disk["series"][0]["samples"][9]["sampleIdentity"] == 12);
    assert(disk["series"][0]["samples"][0]["epochMs"].isNull());
    const auto before = snapshot(*history);
    const String originalBoot = before.samples[0].bootId;
    history.reset(); clockMs += 10000;
    history = fresh(false);
    assert(history->isEnabled() && history->wasRestored() && !history->isDirty());
    auto after = snapshot(*history);
    assert(after.count == 10 && after.containsRestoredSamples);
    for (size_t i = 0; i < after.count; ++i) {
        assert(after.samples[i].restored);
        assert(after.samples[i].value == before.samples[i].value);
        assert(after.samples[i].sequence == before.samples[i].sequence);
        assert(after.samples[i].bootId == originalBoot);
        assert(after.samples[i].epochMs == before.samples[i].epochMs);
    }
    // A reused sample identity after restart is a genuinely new sample.
    sample(*history, 12, 23.0);
    after = snapshot(*history);
    assert(after.count == 10 && after.samples[9].sequence == 13);
    assert(!after.samples[9].restored && after.samples[9].bootId != originalBoot);
}
static void deadline() {
    auto history = fresh(); enable(*history); sample(*history, 1);
    const uint32_t first = history->firstDirtyAt();
    const size_t writes = LittleFS.writeOpens;
    for (uint32_t i = 2; i <= 30; ++i) {
        sample(*history, i);
        history->tick(clockMs);
        assert(LittleFS.writeOpens == writes);
        assert(history->firstDirtyAt() == first);
    }
    clockMs = first + 30000;
    history->tick(clockMs);
    assert(!history->isDirty() && LittleFS.writeOpens == writes + 1);
    history->tick(clockMs + 60000);
    assert(LittleFS.writeOpens == writes + 1);
}
static void during_write() {
    auto history = fresh(); enable(*history); sample(*history, 1);
    LittleFS.duringWrite = [&] { sample(*history, 2, 24.0); };
    assert(history->flushNow());
    assert(history->isDirty());
    assert(history->firstDirtyAt() == 3000);
    assert(parsed(dataPath)["series"][0]["samples"].size() == 1);
    clockMs = 33000; history->tick(clockMs);
    assert(!history->isDirty());
    assert(parsed(dataPath)["series"][0]["samples"].size() == 2);
}
static void backup_restore() {
    auto history = fresh(); enable(*history); sample(*history, 1);
    assert(history->flushNow()); sample(*history, 2); assert(history->flushNow());
    assert(LittleFS.files.count(dataPath + ".bak"));
    LittleFS.files[dataPath] = "{truncated";
    LittleFS.files[dataPath + ".tmp"] = "{unfinished";
    history.reset(); history = fresh(false);
    assert(history->wasRestored());
    assert(snapshot(*history).count == 1);
}
static void malformed() {
    auto history = fresh(); enable(*history); sample(*history, 1); assert(history->flushNow());
    auto good = LittleFS.files.at(dataPath);
    const std::vector<std::string> bad = {
        "{", "[]", "{\"schemaVersion\":99,\"series\":[]}",
        std::string(PointHistory::MAX_FILE_BYTES + 1, 'x')
    };
    for (const auto& value : bad) {
        LittleFS.files[dataPath] = value;
        LittleFS.files.erase(dataPath + ".bak");
        auto restarted = fresh(false);
        assert(restarted->isEnabled() && !restarted->wasRestored());
        assert(restarted->sourceCount() == 0);
    }
    // Parsed JSON that violates numeric/schema bounds must also be rejected.
    for (int mutation = 0; mutation < 4; ++mutation) {
        JsonDocument doc; assert(!deserializeJson(doc, good));
        if (mutation == 0) doc["series"][0]["samples"][0]["value"] = "NaN";
        if (mutation == 1) doc["series"][0]["samples"][0]["sampleIdentity"] = -1;
        if (mutation == 2) doc["series"][0]["sourceRef"] = "invalid/ref";
        if (mutation == 3) {
            auto samples = doc["series"][0]["samples"].as<JsonArray>();
            JsonDocument saved; saved.set(samples[0]);
            for (int i = 0; i < 10; ++i) samples.add(saved.as<JsonObjectConst>());
        }
        LittleFS.files[dataPath].clear(); serializeJson(doc, LittleFS.files[dataPath]);
        auto restarted = fresh(false);
        assert(!restarted->wasRestored() && restarted->sourceCount() == 0);
    }
}
static void failed_write(const std::string& mode) {
    auto history = fresh(); enable(*history); sample(*history, 1); assert(history->flushNow());
    const std::string original = LittleFS.files.at(dataPath);
    sample(*history, 2);
    if (mode == "open") LittleFS.failOpenWrite = dataPath + ".tmp";
    if (mode == "short") { LittleFS.written = 0; LittleFS.writeLimit = 20; }
    if (mode == "corrupt") LittleFS.corruptClose = dataPath + ".tmp";
    if (mode == "rename_old") { LittleFS.failRenameFrom = dataPath; LittleFS.failRenameTo = dataPath + ".bak"; }
    if (mode == "rename_new") { LittleFS.failRenameFrom = dataPath + ".tmp"; LittleFS.failRenameTo = dataPath; }
    if (mode == "remove_backup") {
        LittleFS.files[dataPath + ".bak"] = original;
        LittleFS.failRemove = dataPath + ".bak";
    }
    assert(!history->flushNow());
    assert(history->isDirty() && !history->lastFlushSucceeded());
    assert(LittleFS.files.at(dataPath) == original);
    assert(!LittleFS.files.count(dataPath + ".tmp"));
    LittleFS.failOpenWrite.clear(); LittleFS.failRemove.clear();
    LittleFS.failRenameFrom.clear(); LittleFS.failRenameTo.clear();
    LittleFS.corruptClose.clear(); LittleFS.writeLimit = static_cast<size_t>(-1);
    assert(history->flushNow() && !history->isDirty());
    assert(parsed(dataPath)["series"][0]["samples"].size() == 2);
}
static void power_interruption() {
    auto history = fresh(); enable(*history); sample(*history, 1); assert(history->flushNow());
    const auto original = LittleFS.files.at(dataPath);
    // Simulate reset after old primary->backup but before temporary->primary.
    LittleFS.files[dataPath + ".bak"] = original;
    LittleFS.files.erase(dataPath);
    LittleFS.files[dataPath + ".tmp"] = "{unfinished";
    history.reset(); history = fresh(false);
    assert(history->wasRestored() && snapshot(*history).count == 1);
}
static void revision() {
    auto history = fresh(); enable(*history); sample(*history, 1); assert(history->flushNow());
    history = fresh(false);
    assert(history->recordSample("sensor.temp", 72.0, "F", "mapping-b", 2, 5000));
    assert(history->flushNow());
    auto restarted = fresh(false);
    const auto current = snapshot(*restarted);
    assert(current.count == 1 && current.sourceRevision == "mapping-b" && current.unit == "F");
    assert(current.samples[0].value == 72.0);
}
static void capacity() {
    auto history = fresh();
    String error;
    assert(history->configure(true, {"sensor.temp", "sensor.humidity", "sensor.pm25", "sensor.pm10"}, error));
    assert(history->_series.capacity() == 4);
    // Configuration alone must not allocate all 24 series on restart.
    history = fresh(false);
    assert(history->_series.capacity() == 4 && history->_sourceAllowlist.capacity() == 4);
    sample(*history, 1);
    assert(history->recordSample("sensor.humidity", 55, "%", "r1", 1, 2000));
    assert(history->flushNow());
    history = fresh(false);
    assert(history->sourceCount() == 2 && history->_series.capacity() == 2);
    assert(snapshot(*history).count == 1 && snapshot(*history).containsRestoredSamples);
    assert(!history->recordSample("sensor.other", 1, "", "r1", 1, 2000));
    // Shrinking an allowlist retains previously collected history.
    assert(history->configure(true, {"sensor.temp"}, error));
    assert(history->sourceCount() == 2 && history->_series.capacity() == 2);
    assert(!history->recordSample("sensor.humidity", 56, "%", "r1", 2, 3000));

    history = fresh();
    assert(history->configure(true, {}, error));
    assert(history->_series.capacity() == 0);
    history = fresh(false);
    assert(history->_series.capacity() == 0);
    for (size_t i = 0; i < PointHistory::MAX_SOURCES; ++i) {
        const String source = String("sensor.") + static_cast<unsigned int>(i);
        assert(history->recordSample(source, i, "", "r1", 1, 2000));
        assert(history->_series.capacity() == i + 1);
        assert(history->_series.capacity() <= PointHistory::MAX_SOURCES);
    }
    assert(!history->recordSample("sensor.overflow", 1, "", "r1", 1, 2000));
    assert(history->sourceCount() == 24 && history->_series.capacity() == 24);
    assert(history->recordSample("sensor.0", 2, "", "r1", 2, 3000));
    assert(history->flushNow());
    history = fresh(false);
    assert(history->sourceCount() == 24 && history->_series.capacity() == 24);
    assert(!history->recordSample("sensor.overflow", 1, "", "r1", 1, 2000));
    assert(history->configure(false, {}, error));
    assert(history->_series.capacity() == 0);
}
static void config_transaction() {
    auto history = fresh(); enable(*history); sample(*history, 1);
    const uint32_t revision = history->_configRevision;
    const auto before = parsed(configPath);
    String error;
    LittleFS.failOpenWrite = configPath + ".tmp";
    assert(!history->configure(false, {}, error, true, &revision));
    assert(history->isEnabled() && snapshot(*history).count == 1);
    assert(history->_configRevision == revision && !history->_configWriteInProgress);
    assert(parsed(configPath)["revision"] == before["revision"]);
    LittleFS.failOpenWrite.clear();
    bool concurrentRejected = false;
    LittleFS.duringWrite = [&] {
        String concurrentError;
        concurrentRejected = !history->configure(false, {}, concurrentError, false, &revision);
    };
    assert(history->configure(true, {"sensor.temp"}, error, true, &revision));
    assert(concurrentRejected && history->_configRevision == revision + 1);
    LittleFS.duringWrite = {};
    assert(!history->configure(false, {}, error, true, &revision));
    assert(error == "History configuration revision conflict");
    auto restarted = fresh(false);
    assert(restarted->_configRevision == revision + 1 && restarted->isEnabled());
    assert(restarted->_sourceAllowlist.size() == 1);
    LittleFS.files[configPath + ".bak"] = LittleFS.files[configPath];
    LittleFS.files.erase(configPath);
    LittleFS.files[configPath + ".tmp"] = "{\"schemaVersion\":1,\"enabled\":false,\"revision\":99,\"sourceAllowlist\":[]}";
    restarted = fresh(false);
    assert(restarted->isEnabled() && restarted->_configRevision == revision + 1);
    assert(parsed(configPath)["revision"] == revision + 1);

    // Backup is readable but restoring its primary filename fails. A later
    // failed commit must retain that sole committed generation for reboot.
    LittleFS.files[configPath + ".bak"] = LittleFS.files[configPath];
    LittleFS.files.erase(configPath);
    LittleFS.failRenameFrom = configPath + ".bak";
    LittleFS.failRenameTo = configPath;
    restarted = fresh(false);
    assert(restarted->isEnabled() && !LittleFS.files.count(configPath));
    const std::string committedBackup = LittleFS.files.at(configPath + ".bak");
    LittleFS.failRenameFrom = configPath + ".tmp";
    assert(!restarted->configure(false, {}, error));
    assert(restarted->isEnabled() && restarted->_configRevision == revision + 1);
    assert(LittleFS.files.at(configPath + ".bak") == committedBackup);
    LittleFS.failRenameFrom.clear(); LittleFS.failRenameTo.clear();
    auto recovered = fresh(false);
    assert(recovered->isEnabled() && recovered->_configRevision == revision + 1);

    // Storage completed while a reader briefly owns the mutex. Both the data
    // flush and config publisher must complete ownership release, not wedge
    // the service permanently behind an in-progress flag after 50 ms.
    sample(*recovered, 2);
    LittleFS.duringWrite = [] { readerContention = true; };
    assert(recovered->flushNow() && !recovered->_flushInProgress);
    const unsigned published = blockingPublications;
    LittleFS.duringWrite = [] { readerContention = true; };
    assert(recovered->configure(true, {"sensor.temp"}, error));
    assert(!recovered->_configWriteInProgress && blockingPublications == published + 1);
}
static void config_integrity() {
    auto history = fresh(); enable(*history);
    const auto before = LittleFS.files.at(configPath);
    String error;
    const uint32_t revision = history->_configRevision;
    LittleFS.mutateJsonOnClose = true;
    assert(!history->configure(true, {"sensor.temp"}, error, true, &revision));
    assert(LittleFS.files.at(configPath) == before);
    assert(history->_configRevision == revision && history->_sourceAllowlist.size() == 2);
    LittleFS.mutateJsonOnClose = false;
    history->_configRevision = UINT32_MAX;
    assert(!history->configure(false, {}, error));
    assert(history->isEnabled() && history->_configRevision == UINT32_MAX);
    assert(LittleFS.files.at(configPath) == before);
    LittleFS.files[configPath] = R"({"schemaVersion":1,"revision":5,"enabled":true,"sourceAllowlist":["sensor.temp\u0000hidden"]})";
    auto restarted = fresh(false);
    assert(!restarted->isEnabled() && restarted->_sourceAllowlist.empty());
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string name(argv[1]);
    if (name == "roundtrip") roundtrip();
    else if (name == "deadline") deadline();
    else if (name == "during_write") during_write();
    else if (name == "backup_restore") backup_restore();
    else if (name == "malformed") malformed();
    else if (name == "power_interruption") power_interruption();
    else if (name == "revision") revision();
    else if (name == "capacity") capacity();
    else if (name == "config_transaction") config_transaction();
    else if (name == "config_integrity") config_integrity();
    else failed_write(name);
    return 0;
}
