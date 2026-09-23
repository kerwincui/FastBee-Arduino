#include "core/SensorDriverCache.h"

#include "core/DriverRegistry.h"
#include "core/RuntimeFeatureManager.h"
#include "systems/LoggerSystem.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace {
class CacheMutexGuard {
public:
    explicit CacheMutexGuard(void* handle)
        : _handle(static_cast<SemaphoreHandle_t>(handle)),
          _locked(_handle && xSemaphoreTake(_handle, pdMS_TO_TICKS(3000)) == pdTRUE) {}
    ~CacheMutexGuard() {
        if (_locked) xSemaphoreGive(_handle);
    }
    bool locked() const { return _locked; }

private:
    SemaphoreHandle_t _handle;
    bool _locked;
};
}

namespace FastBee {

SensorDriverCache& SensorDriverCache::instance() {
    static SensorDriverCache cache;
    return cache;
}

SensorDriverCache::~SensorDriverCache() {
    releaseAll();
    if (void* mutex = _mutex.load(std::memory_order_acquire)) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(mutex));
        _mutex.store(nullptr, std::memory_order_release);
    }
}

bool SensorDriverCache::ensureMutex() const {
    if (_mutex.load(std::memory_order_acquire)) return true;
    void* created = static_cast<void*>(xSemaphoreCreateMutex());
    if (!created) return false;
    void* expected = nullptr;
    if (!_mutex.compare_exchange_strong(expected, created,
            std::memory_order_release, std::memory_order_acquire)) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(created));
    }
    return true;
}

SensorDriverCache::Entry* SensorDriverCache::findExact(
    const String& peripheralId, const char* driverName, uint8_t pin,
    const String& params, uint32_t configVersion) {
    for (Entry& entry : _entries) {
        if (entry.used && entry.peripheralId == peripheralId &&
            sensorNameEquals(entry.driverName.c_str(), driverName) && entry.pin == pin &&
            entry.params == params && entry.configVersion == configVersion) {
            return &entry;
        }
    }
    return nullptr;
}

void SensorDriverCache::clearEntry(Entry& entry) {
    if (entry.driver) {
        entry.driver->deinit();
        delete entry.driver;
    }
    entry = Entry();
}

SensorDriverCache::Entry* SensorDriverCache::allocateEntry() {
    for (Entry& entry : _entries) {
        if (!entry.used) return &entry;
    }
    Entry* oldest = &_entries[0];
    for (Entry& entry : _entries) {
        if (static_cast<int32_t>(entry.lastUse - oldest->lastUse) < 0) oldest = &entry;
    }
    clearEntry(*oldest);
    return oldest;
}

bool SensorDriverCache::readChannel(const String& peripheralId,
                                    const char* driverName, uint8_t pin,
                                    const char* params, const char* channelName,
                                    float& value, String& unit,
                                    String& errorMsg, uint32_t sampleIntervalMs,
                                    uint32_t* sampleTimestamp, uint32_t configVersion,
                                    const uint32_t* collectionStartedAt) {
    value = NAN;
    unit = "";
    if (sampleTimestamp) *sampleTimestamp = 0;
    // Production callers capture this together with their PM snapshot. The
    // zero default supports direct driver users without a configured device.
    if (!configVersion) configVersion = SensorConfigEpoch::current();
    if (!SensorConfigEpoch::matches(configVersion)) {
        errorMsg = "SENSOR_CONFIG_CHANGED";
        return false;
    }
    if (peripheralId.isEmpty() || peripheralId.length() > 40 ||
        !driverName || !driverName[0] || strlen(driverName) > 31 ||
        (params && strlen(params) > 512) || sampleIntervalMs > 86400000UL) {
        errorMsg = "SENSOR_DRIVER_INVALID: peripheral id and driver are required";
        return false;
    }
    const auto* metadata = DriverRegistry::getInstance().getMetadata(driverName);
    if (metadata && metadata->featureKey &&
        !RuntimeFeatureManager::instance().shouldEnable(metadata->featureKey)) {
        if (_mutex.load(std::memory_order_acquire)) release(peripheralId);
        errorMsg = "I2C_SENSORS_UNAVAILABLE";
        return false;
    }
    if (!ensureMutex()) {
        errorMsg = "SENSOR_DRIVER_MUTEX_UNAVAILABLE";
        return false;
    }
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) {
        errorMsg = "SENSOR_DRIVER_BUSY";
        return false;
    }
    if (!SensorConfigEpoch::matches(configVersion)) {
        errorMsg = "SENSOR_CONFIG_CHANGED";
        return false;
    }

    const String normalizedParams = params ? String(params) : String();
    Entry* entry = findExact(peripheralId, driverName, pin, normalizedParams, configVersion);
    if (!entry) {
        // A changed address/mode/pin must replace the old instance for this
        // peripheral before a new one is initialized.
        for (Entry& current : _entries) {
            if (current.used && current.peripheralId == peripheralId) clearEntry(current);
        }
        entry = allocateEntry();
        entry->driver = DriverRegistry::getInstance().createDriver(driverName);
        if (!entry->driver) {
            errorMsg = String("SENSOR_DRIVER_UNKNOWN: ") + driverName;
            clearEntry(*entry);
            return false;
        }
        entry->used = true;
        entry->peripheralId = peripheralId;
        entry->driverName = driverName;
        entry->params = normalizedParams;
        entry->pin = pin;
        entry->configVersion = configVersion;
    }

    const uint32_t now = static_cast<uint32_t>(millis());
    entry->lastUse = now;
    entry->sampleIntervalMs = std::max<uint32_t>(sampleIntervalMs,
        std::max<uint32_t>(1, entry->driver->getMinInterval()));
    if (!entry->initialized) {
        if (entry->attempted && static_cast<uint32_t>(now - entry->lastAttempt) < entry->sampleIntervalMs) {
            errorMsg = entry->lastError;
            return false;
        }
        entry->lastAttempt = now;
        entry->attempted = true;
        if (!entry->driver->init(pin, normalizedParams.isEmpty() ? nullptr : normalizedParams.c_str())) {
            const char* detail = entry->driver->getLastError();
            snprintf(entry->lastError, sizeof(entry->lastError), "%s",
                detail && detail[0] ? detail : "SENSOR_DRIVER_INIT_FAILED");
            entry->driver->deinit();
            errorMsg = entry->lastError;
            return false;
        }
        entry->initialized = true;
        entry->attempted = false;
        entry->sampleIntervalMs = std::max<uint32_t>(sampleIntervalMs,
            std::max<uint32_t>(1, entry->driver->getMinInterval()));
        LOG_INFOF("Sensor cache: initialized %s for %s", driverName, peripheralId.c_str());
    }
    if (!SensorConfigEpoch::matches(configVersion)) {
        clearEntry(*entry);
        errorMsg = "SENSOR_CONFIG_CHANGED";
        return false;
    }

    uint8_t channel = 0;
    const uint8_t channelCount = entry->driver->getChannelCount();
    if (!channelCount || channelCount > 4) {
        errorMsg = "SENSOR_CHANNEL_COUNT_INVALID";
        return false;
    }
    if (channelName && channelName[0]) {
        bool found = false;
        for (uint8_t i = 0; i < channelCount; ++i) {
            if (strcmp(channelName, entry->driver->getChannelName(i)) == 0) {
                channel = i;
                found = true;
                break;
            }
        }
        if (!found) {
            errorMsg = String("SENSOR_CHANNEL_UNKNOWN: ") + channelName;
            return false;
        }
    }
    const uint32_t sampleAge = now - static_cast<uint32_t>(entry->sample.timestamp);
    // Periodic execution is scheduled from dispatch, while a sensor timestamp
    // is taken after conversion. Equal 10-second rule/cache intervals can
    // therefore reuse the previous sample just before expiry on every other
    // round. Request a new physical sample for this collection once the driver
    // permits it; never relabel the old timestamp as a new acquisition.
    const bool previousCollection = collectionStartedAt &&
        static_cast<int32_t>(static_cast<uint32_t>(entry->sample.timestamp) -
                             *collectionStartedAt) < 0;
    const uint32_t driverInterval = std::max<uint32_t>(1, entry->driver->getMinInterval());
    const bool fresh = entry->sample.success && sampleAge < entry->sampleIntervalMs &&
        !(previousCollection && sampleAge >= driverInterval);
    if (!fresh) {
        // Failed channels of the same sample share the error and retry window,
        // rather than hammering an unplugged device once per channel.
        if (entry->attempted && !entry->sample.success &&
            static_cast<uint32_t>(now - entry->lastAttempt) < entry->sampleIntervalMs) {
            errorMsg = entry->lastError;
            return false;
        }
        entry->lastAttempt = now;
        entry->attempted = true;
        SensorReading reading;
        bool valid = entry->driver->read(reading) && reading.success &&
            reading.channelCount == channelCount;
        if (!SensorConfigEpoch::matches(configVersion)) {
            clearEntry(*entry);
            errorMsg = "SENSOR_CONFIG_CHANGED";
            return false;
        }
        for (uint8_t i = 0; valid && i < channelCount; ++i) {
            valid = std::isfinite(reading.values[i]);
        }
        if (valid && static_cast<uint32_t>(static_cast<uint32_t>(millis()) -
                          static_cast<uint32_t>(reading.timestamp)) >= entry->sampleIntervalMs) {
            valid = false;
        }
        if (!valid) {
            entry->sample.success = false;
            // A reset/unplugged sensor can lose calibration or operating mode.
            // Keep the shared retry window, but run init again on the next
            // permitted attempt instead of retrying read forever.
            entry->initialized = false;
            const char* detail = entry->driver->getLastError();
            snprintf(entry->lastError, sizeof(entry->lastError), "%s",
                detail && detail[0] ? detail : "SENSOR_DRIVER_READ_FAILED");
            errorMsg = entry->lastError;
            return false;
        }
        entry->sample = reading;
        entry->lastError[0] = '\0';
    }
    if (!SensorConfigEpoch::matches(configVersion)) {
        clearEntry(*entry);
        errorMsg = "SENSOR_CONFIG_CHANGED";
        return false;
    }
    value = entry->sample.values[channel];
    unit = entry->driver->getChannelUnit(channel);
    if (sampleTimestamp) *sampleTimestamp = static_cast<uint32_t>(entry->sample.timestamp);
    errorMsg = "";
    return true;
}

bool SensorDriverCache::getSnapshot(const String& peripheralId,
                                    SensorReading& reading, bool& stale,
                                    String& errorMsg) const {
    reading = SensorReading();
    stale = true;
    if (!ensureMutex()) { errorMsg = "SENSOR_DRIVER_MUTEX_UNAVAILABLE"; return false; }
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) { errorMsg = "SENSOR_DRIVER_BUSY"; return false; }
    for (const Entry& entry : _entries) {
        if (!entry.used || entry.peripheralId != peripheralId) continue;
        reading = entry.sample;
        const auto* metadata = DriverRegistry::getInstance().getMetadata(entry.driverName.c_str());
        const bool featureActive = !metadata || !metadata->featureKey ||
            RuntimeFeatureManager::instance().shouldEnable(metadata->featureKey);
        const bool currentConfig = SensorConfigEpoch::matches(entry.configVersion);
        stale = !currentConfig || !reading.success || static_cast<uint32_t>(
            static_cast<uint32_t>(millis()) - static_cast<uint32_t>(reading.timestamp)) >= entry.sampleIntervalMs || !featureActive;
        reading.success = !stale;
        errorMsg = !currentConfig ? String("SENSOR_CONFIG_CHANGED") : !featureActive ? String("I2C_SENSORS_UNAVAILABLE") : entry.lastError[0] ? String(entry.lastError)
                                     : (stale ? String("SENSOR_READING_STALE") : String());
        return true;
    }
    errorMsg = "SENSOR_NOT_SAMPLED";
    return false;
}

void SensorDriverCache::release(const String& peripheralId) {
    if (!ensureMutex()) return;
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) return;
    for (Entry& entry : _entries) {
        if (entry.used && entry.peripheralId == peripheralId) clearEntry(entry);
    }
}

void SensorDriverCache::releaseAll() {
    if (!ensureMutex()) return;
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) return;
    for (Entry& entry : _entries) clearEntry(entry);
}

size_t SensorDriverCache::activeCount() const {
    if (!ensureMutex()) return 0;
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) return 0;
    size_t count = 0;
    for (const Entry& entry : _entries) {
        if (entry.used) ++count;
    }
    return count;
}

} // namespace FastBee
