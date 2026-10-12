#include "core/SensorDriverCache.h"

#include "core/DriverRegistry.h"
#include "core/FeatureInstanceRegistry.h"
#include "core/RuntimeFeatureManager.h"
#include "systems/LoggerSystem.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <new>
#include <esp_timer.h>

namespace {
class CacheMutexGuard {
public:
    explicit CacheMutexGuard(void* handle, TickType_t wait = pdMS_TO_TICKS(3000))
        : _handle(static_cast<SemaphoreHandle_t>(handle)),
          _locked(_handle && xSemaphoreTake(_handle, wait) == pdTRUE) {}
    ~CacheMutexGuard() {
        if (_locked) xSemaphoreGive(_handle);
    }
    bool locked() const { return _locked; }
    void unlock() {
        if (_locked) { xSemaphoreGive(_handle); _locked = false; }
    }
    bool lock(TickType_t wait = 0) {
        if (!_locked) _locked = _handle && xSemaphoreTake(_handle, wait) == pdTRUE;
        return _locked;
    }

private:
    SemaphoreHandle_t _handle;
    bool _locked;
};
}

namespace FastBee {

SensorDriverCache& SensorDriverCache::instance() {
    static SensorDriverCache cache;
    static const FeatureInstanceObserver observer{"i2cSensors", [](const void* context) {
        const auto* current = static_cast<const SensorDriverCache*>(context);
        void* mutex = current->_mutex.load(std::memory_order_acquire);
        if (!mutex) return FeatureInstanceSnapshot{FeatureInstanceState::NotInitialized, 0};
        // Status reads must not create locks/drivers, perform I/O, or wait behind a conversion.
        CacheMutexGuard guard(mutex, 0);
        if (!guard.locked()) return FeatureInstanceSnapshot{FeatureInstanceState::Busy, 0};
        uint16_t initialized = 0;
        for (const auto& slot : current->_entries) {
            if (!slot) continue;
            const Entry& entry = *slot;
            if (entry.used && entry.initialized && entry.driver && entry.i2cSensorInstance) ++initialized;
        }
        return FeatureInstanceSnapshot{initialized ? FeatureInstanceState::Initialized
                                                  : FeatureInstanceState::NotInitialized, initialized};
    }, &cache};
    static const bool registered = FeatureInstanceRegistry::instance().registerObserver(&observer);
    (void)registered; // Registry exhaustion leaves the feature unobserved, never fabricates success.
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
    for (auto& slot : _entries) {
        if (!slot) continue;
        Entry& entry = *slot;
        if (entry.used && entry.peripheralId == peripheralId &&
            sensorNameEquals(entry.driverName.c_str(), driverName) && entry.pin == pin &&
            entry.params == params && entry.configVersion == configVersion) {
            return &entry;
        }
    }
    return nullptr;
}

void SensorDriverCache::clearEntry(Entry& entry) {
    for (auto& slot : _entries) {
        if (slot.get() == &entry) {
            slot.reset(); // Entry owns the driver; release only its bus lease.
            return;
        }
    }
}

SensorDriverCache::Entry* SensorDriverCache::allocateEntry(String& errorMsg) {
    for (auto& slot : _entries) {
        if (!slot) {
            slot.reset(new (std::nothrow) Entry());
            if (!slot) errorMsg = "SENSOR_DRIVER_MEMORY_UNAVAILABLE";
            return slot.get();
        }
    }
    Entry* oldest = nullptr;
    for (auto& slot : _entries) {
        Entry& entry = *slot;
        if (entry.readToken) {
            const uint32_t budget = entry.driver->cooperativeReadBudgetMs();
            if (budget && static_cast<uint32_t>(millis()) - entry.lastAttempt < budget) continue;
            entry.readToken = 0;
            entry.sample.success = false;
            entry.driver->deinit();
            entry.initialized = false;
        } // Never evict another caller's unexpired scan.
        if (!oldest || static_cast<int32_t>(entry.lastUse - oldest->lastUse) < 0) oldest = &entry;
    }
    if (oldest) {
        // Reuse the allocated slot on eviction; no transient seventh entry or
        // allocation failure after discarding an existing bounded sample.
        oldest->~Entry();
        new (oldest) Entry();
    } else {
        errorMsg = "SENSOR_DRIVER_CACHE_BUSY";
    }
    return oldest;
}

bool SensorDriverCache::readChannel(const String& peripheralId,
                                    const char* driverName, uint8_t pin,
                                    const char* params, const char* channelName,
                                    float& value, String& unit,
                                    String& errorMsg, uint32_t sampleIntervalMs,
                                    uint32_t* sampleTimestamp, uint32_t configVersion,
                                    const uint32_t* collectionStartedAt, SensorSampleMetadata* sampleMetadata) {
    if (sampleMetadata) *sampleMetadata = {};
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
        for (auto& current : _entries) {
            if (current && current->used && current->peripheralId == peripheralId) clearEntry(*current);
        }
        entry = allocateEntry(errorMsg);
        if (!entry) return false;
        entry->driver = DriverRegistry::getInstance().createDriver(driverName);
        if (!entry->driver) {
            errorMsg = String("SENSOR_DRIVER_UNKNOWN: ") + driverName;
            clearEntry(*entry);
            return false;
        }
        entry->used = true;
        entry->i2cSensorInstance = metadata && metadata->featureKey &&
            std::strcmp(metadata->featureKey, "i2cSensors") == 0;
        entry->peripheralId = peripheralId;
        entry->driverName = driverName;
        entry->params = normalizedParams;
        entry->pin = pin;
        entry->configVersion = configVersion;
    }

    const uint32_t now = static_cast<uint32_t>(millis());
    entry->lastUse = now;
    if (entry->readToken) {
        const uint32_t budget = entry->driver->cooperativeReadBudgetMs();
        if (budget && static_cast<uint32_t>(now - entry->lastAttempt) < budget) {
            errorMsg = "SENSOR_DRIVER_BUSY"; return false;
        }
        // An abandoned caller may have timed out without reacquiring the lock.
        // No background work exists. Reclaim on the next explicit read; passive
        // snapshots only show the invalid pending state and never perform I/O.
        entry->readToken = 0;
        entry->sample.success = false;
        entry->driver->deinit();
        entry->initialized = false;
        snprintf(entry->lastError, sizeof(entry->lastError), "%s", "SENSOR_SCAN_TIMEOUT");
        errorMsg = entry->lastError;
        return false;
    }
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
    const int64_t monotonicUs = esp_timer_get_time();
    if (monotonicUs < 0) { errorMsg = "SENSOR_CLOCK_INVALID"; return false; }
    const uint64_t monotonicMs = static_cast<uint64_t>(monotonicUs) / 1000;
    const uint64_t sampleAge = monotonicMs >= entry->sampledAtMs
        ? monotonicMs - entry->sampledAtMs : UINT64_MAX;
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
        const uint32_t cooperativeBudget = entry->driver->cooperativeReadBudgetMs();
        uint64_t completedGroup = 0;
        bool readOk = false;
        if (!cooperativeBudget) {
            readOk = entry->driver->read(reading);
        } else {
            if (cooperativeBudget > 1200 || _nextReadToken == UINT64_MAX) {
                errorMsg = "SENSOR_COOPERATIVE_ADMISSION_FAILED"; return false;
            }
            const uint64_t token = ++_nextReadToken;
            const uint32_t startedAt = static_cast<uint32_t>(millis());
            entry->lastAttempt = startedAt;
            entry->readToken = token;
            entry->sampleGroup = 0;
            // A new scan owns its diagnostics. An outer timeout or lost cache
            // lock cannot safely retrieve the driver's staged channels, so
            // expose no channels until this scan returns a terminal reading.
            // Never retain the previous successful scan as this scan's data.
            entry->sample = SensorReading();
            for (auto& previousValue : entry->sample.values) previousValue = NAN;
            entry->sampledAtMs = 0;
            for (;;) {
                if (!SensorConfigEpoch::matches(configVersion)) {
                    clearEntry(*entry);
                    errorMsg = "SENSOR_CONFIG_CHANGED"; return false;
                }
                if (metadata && metadata->featureKey &&
                    !RuntimeFeatureManager::instance().shouldEnable(metadata->featureKey)) {
                    clearEntry(*entry);
                    errorMsg = "I2C_SENSORS_UNAVAILABLE"; return false;
                }
                if (static_cast<uint32_t>(millis()) - startedAt >= cooperativeBudget) {
                    entry->driver->deinit();
                    entry->initialized = false;
                    entry->readToken = 0;
                    snprintf(entry->lastError, sizeof(entry->lastError), "%s", "SENSOR_SCAN_TIMEOUT");
                    errorMsg = entry->lastError; return false;
                }
                const SensorReadStep step = entry->driver->readStep(reading);
                if (step == SensorReadStep::Complete || step == SensorReadStep::Failed) {
                    entry->readToken = 0;
                    readOk = step == SensorReadStep::Complete &&
                        static_cast<uint32_t>(millis()) - startedAt < cooperativeBudget;
                    if (readOk) completedGroup = token;
                    break;
                }
                // The entry can be released/replaced/disabled after unlock.
                // Keep only the value token and immutable request identity.
                entry = nullptr;
                guard.unlock();
                if (step == SensorReadStep::Pending) delay(1);
                while (!guard.lock()) {
                    if (static_cast<uint32_t>(millis()) - startedAt >= cooperativeBudget) {
                        errorMsg = "SENSOR_SCAN_TIMEOUT"; return false;
                    }
                    delay(1);
                }
                entry = findExact(peripheralId, driverName, pin, normalizedParams, configVersion);
                if (!entry || entry->readToken != token) {
                    errorMsg = SensorConfigEpoch::matches(configVersion)
                        ? "SENSOR_SCAN_CANCELLED" : "SENSOR_CONFIG_CHANGED";
                    return false;
                }
            }
        }
        bool valid = readOk && reading.success && reading.channelCount == channelCount;
        if (!SensorConfigEpoch::matches(configVersion)) {
            clearEntry(*entry);
            errorMsg = "SENSOR_CONFIG_CHANGED";
            return false;
        }
        for (uint8_t i = 0; valid && i < channelCount; ++i) {
            valid = std::isfinite(reading.values[i]) &&
                (!reading.channelMetadata || reading.channelErrors[i] == SensorChannelError::None);
        }
        const int64_t finishedUs = esp_timer_get_time();
        const uint64_t finishedMs = finishedUs < 0 ? 0 : static_cast<uint64_t>(finishedUs) / 1000;
        const uint32_t finishedTick = static_cast<uint32_t>(millis());
        const uint32_t readingAge = finishedTick - static_cast<uint32_t>(reading.timestamp);
        if (finishedUs < 0 || readingAge >= entry->sampleIntervalMs || readingAge > finishedMs) valid = false;
        if (valid && reading.channelMetadata) {
            for (uint8_t i = 0; i < channelCount; ++i) {
                const uint32_t age = finishedTick - reading.channelTimestamps[i];
                if (age > readingAge || age >= entry->sampleIntervalMs || age > finishedMs) valid = false;
            }
        }
        if (!valid) {
            // Preserve this scan's per-MUX diagnostics, never prior values
            // disguised as a partially refreshed scan. Legacy snapshots retain
            // their established previous-value/invalid semantics.
            if (reading.channelMetadata && reading.channelCount == channelCount)
                entry->sample = reading;
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
        entry->sampleGroup = completedGroup;
        entry->sampledAtMs = finishedMs - readingAge;
        entry->lastError[0] = '\0';
    }
    if (!SensorConfigEpoch::matches(configVersion)) {
        clearEntry(*entry);
        errorMsg = "SENSOR_CONFIG_CHANGED";
        return false;
    }
    value = entry->sample.values[channel];
    unit = entry->driver->getChannelUnit(channel);
    if (unit != entry->driver->getChannelUnit(channel)) { errorMsg = "SENSOR_UNIT_MEMORY_UNAVAILABLE"; return false; }
    const int64_t capturedUs = esp_timer_get_time();
    if (capturedUs < 0) { errorMsg = "SENSOR_CLOCK_INVALID"; return false; }
    SensorSampleMetadata captured;
    const uint32_t timestamp = entry->sample.channelMetadata
        ? entry->sample.channelTimestamps[channel] : static_cast<uint32_t>(entry->sample.timestamp);
    const uint32_t offset = timestamp - static_cast<uint32_t>(entry->sample.timestamp);
    const uint64_t sampledAt = entry->sampledAtMs + offset;
    describeSensorSample(value, sampledAt, static_cast<uint64_t>(capturedUs) / 1000,
        entry->sampleIntervalMs, true, &captured);
    if (captured.quality != SensorSampleQuality::Good) { errorMsg = "SENSOR_READING_STALE"; return false; }
    captured.sampleGroup = entry->sampleGroup;
    if (sampleMetadata) *sampleMetadata = captured;
    if (sampleTimestamp) *sampleTimestamp = timestamp;
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
    for (const auto& slot : _entries) {
        if (!slot) continue;
        const Entry& entry = *slot;
        if (!entry.used || entry.peripheralId != peripheralId) continue;
        reading = entry.sample;
        const auto* metadata = DriverRegistry::getInstance().getMetadata(entry.driverName.c_str());
        const bool featureActive = !metadata || !metadata->featureKey ||
            RuntimeFeatureManager::instance().shouldEnable(metadata->featureKey);
        const bool currentConfig = SensorConfigEpoch::matches(entry.configVersion);
        const int64_t nowUs = esp_timer_get_time();
        const uint64_t nowMs = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
        stale = entry.readToken || !currentConfig || !reading.success || nowUs < 0 || nowMs < entry.sampledAtMs ||
            nowMs - entry.sampledAtMs >= entry.sampleIntervalMs || !featureActive;
        reading.success = !stale;
        errorMsg = entry.readToken ? String("SENSOR_DRIVER_BUSY") : !currentConfig ? String("SENSOR_CONFIG_CHANGED") : !featureActive ? String("I2C_SENSORS_UNAVAILABLE") : entry.lastError[0] ? String(entry.lastError)
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
    for (auto& entry : _entries) {
        if (entry && entry->used && entry->peripheralId == peripheralId) clearEntry(*entry);
    }
}

void SensorDriverCache::releaseAll() {
    if (!ensureMutex()) return;
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) return;
    for (auto& entry : _entries) entry.reset();
}

size_t SensorDriverCache::activeCount() const {
    if (!ensureMutex()) return 0;
    CacheMutexGuard guard(_mutex);
    if (!guard.locked()) return 0;
    size_t count = 0;
    for (const auto& entry : _entries) {
        if (entry && entry->used) ++count;
    }
    return count;
}

} // namespace FastBee
