#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_POINT_HISTORY

#include "core/PointHistory.h"

#include <LittleFS.h>
#include <cmath>
#include <cstring>

#include "systems/LoggerSystem.h"

namespace FastBee {
namespace {

class HistoryLock {
public:
    explicit HistoryLock(SemaphoreHandle_t mutex, TickType_t timeout = pdMS_TO_TICKS(50))
        : _mutex(mutex), _locked(mutex && xSemaphoreTake(mutex, timeout) == pdTRUE) {}
    ~HistoryLock() { if (_locked) xSemaphoreGive(_mutex); }
    bool locked() const { return _locked; }

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};

bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
    return since != 0 && static_cast<uint32_t>(now - since) >= duration;
}

bool parseUnsigned(JsonVariantConst value, uint32_t& out) {
    if (!value.is<uint32_t>()) return false;
    out = value.as<uint32_t>();
    return true;
}

}  // namespace

PointHistory& PointHistory::instance() {
    static PointHistory history;
    return history;
}

bool PointHistory::initialize() {
    if (_initialized) return true;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    char boot[32];
#if defined(ARDUINO_ARCH_ESP32)
    const uint64_t chip = ESP.getEfuseMac();
    snprintf(boot, sizeof(boot), "%08lx-%08lx",
             static_cast<unsigned long>(chip >> 32),
             static_cast<unsigned long>(chip ^ millis()));
#else
    snprintf(boot, sizeof(boot), "boot-%08lx", static_cast<unsigned long>(millis()));
#endif
    _bootId = boot;
    _initialized = true;

    if (!loadConfig()) {
        LOGGER.warning("[PointHistory] Config invalid; history remains disabled");
        _enabled = false;
        _sourceAllowlist.clear();
        return true;
    }
    if (_enabled && !loadHistory()) {
        LOGGER.warning("[PointHistory] No valid history generation restored");
    }
    return true;
}

bool PointHistory::validSourceRef(const String& sourceRef) {
    if (sourceRef.isEmpty() || sourceRef.length() > MAX_SOURCE_REF_LENGTH) return false;
    for (size_t i = 0; i < sourceRef.length(); ++i) {
        const char c = sourceRef.charAt(i);
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                           c == '.' || c == ':';
        if (!valid) return false;
    }
    return true;
}

bool PointHistory::isAllowedSource(const String& sourceRef) const {
    if (_sourceAllowlist.empty()) return true;
    for (const auto& allowed : _sourceAllowlist) {
        if (allowed == sourceRef) return true;
    }
    return false;
}

bool PointHistory::configure(bool enabled, const std::vector<String>& sourceAllowlist,
                             String& error, bool persist) {
    error = "";
    if (!_initialized && !initialize()) {
        error = "Point history is unavailable";
        return false;
    }
    {
        HistoryLock lock(_mutex);
        if (!lock.locked() || _writesPaused) {
            error = "Point history writes are paused";
            return false;
        }
    }
    if (sourceAllowlist.size() > MAX_SOURCES) {
        error = String("History source limit exceeded (max ") + MAX_SOURCES + ")";
        return false;
    }
    for (size_t i = 0; i < sourceAllowlist.size(); ++i) {
        if (!validSourceRef(sourceAllowlist[i])) {
            error = "Invalid history sourceRef";
            return false;
        }
        for (size_t j = i + 1; j < sourceAllowlist.size(); ++j) {
            if (sourceAllowlist[i] == sourceAllowlist[j]) {
                error = "Duplicate history sourceRef";
                return false;
            }
        }
    }

    if (!enabled && _dirty && !flushNow()) {
        error = "Unable to flush point history before disabling";
        return false;
    }

    bool oldEnabled;
    std::vector<String> oldAllowlist;
    {
        HistoryLock lock(_mutex);
        if (!lock.locked()) {
            error = "Point history is busy";
            return false;
        }
        oldEnabled = _enabled;
        oldAllowlist = _sourceAllowlist;
        _enabled = enabled;
        _sourceAllowlist = sourceAllowlist;
        if (enabled) {
            // An empty allowlist means accept any source, not preallocate all
            // 24 series (each embeds ten samples). Keep existing history when
            // changing the allowlist, and reserve only the configured demand.
            const size_t capacity = _series.size() > sourceAllowlist.size()
                ? _series.size() : sourceAllowlist.size();
            if (_series.capacity() > capacity) _series.shrink_to_fit();
            if (_series.capacity() < capacity) _series.reserve(capacity);
        } else {
            _series.clear();
            _series.shrink_to_fit();
            _restored = false;
            _dirty = false;
            _firstDirtyAt = 0;
            _postSnapshotDirtyAt = 0;
        }
    }

    if (persist && !saveConfig()) {
        HistoryLock lock(_mutex);
        if (lock.locked()) {
            _enabled = oldEnabled;
            _sourceAllowlist = std::move(oldAllowlist);
        }
        error = "Failed to persist history configuration";
        return false;
    }
    return true;
}

bool PointHistory::recordSample(const String& sourceRef, double value,
                                const String& unit, const String& sourceRevision,
                                uint32_t sampleIdentity, uint32_t uptimeMs,
                                uint64_t epochMs) {
    if (!_initialized && !initialize()) return false;
    if (!std::isfinite(value) || !validSourceRef(sourceRef) ||
        unit.length() > 24 || sourceRevision.length() > 64) return false;

    const uint32_t now = uptimeMs ? uptimeMs : millis();
    HistoryLock lock(_mutex);
    if (!lock.locked() || _writesPaused || !_enabled ||
        !isAllowedSource(sourceRef)) return false;

    Series* series = nullptr;
    for (auto& candidate : _series) {
        if (candidate.sourceRef == sourceRef) {
            series = &candidate;
            break;
        }
    }
    if (!series) {
        if (_series.size() >= MAX_SOURCES) return false;
        // Explicit growth prevents vector's geometric growth from reserving
        // more than MAX_SOURCES when unrestricted sources are discovered.
        if (_series.size() == _series.capacity()) _series.reserve(_series.size() + 1);
        _series.emplace_back();
        series = &_series.back();
        series->sourceRef = sourceRef;
        series->sourceRevision = sourceRevision;
        series->unit = unit;
    } else if (series->sourceRevision != sourceRevision || series->unit != unit) {
        series->sourceRevision = sourceRevision;
        series->unit = unit;
        series->head = 0;
        series->count = 0;
        series->containsRestoredSamples = false;
    }
    series->stale = false;
    series->staleReason = "";

    if (sampleIdentity != 0) {
        for (uint8_t i = 0; i < series->count; ++i) {
            const size_t index = (series->head + i) % MAX_SAMPLES_PER_SOURCE;
            const auto& sample = series->samples[index];
            if (!sample.restored && sample.bootId == _bootId &&
                sample.sampleIdentity == sampleIdentity) return false;
        }
    }

    PointHistorySample sample;
    sample.value = value;
    sample.sequence = ++_sequence;
    sample.sampleIdentity = sampleIdentity ? sampleIdentity : sample.sequence;
    sample.uptimeMs = now;
    sample.epochMs = epochMs;
    sample.bootId = _bootId;
    sample.restored = false;

    size_t target;
    if (series->count < MAX_SAMPLES_PER_SOURCE) {
        target = (series->head + series->count) % MAX_SAMPLES_PER_SOURCE;
        ++series->count;
    } else {
        target = series->head;
        series->head = (series->head + 1) % MAX_SAMPLES_PER_SOURCE;
    }
    series->samples[target] = sample;

    ++_generation;
    if (!_dirty) {
        _dirty = true;
        _firstDirtyAt = now ? now : 1;
    } else if (_flushInProgress && _postSnapshotDirtyAt == 0) {
        _postSnapshotDirtyAt = now ? now : 1;
    }
    return true;
}

bool PointHistory::invalidateSource(const String& sourceRef,
                                    const String& reason,
                                    const String& nextSourceRevision) {
    if (!_initialized || !validSourceRef(sourceRef) || reason.length() > 96 ||
        nextSourceRevision.length() > 64) {
        return false;
    }
    const uint32_t now = millis();
    HistoryLock lock(_mutex);
    if (!lock.locked() || !_enabled) return false;
    for (auto& series : _series) {
        if (series.sourceRef != sourceRef) continue;
        if (!nextSourceRevision.isEmpty() &&
            nextSourceRevision != series.sourceRevision) {
            series.sourceRevision = nextSourceRevision;
            series.head = 0;
            series.count = 0;
            series.containsRestoredSamples = false;
        }
        series.stale = true;
        series.staleReason = reason;
        ++_generation;
        if (!_dirty) {
            _dirty = true;
            _firstDirtyAt = now ? now : 1;
        } else if (_flushInProgress && _postSnapshotDirtyAt == 0) {
            _postSnapshotDirtyAt = now ? now : 1;
        }
        return true;
    }
    return false;
}

bool PointHistory::getSeries(const String& sourceRef,
                             PointHistorySeriesSnapshot& out) const {
    HistoryLock lock(_mutex);
    if (!lock.locked()) return false;
    for (const auto& series : _series) {
        if (series.sourceRef != sourceRef) continue;
        out = PointHistorySeriesSnapshot();
        out.sourceRef = series.sourceRef;
        out.sourceRevision = series.sourceRevision;
        out.unit = series.unit;
        out.count = series.count;
        out.containsRestoredSamples = series.containsRestoredSamples;
        out.stale = series.stale;
        out.staleReason = series.staleReason;
        for (uint8_t i = 0; i < series.count; ++i) {
            out.samples[i] = series.samples[(series.head + i) % MAX_SAMPLES_PER_SOURCE];
        }
        return true;
    }
    return false;
}

void PointHistory::appendSeriesJson(const String& sourceRef, JsonObject out) const {
    PointHistorySeriesSnapshot snapshot;
    out["sourceRef"] = sourceRef;
    JsonArray points = out["points"].to<JsonArray>();
    if (!getSeries(sourceRef, snapshot)) {
        out["found"] = false;
        out["count"] = 0;
        return;
    }
    out["found"] = true;
    out["sourceRevision"] = snapshot.sourceRevision;
    out["unit"] = snapshot.unit;
    out["count"] = snapshot.count;
    out["containsRestoredSamples"] = snapshot.containsRestoredSamples;
    out["stale"] = snapshot.stale;
    if (!snapshot.staleReason.isEmpty()) out["staleReason"] = snapshot.staleReason;
    for (uint8_t i = 0; i < snapshot.count; ++i) {
        const auto& sample = snapshot.samples[i];
        JsonObject item = points.add<JsonObject>();
        item["value"] = sample.value;
        item["seq"] = sample.sequence;
        item["sampleIdentity"] = sample.sampleIdentity;
        item["bootId"] = sample.bootId;
        item["uptimeMs"] = sample.uptimeMs;
        if (sample.epochMs) item["epochMs"] = sample.epochMs;
        else item["epochMs"] = nullptr;
        item["restored"] = sample.restored;
    }
}

void PointHistory::appendConfigJson(JsonObject out) const {
    HistoryLock lock(_mutex);
    if (!lock.locked()) return;
    out["enabled"] = _enabled;
    out["writesPaused"] = _writesPaused;
    out["maxSources"] = MAX_SOURCES;
    out["maxSamplesPerSource"] = MAX_SAMPLES_PER_SOURCE;
    JsonArray allowlist = out["sourceAllowlist"].to<JsonArray>();
    for (const auto& source : _sourceAllowlist) allowlist.add(source);
}

void PointHistory::appendStatusJson(JsonObject out) const {
    HistoryLock lock(_mutex);
    if (!lock.locked()) return;
    out["enabled"] = _enabled;
    out["dirty"] = _dirty;
    out["restored"] = _restored;
    out["persisted"] = !_dirty && _lastFlushSucceeded;
    out["lastFlushSucceeded"] = _lastFlushSucceeded;
    out["sourceCount"] = _series.size();
    out["firstDirtyAt"] = _firstDirtyAt;
    out["flushDeadlineMs"] = FLUSH_DEADLINE_MS;
}

size_t PointHistory::sourceCount() const {
    HistoryLock lock(_mutex);
    return lock.locked() ? _series.size() : 0;
}

void PointHistory::tick(uint32_t nowMs) {
    if (!_initialized || !_enabled || _writesPaused || !_dirty ||
        _flushInProgress) return;
    const uint32_t now = nowMs ? nowMs : millis();
    if (elapsed(now, _firstDirtyAt, FLUSH_DEADLINE_MS)) flushNow();
}

bool PointHistory::flushNow(uint32_t timeoutMs) {
    (void)timeoutMs;
    // An uninitialized history service has no dirty samples to persist. This
    // also keeps controlled restart working in builds where history is
    // compiled but the peripheral execution manager is disabled.
    if (!_initialized) return true;

    std::vector<Series> snapshot;
    uint32_t generation = 0;
    uint32_t sequence = 0;
    {
        HistoryLock lock(_mutex);
        if (!lock.locked()) return false;
        if (!_dirty) return true;
        if (_writesPaused || _flushInProgress) return false;
        _flushInProgress = true;
        _postSnapshotDirtyAt = 0;
        snapshot = _series;
        generation = _generation;
        sequence = _sequence;
    }

    const bool success = writeHistorySnapshot(snapshot, sequence);
    const uint32_t finishedAt = millis();
    {
        HistoryLock lock(_mutex);
        if (!lock.locked()) return false;
        _flushInProgress = false;
        _lastFlushSucceeded = success;
        if (success && generation == _generation) {
            _dirty = false;
            _firstDirtyAt = 0;
            _postSnapshotDirtyAt = 0;
        } else if (success) {
            _dirty = true;
            _firstDirtyAt = _postSnapshotDirtyAt ? _postSnapshotDirtyAt : (finishedAt ? finishedAt : 1);
            _postSnapshotDirtyAt = 0;
        }
    }
    return success;
}

bool PointHistory::pauseWrites(uint32_t timeoutMs) {
    if (!_initialized && !initialize()) return false;

    {
        HistoryLock lock(_mutex);
        if (!lock.locked()) return false;
        _writesPaused = true;
    }

    const uint32_t startedAt = millis();
    while (true) {
        {
            HistoryLock lock(_mutex);
            if (!lock.locked()) return false;
            if (!_flushInProgress && !_configWriteInProgress) return true;
        }
        if (static_cast<uint32_t>(millis() - startedAt) >= timeoutMs) {
            HistoryLock lock(_mutex);
            if (lock.locked()) _writesPaused = false;
            return false;
        }
        delay(5);
    }
}

bool PointHistory::resumeWrites(bool resetRuntimeState) {
    if (!_initialized) return false;
    HistoryLock lock(_mutex);
    if (!lock.locked() || _flushInProgress || _configWriteInProgress) {
        return false;
    }
    if (resetRuntimeState) {
        _enabled = false;
        _dirty = false;
        _restored = false;
        _lastFlushSucceeded = true;
        _firstDirtyAt = 0;
        _postSnapshotDirtyAt = 0;
        _sequence = 0;
        ++_generation;
        _sourceAllowlist.clear();
        _series.clear();
    }
    _writesPaused = false;
    return true;
}

bool PointHistory::loadConfig() {
    if (!LittleFS.exists(CONFIG_FILE)) return true;
    File file = LittleFS.open(CONFIG_FILE, FILE_READ);
    if (!file || file.size() > 4096) {
        if (file) file.close();
        return false;
    }
    JsonDocument doc;
    const DeserializationError parseError = deserializeJson(doc, file);
    file.close();
    if (parseError || !doc.is<JsonObjectConst>() || doc["schemaVersion"] != 1 ||
        !doc["enabled"].is<bool>() || !doc["sourceAllowlist"].is<JsonArrayConst>()) return false;

    std::vector<String> allowlist;
    JsonArrayConst sources = doc["sourceAllowlist"].as<JsonArrayConst>();
    if (sources.size() > MAX_SOURCES) return false;
    allowlist.reserve(sources.size());
    for (JsonVariantConst item : sources) {
        if (!item.is<const char*>()) return false;
        String source = item.as<const char*>();
        if (!validSourceRef(source)) return false;
        for (const auto& existing : allowlist) if (existing == source) return false;
        allowlist.push_back(source);
    }
    _enabled = doc["enabled"].as<bool>();
    _sourceAllowlist = std::move(allowlist);
    if (_enabled) {
        _series.reserve(_sourceAllowlist.size());
    }
    return true;
}

bool PointHistory::saveConfig() {
    JsonDocument doc;
    doc["schemaVersion"] = 1;
    {
        HistoryLock lock(_mutex);
        if (!lock.locked() || _writesPaused || _configWriteInProgress) {
            return false;
        }
        _configWriteInProgress = true;
        doc["enabled"] = _enabled;
        JsonArray sources = doc["sourceAllowlist"].to<JsonArray>();
        for (const auto& source : _sourceAllowlist) sources.add(source);
    }
    const bool success = atomicWriteJsonWithBackup(CONFIG_FILE, doc);
    {
        HistoryLock lock(_mutex);
        if (lock.locked()) _configWriteInProgress = false;
    }
    return success;
}

bool PointHistory::loadHistoryFile(const String& path, std::vector<Series>& restored,
                                   uint32_t& restoredSequence) const {
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.size() == 0 || file.size() > MAX_FILE_BYTES) {
        if (file) file.close();
        return false;
    }
    JsonDocument doc;
    const DeserializationError parseError = deserializeJson(doc, file);
    file.close();
    if (parseError || !doc.is<JsonObjectConst>() || doc["schemaVersion"] != 1 ||
        !doc["series"].is<JsonArrayConst>()) return false;

    JsonArrayConst allSeries = doc["series"].as<JsonArrayConst>();
    if (allSeries.size() > MAX_SOURCES) return false;
    // A rejected primary may have allocated a larger vector than its valid
    // backup needs. Release that capacity before restoring the actual count.
    std::vector<Series>().swap(restored);
    restored.reserve(allSeries.size());
    restoredSequence = 0;

    for (JsonObjectConst source : allSeries) {
        const char* sourceRef = source["sourceRef"] | "";
        const char* sourceRevision = source["sourceRevision"] | "";
        const char* unit = source["unit"] | "";
        if (!validSourceRef(String(sourceRef)) || strlen(sourceRevision) > 64 ||
            strlen(unit) > 24 || !source["samples"].is<JsonArrayConst>() ||
            (!source["stale"].isNull() && !source["stale"].is<bool>()) ||
            (!source["staleReason"].isNull() &&
             (!source["staleReason"].is<const char*>() ||
              strlen(source["staleReason"].as<const char*>()) > 96))) return false;
        JsonArrayConst samples = source["samples"].as<JsonArrayConst>();
        if (samples.size() > MAX_SAMPLES_PER_SOURCE) return false;

        Series series;
        series.sourceRef = sourceRef;
        series.sourceRevision = sourceRevision;
        series.unit = unit;
        series.containsRestoredSamples = samples.size() > 0;
        series.stale = source["stale"] | false;
        series.staleReason = source["staleReason"] | "";
        for (JsonObjectConst raw : samples) {
            if (!raw["value"].is<double>() || !raw["seq"].is<uint32_t>() ||
                !raw["sampleIdentity"].is<uint32_t>() ||
                !raw["uptimeMs"].is<uint32_t>() || !raw["bootId"].is<const char*>()) return false;
            const double value = raw["value"].as<double>();
            const char* bootId = raw["bootId"].as<const char*>();
            if (!std::isfinite(value) || strlen(bootId) > 31) return false;
            PointHistorySample sample;
            sample.value = value;
            if (!parseUnsigned(raw["seq"], sample.sequence) ||
                !parseUnsigned(raw["sampleIdentity"], sample.sampleIdentity) ||
                !parseUnsigned(raw["uptimeMs"], sample.uptimeMs)) return false;
            if (!raw["epochMs"].isNull()) {
                if (!raw["epochMs"].is<uint64_t>()) return false;
                sample.epochMs = raw["epochMs"].as<uint64_t>();
            }
            sample.bootId = bootId;
            sample.restored = true;
            series.samples[series.count++] = sample;
            if (sample.sequence > restoredSequence) restoredSequence = sample.sequence;
        }
        restored.push_back(std::move(series));
    }
    return true;
}

bool PointHistory::loadHistory() {
    std::vector<Series> restored;
    uint32_t restoredSequence = 0;
    bool loaded = loadHistoryFile(DATA_FILE, restored, restoredSequence);
    if (!loaded) loaded = loadHistoryFile(String(DATA_FILE) + ".bak", restored, restoredSequence);
    if (!loaded) return false;

    HistoryLock lock(_mutex);
    if (!lock.locked()) return false;
    _series = std::move(restored);
    _sequence = restoredSequence;
    _restored = !_series.empty();
    _dirty = false;
    return true;
}

bool PointHistory::writeHistorySnapshot(const std::vector<Series>& snapshot,
                                        uint32_t sequence) const {
    JsonDocument doc;
    doc["schemaVersion"] = 1;
    doc["sequence"] = sequence;
    JsonArray allSeries = doc["series"].to<JsonArray>();
    for (const auto& series : snapshot) {
        JsonObject source = allSeries.add<JsonObject>();
        source["sourceRef"] = series.sourceRef;
        source["sourceRevision"] = series.sourceRevision;
        source["unit"] = series.unit;
        source["stale"] = series.stale;
        if (!series.staleReason.isEmpty()) {
            source["staleReason"] = series.staleReason;
        }
        JsonArray samples = source["samples"].to<JsonArray>();
        for (uint8_t i = 0; i < series.count; ++i) {
            const auto& sample = series.samples[(series.head + i) % MAX_SAMPLES_PER_SOURCE];
            JsonObject item = samples.add<JsonObject>();
            item["value"] = sample.value;
            item["seq"] = sample.sequence;
            item["sampleIdentity"] = sample.sampleIdentity;
            item["bootId"] = sample.bootId;
            item["uptimeMs"] = sample.uptimeMs;
            if (sample.epochMs) item["epochMs"] = sample.epochMs;
            else item["epochMs"] = nullptr;
        }
    }
    if (doc.overflowed() || measureJson(doc) > MAX_FILE_BYTES) return false;
    return atomicWriteJsonWithBackup(DATA_FILE, doc);
}

bool PointHistory::atomicWriteJsonWithBackup(const String& path, JsonDocument& doc) {
    const int slash = path.lastIndexOf('/');
    if (slash > 0) {
        const String parent = path.substring(0, slash);
        if (!LittleFS.exists(parent) && !LittleFS.mkdir(parent)) return false;
    }

    const String tmp = path + ".tmp";
    const String backup = path + ".bak";
    if (LittleFS.exists(tmp)) LittleFS.remove(tmp);
    File file = LittleFS.open(tmp, FILE_WRITE);
    if (!file) return false;
    const size_t expected = measureJson(doc);
    const size_t written = serializeJson(doc, file);
    file.flush();
    file.close();
    if (written != expected) {
        LittleFS.remove(tmp);
        return false;
    }
    File verify = LittleFS.open(tmp, FILE_READ);
    JsonDocument parsed;
    const bool valid = verify && !deserializeJson(parsed, verify);
    if (verify) verify.close();
    if (!valid) {
        LittleFS.remove(tmp);
        return false;
    }

    const bool hadPrimary = LittleFS.exists(path);
    if (LittleFS.exists(backup) && !LittleFS.remove(backup)) {
        LittleFS.remove(tmp);
        return false;
    }
    if (hadPrimary && !LittleFS.rename(path, backup)) {
        LittleFS.remove(tmp);
        return false;
    }
    if (LittleFS.rename(tmp, path)) return true;
    if (hadPrimary) LittleFS.rename(backup, path);
    LittleFS.remove(tmp);
    return false;
}

}  // namespace FastBee

#endif  // FASTBEE_ENABLE_POINT_HISTORY
