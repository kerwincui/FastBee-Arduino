#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace FastBee {

struct PointHistorySample {
    double value = 0.0;
    uint32_t sequence = 0;
    uint32_t sampleIdentity = 0;
    uint32_t uptimeMs = 0;
    uint64_t epochMs = 0;
    String bootId;
    bool restored = false;
};

struct PointHistorySeriesSnapshot {
    String sourceRef;
    String sourceRevision;
    String unit;
    std::array<PointHistorySample, 10> samples{};
    uint8_t count = 0;
    bool containsRestoredSamples = false;
    bool stale = false;
    String staleReason;
};

class PointHistory {
public:
    static constexpr size_t MAX_SOURCES = 24;
    static constexpr size_t MAX_SAMPLES_PER_SOURCE = 10;
    static constexpr size_t MAX_SOURCE_REF_LENGTH = 96;
    static constexpr size_t MAX_FILE_BYTES = 64 * 1024;
    static constexpr uint32_t FLUSH_DEADLINE_MS = 30000;
    static constexpr const char* DATA_FILE = "/data/point_history.json";
    static constexpr const char* CONFIG_FILE = "/config/point_history.json";

    static PointHistory& instance();

    PointHistory(const PointHistory&) = delete;
    PointHistory& operator=(const PointHistory&) = delete;

    bool initialize();
    bool configure(bool enabled, const std::vector<String>& sourceAllowlist,
                   String& error, bool persist = true,
                   const uint32_t* expectedRevision = nullptr);
    bool recordSample(const String& sourceRef, double value,
                      const String& unit, const String& sourceRevision,
                      uint32_t sampleIdentity = 0, uint32_t uptimeMs = 0,
                      uint64_t epochMs = 0);
    bool getSeries(const String& sourceRef, PointHistorySeriesSnapshot& out) const;
    bool invalidateSource(const String& sourceRef, const String& reason,
                          const String& nextSourceRevision = "");
    void appendSeriesJson(const String& sourceRef, JsonObject out) const;
    void appendConfigJson(JsonObject out) const;
    void appendStatusJson(JsonObject out) const;

    void tick(uint32_t nowMs = 0);
    bool flushNow(uint32_t timeoutMs = 2000);
    bool pauseWrites(uint32_t timeoutMs = 2000);
    bool resumeWrites(bool resetRuntimeState = false);

    bool isEnabled() const { return _enabled; }
    bool isDirty() const { return _dirty; }
    bool wasRestored() const { return _restored; }
    bool lastFlushSucceeded() const { return _lastFlushSucceeded; }
    uint32_t firstDirtyAt() const { return _firstDirtyAt; }
    size_t sourceCount() const;

private:
    PointHistory() = default;

    struct Series {
        String sourceRef;
        String sourceRevision;
        String unit;
        std::array<PointHistorySample, MAX_SAMPLES_PER_SOURCE> samples{};
        uint8_t head = 0;
        uint8_t count = 0;
        bool containsRestoredSamples = false;
        bool stale = false;
        String staleReason;
    };

    bool loadConfig();
    bool loadConfigFile(const String& path);
    bool loadHistory();
    bool loadHistoryFile(const String& path, std::vector<Series>& restored,
                         uint32_t& restoredSequence) const;
    bool writeHistorySnapshot(const std::vector<Series>& snapshot,
                              uint32_t sequence) const;
    bool isAllowedSource(const String& sourceRef) const;
    static bool validSourceRef(const String& sourceRef);
    static bool atomicWriteJsonWithBackup(const String& path, JsonDocument& doc);

    mutable SemaphoreHandle_t _mutex = nullptr;
    bool _initialized = false;
    bool _enabled = false;
    bool _dirty = false;
    bool _restored = false;
    bool _writesPaused = false;
    bool _flushInProgress = false;
    bool _configWriteInProgress = false;
    bool _lastFlushSucceeded = true;
    uint32_t _firstDirtyAt = 0;
    uint32_t _postSnapshotDirtyAt = 0;
    uint32_t _generation = 0;
    uint32_t _configRevision = 0;
    uint32_t _sequence = 0;
    String _bootId;
    std::vector<String> _sourceAllowlist;
    std::vector<Series> _series;
};

}  // namespace FastBee
