#ifndef SENSOR_DRIVER_CACHE_H
#define SENSOR_DRIVER_CACHE_H

#include <Arduino.h>
#include <atomic>
#include "core/interfaces/ISensorDriver.h"
#include "core/SensorConfigEpoch.h"

namespace FastBee {

class SensorDriverCache {
public:
    static constexpr size_t MAX_ENTRIES = 6;

    static SensorDriverCache& instance();

    bool readChannel(const String& peripheralId, const char* driverName,
                     uint8_t pin, const char* params, const char* channelName,
                     float& value, String& unit, String& errorMsg,
                     uint32_t sampleIntervalMs = 0,
                     uint32_t* sampleTimestamp = nullptr,
                     uint32_t configVersion = 0,
                     const uint32_t* collectionStartedAt = nullptr);
    // A collection requests a sample from this execution, sharing it between
    // channels. The driver's minimum conversion interval is always respected;
    // nullptr retains ordinary cache-only-within-interval read behavior.
    // Diagnostic snapshot only: never initiates an I2C transaction. A stale or
    // failed sample keeps its values for inspection but has success=false.
    bool getSnapshot(const String& peripheralId, SensorReading& reading,
                     bool& stale, String& errorMsg) const;
    void release(const String& peripheralId);
    void releaseAll();
    size_t activeCount() const;

private:
    SensorDriverCache() = default;
    ~SensorDriverCache();
    SensorDriverCache(const SensorDriverCache&) = delete;
    SensorDriverCache& operator=(const SensorDriverCache&) = delete;

    struct Entry {
        bool used = false;
        bool initialized = false;
        String peripheralId;
        String driverName;
        String params;
        uint8_t pin = 255;
        unsigned long lastUse = 0;
        uint32_t configVersion = 0;
        uint32_t sampleIntervalMs = 0;
        uint32_t lastAttempt = 0;
        bool attempted = false;
        SensorReading sample;
        char lastError[96] = {};
        ISensorDriver* driver = nullptr;
    };

    bool ensureMutex() const;
    Entry* findExact(const String& peripheralId, const char* driverName,
                     uint8_t pin, const String& params, uint32_t configVersion);
    Entry* allocateEntry();
    void clearEntry(Entry& entry);

    Entry _entries[MAX_ENTRIES];
    mutable std::atomic<void*> _mutex{nullptr};
};

} // namespace FastBee

#endif // SENSOR_DRIVER_CACHE_H
