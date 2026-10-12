#include "core/CheckedAdc.h"
#include <Arduino.h>
#include <atomic>
#include <math.h>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"
#include <esp32-hal-periman.h>

namespace FastBee {
namespace {
static_assert(SOC_ADC_PERIPH_NUM <= AdcResourceSnapshot::UNIT_CAPACITY, "ADC snapshot unit capacity");
static_assert(SOC_ADC_PERIPH_NUM * SOC_ADC_MAX_CHANNEL_NUM <= AdcResourceSnapshot::CHANNEL_CAPACITY,
              "ADC snapshot channel capacity");
constexpr size_t MAX_CONSUMERS = AdcResourceSnapshot::CONSUMER_CAPACITY;
std::atomic<SemaphoreHandle_t> adcMutex{nullptr};
struct UnitState {
    adc_oneshot_unit_handle_t handle = nullptr;
    bool cleanupFailed = false;
    esp_err_t lastError = ESP_OK;
    struct Channel { bool seen = false, known = false; uint8_t attenuation = 0, pin = 255; }
        channels[SOC_ADC_MAX_CHANNEL_NUM]{};
} units[SOC_ADC_PERIPH_NUM];
struct ConsumerState {
    bool used = false, transient = false, retiring = false;
    ResourceConsumerIdentity owner{};
    uint8_t pin = 255, unit = 0, channel = 0, attenuation = 0, resolution = 12;
    esp_err_t lastError = ESP_OK;
};
struct ConsumerTable {
    struct Storage { ConsumerState items[MAX_CONSUMERS]{}; };
    Storage* storage = nullptr;
    ConsumerState* begin() { return storage ? storage->items : nullptr; }
    ConsumerState* end() { return storage ? storage->items + MAX_CONSUMERS : nullptr; }
    bool ensure() {
        if (storage) return true;
        void* memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) return false;
        storage = new (memory) Storage{};
        return true;
    }
    void releaseIfEmpty() {
        if (!storage) return;
        for (const auto& item : storage->items) if (item.used) return;
        storage->~Storage();
        heap_caps_free(storage);
        storage = nullptr;
    }
} consumers;
struct CalibrationState {
    adc_cali_handle_t handle = nullptr;
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    uint8_t attenuation = 0;
    bool cleanupFailed = false;
} calibrations[8];
uint8_t nextCalibration = 0;

SemaphoreHandle_t getMutex(bool create) {
    auto mutex = adcMutex.load(std::memory_order_acquire);
    if (mutex || !create) return mutex;
    auto candidate = xSemaphoreCreateMutex();
    if (!candidate) return nullptr;
    if (!adcMutex.compare_exchange_strong(mutex, candidate, std::memory_order_acq_rel)) {
        vSemaphoreDelete(candidate);
        return mutex;
    }
    return candidate;
}
struct Guard {
    SemaphoreHandle_t mutex;
    bool acquired;
    explicit Guard(bool create = true) : mutex(getMutex(create)),
        acquired(mutex && xSemaphoreTake(mutex, create ? pdMS_TO_TICKS(50) : 0) == pdTRUE) {}
    ~Guard() { if (acquired) xSemaphoreGive(mutex); }
    AdcStatus failure() const { return mutex ? AdcStatus::Busy : AdcStatus::NoMemory; }
};
bool mapPin(uint8_t pin, adc_unit_t& unit, adc_channel_t& channel) {
    return adc_oneshot_io_to_channel(pin, &unit, &channel) == ESP_OK &&
        static_cast<unsigned>(unit) < SOC_ADC_PERIPH_NUM &&
        static_cast<unsigned>(channel) < SOC_ADC_CHANNEL_NUM(static_cast<unsigned>(unit));
}
ConsumerState* find(const ResourceConsumerIdentity& owner) {
    for (auto& item : consumers) if (item.used && !item.transient && item.owner.equals(owner)) return &item;
    return nullptr;
}
esp_err_t deleteCalibration(CalibrationState& item) {
    if (!item.handle) return ESP_OK;
    esp_err_t error = ESP_ERR_NOT_SUPPORTED;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    error = adc_cali_delete_scheme_curve_fitting(item.handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    error = adc_cali_delete_scheme_line_fitting(item.handle);
#endif
    if (error == ESP_OK) item = CalibrationState{};
    else item.cleanupFailed = true;
    return error;
}
AdcStatus releaseLocked(ConsumerState& item) {
    auto& unit = units[item.unit];
    size_t peers = 0;
    for (const auto& other : consumers) if (other.used && other.unit == item.unit) ++peers;
    if (peers > 1) { item = ConsumerState{}; return AdcStatus::Ok; }
    item.retiring = true;
    bool foreignPin = perimanGetPinBusType(item.pin) != ESP32_BUS_TYPE_INIT;
    for (const auto& channel : unit.channels)
        if (channel.seen && perimanGetPinBusType(channel.pin) != ESP32_BUS_TYPE_INIT) foreignPin = true;
    if (foreignPin) {
        unit.cleanupFailed = true; unit.lastError = item.lastError = ESP_ERR_INVALID_STATE;
        return AdcStatus::ReleaseFailed;
    }
    // Calibration handles are independent allocations and have checked cleanup.
    // Keep the last unit/consumer reservation if either cleanup is ambiguous.
    for (auto& calibration : calibrations) {
        if (calibration.handle && static_cast<uint8_t>(calibration.unit) == item.unit) {
            const auto error = deleteCalibration(calibration);
            if (error != ESP_OK) {
                unit.cleanupFailed = true; unit.lastError = item.lastError = error;
                return AdcStatus::ReleaseFailed;
            }
        }
    }
    const auto error = unit.handle ? adc_oneshot_del_unit(unit.handle) : ESP_OK;
    if (error != ESP_OK) {
        unit.cleanupFailed = true; unit.lastError = item.lastError = error;
        return AdcStatus::ReleaseFailed;
    }
    unit = UnitState{};
    item = ConsumerState{};
    return AdcStatus::Ok;
}
AdcStatus configureLocked(ConsumerState& item) {
    auto& unit = units[item.unit];
    auto& channel = unit.channels[item.channel];
    if (unit.cleanupFailed || item.retiring) return AdcStatus::ReleaseFailed;
    if (perimanGetPinBusType(item.pin) != ESP32_BUS_TYPE_INIT) {
        unit.lastError = item.lastError = ESP_ERR_INVALID_STATE;
        return AdcStatus::ConfigureFailed;
    }
    if (channel.known && channel.attenuation == item.attenuation) return AdcStatus::Ok;
    adc_oneshot_chan_cfg_t config = {};
    config.atten = static_cast<adc_atten_t>(item.attenuation);
    config.bitwidth = ADC_BITWIDTH_12;
    const auto error = adc_oneshot_config_channel(unit.handle, static_cast<adc_channel_t>(item.channel), &config);
    channel.seen = true;
    channel.pin = item.pin;
    channel.known = error == ESP_OK;
    unit.lastError = item.lastError = error;
    if (error != ESP_OK) return AdcStatus::ConfigureFailed;
    channel.attenuation = item.attenuation;
    return AdcStatus::Ok;
}
AdcStatus acquireLocked(const ResourceConsumerIdentity& owner, uint8_t pin, uint8_t attenuation,
                        uint8_t resolution, bool transient, ConsumerState*& result) {
    result = nullptr;
    if (!owner.specified() || attenuation > 3 || resolution < 9 || resolution > 12) return AdcStatus::Invalid;
    // Validate a bounded, copied identity even when a caller constructed it directly.
    ResourceConsumerIdentity copied;
    if (!copied.assign(owner.role, owner.id)) return AdcStatus::Invalid;
    adc_unit_t mappedUnit;
    adc_channel_t mappedChannel;
    if (!mapPin(pin, mappedUnit, mappedChannel)) return AdcStatus::Invalid;
    // Direct IDF handles do not register in Arduino periman. Never implicitly
    // detach a GPIO/RMT/LEDC/third-party owner to create an ADC channel.
    if (perimanGetPinBusType(pin) != ESP32_BUS_TYPE_INIT) return AdcStatus::NotOwned;
    auto* existing = transient ? nullptr : find(copied);
    if (existing) {
        if (existing->pin != pin || existing->attenuation != attenuation || existing->resolution != resolution)
            return AdcStatus::Invalid; // release the previous configuration explicitly
        result = existing;
        return configureLocked(*existing);
    }
    // Allocate only after validating the requested owner/configuration/pin.
    // The guard protects both table lifetime and all pointers into its storage.
    if (!consumers.ensure()) return AdcStatus::NoMemory;
    for (auto& item : consumers) if (!item.used) { result = &item; break; }
    if (!result) return AdcStatus::Full;
    auto& unit = units[static_cast<unsigned>(mappedUnit)];
    if (unit.cleanupFailed) { result = nullptr; return AdcStatus::ReleaseFailed; }
    auto& item = *result;
    item.used = true; item.transient = transient; item.owner = copied;
    item.pin = pin; item.unit = static_cast<uint8_t>(mappedUnit); item.channel = static_cast<uint8_t>(mappedChannel);
    item.attenuation = attenuation; item.resolution = resolution;
    if (!unit.handle) {
        adc_oneshot_unit_init_cfg_t config = {};
        config.unit_id = mappedUnit;
        config.ulp_mode = ADC_ULP_MODE_DISABLE;
        // Never adopt or delete an Arduino/third-party unit on NOT_FOUND.
        adc_oneshot_unit_handle_t created = nullptr;
        auto error = adc_oneshot_new_unit(&config, &created);
        if (error == ESP_OK && !created) error = ESP_ERR_INVALID_STATE;
        unit.handle = created;
        unit.lastError = error;
        if (error != ESP_OK) {
            if (created) {
                // An abnormal partial SDK allocation keeps its exact owner and
                // cannot enter the read path even if cleanup also fails.
                item.retiring = true; item.lastError = error; unit.cleanupFailed = true;
                const auto released = releaseLocked(item);
                result = nullptr;
                return released == AdcStatus::Ok ? AdcStatus::CreateFailed : released;
            }
            item = ConsumerState{};
            result = nullptr;
            return AdcStatus::CreateFailed;
        }
    }
    const auto configured = configureLocked(item);
    if (configured != AdcStatus::Ok) {
        const auto released = releaseLocked(item);
        result = nullptr;
        return released == AdcStatus::Ok ? configured : released;
    }
    return AdcStatus::Ok;
}
AdcStatus readLocked(ConsumerState& item, uint16_t* samples, uint8_t count) {
    const auto configured = configureLocked(item);
    if (configured != AdcStatus::Ok) return configured;
    for (uint8_t i = 0; i < count; ++i) {
        int raw = -1;
        auto error = adc_oneshot_read(units[item.unit].handle, static_cast<adc_channel_t>(item.channel), &raw);
        if (error == ESP_OK && (raw < 0 || raw > 4095)) error = ESP_ERR_INVALID_STATE;
        units[item.unit].lastError = item.lastError = error;
        if (error != ESP_OK || raw < 0 || raw > 4095) return AdcStatus::ReadFailed;
        samples[i] = static_cast<uint16_t>(raw >> (12 - item.resolution));
        if (i + 1 < count) delayMicroseconds(100);
    }
    return AdcStatus::Ok;
}
}

AdcStatus acquireAdc(const ResourceConsumerIdentity& owner, uint8_t pin, uint8_t attenuation, uint8_t resolution) {
    Guard guard;
    if (!guard.acquired) return guard.failure();
    ConsumerState* item = nullptr;
    const auto status = acquireLocked(owner, pin, attenuation, resolution, false, item);
    // Failed acquisition may have allocated an otherwise empty table. Cleanup
    // failures retain a used owner, so its unit/fault reservation stays alive.
    consumers.releaseIfEmpty();
    return status;
}
AdcStatus releaseAdc(const ResourceConsumerIdentity& owner) {
    Guard guard;
    if (!guard.acquired) return guard.failure();
    auto* item = find(owner);
    const auto status = item ? releaseLocked(*item) : AdcStatus::Ok;
    // releaseLocked and every reference it holds have returned before freeing.
    consumers.releaseIfEmpty();
    return status;
}
AdcStatus readAdcBatch(const ResourceConsumerIdentity& owner, uint16_t* samples, uint8_t count) {
    if (!samples || count == 0 || count > 32) return AdcStatus::Invalid;
    Guard guard;
    if (!guard.acquired) return guard.failure();
    auto* item = find(owner);
    return item ? readLocked(*item, samples, count) : AdcStatus::NotOwned;
}
AdcStatus readTransientAdcBatch(uint8_t pin, uint16_t* samples, uint8_t count) {
    if (!samples || count == 0 || count > 32) return AdcStatus::Invalid;
    Guard guard;
    if (!guard.acquired) return guard.failure();
    // A previous ambiguous transient cleanup cannot be silently reused or
    // discarded by a subsequent read. It remains visible until controlled reboot.
    for (const auto& item : consumers)
        if (item.used && item.transient && item.pin == pin && item.retiring) return AdcStatus::ReleaseFailed;
    ResourceConsumerIdentity owner;
    owner.assign(ResourceConsumerRole::Sensor);
    ConsumerState* item = nullptr;
    const auto acquired = acquireLocked(owner, pin, 3, 12, true, item);
    if (acquired != AdcStatus::Ok) {
        consumers.releaseIfEmpty();
        return acquired;
    }
    const auto read = readLocked(*item, samples, count);
    const auto released = releaseLocked(*item);
    consumers.releaseIfEmpty();
    return released == AdcStatus::Ok ? read : released;
}
bool checkedAdcVoltage(uint8_t pin, uint8_t attenuation, uint8_t resolution, uint16_t raw, float& volts) {
    volts = NAN;
    if (attenuation > 3 || resolution < 9 || resolution > 12 || raw >= (1U << resolution)) return false;
    Guard guard;
    if (!guard.acquired) return false;
    adc_unit_t unit;
    adc_channel_t channel;
    if (!mapPin(pin, unit, channel)) return false;
#if !SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED
    channel = ADC_CHANNEL_0;
#endif
    CalibrationState* selected = nullptr;
    for (auto& item : calibrations) {
        if (item.handle && item.cleanupFailed && item.unit == unit && item.channel == channel && item.attenuation == attenuation)
            return false;
        if (item.handle && !item.cleanupFailed && item.unit == unit && item.channel == channel && item.attenuation == attenuation) {
            selected = &item; break;
        }
    }
    if (!selected) {
        for (auto& item : calibrations) if (!item.handle) { selected = &item; break; }
        if (!selected) selected = &calibrations[nextCalibration++ % 8];
        if (deleteCalibration(*selected) != ESP_OK) return false;
        adc_cali_handle_t handle = nullptr;
        esp_err_t error = ESP_ERR_NOT_SUPPORTED;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t config = {};
        config.unit_id = unit; config.chan = channel;
        config.atten = static_cast<adc_atten_t>(attenuation); config.bitwidth = ADC_BITWIDTH_12;
        error = adc_cali_create_scheme_curve_fitting(&config, &handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_line_fitting_config_t config = {};
        config.unit_id = unit; config.atten = static_cast<adc_atten_t>(attenuation); config.bitwidth = ADC_BITWIDTH_12;
        error = adc_cali_create_scheme_line_fitting(&config, &handle);
#endif
        if (error == ESP_OK && !handle) error = ESP_ERR_INVALID_STATE;
        if (error != ESP_OK) {
            units[static_cast<unsigned>(unit)].lastError = error;
            if (handle) {
                selected->handle = handle; selected->unit = unit; selected->channel = channel;
                selected->attenuation = attenuation; selected->cleanupFailed = true;
                deleteCalibration(*selected); // Failed deletion retains all identity fields.
            }
            return false;
        }
        selected->handle = handle; selected->unit = unit; selected->channel = channel; selected->attenuation = attenuation;
    }
    int millivolts = 0;
    // Hardware is explicitly 12-bit on all supported targets. Public reduced
    // raw values are expanded from that same filtered sample; never resample.
    auto error = adc_cali_raw_to_voltage(selected->handle, static_cast<int>(raw) << (12 - resolution), &millivolts);
    if (error == ESP_OK && millivolts < 0) error = ESP_ERR_INVALID_STATE;
    units[static_cast<unsigned>(unit)].lastError = error;
    if (error != ESP_OK) return false;
    volts = static_cast<float>(millivolts) / 1000.0f;
    return true;
}
void snapshotAdcResources(AdcResourceSnapshot& snapshot) {
    // Reset in place: firmware GCC rejects the implicit aggregate assignment,
    // and a full temporary would add the entire consumer table to HTTP stacks.
    snapshot.observed = false;
    snapshot.initialized = false;
    snapshot.unitCount = snapshot.channelCount = snapshot.consumerCount = 0;
    snapshot.calibrationCount = snapshot.calibrationCleanupPending = 0;
    for (auto& unit : snapshot.units) unit = AdcResourceSnapshot::Unit{};
    for (auto& channel : snapshot.channels) channel = AdcResourceSnapshot::Channel{};
    for (auto& consumer : snapshot.consumers) consumer = AdcResourceSnapshot::Consumer{};
    const auto mutex = getMutex(false);
    if (!mutex) { snapshot.observed = true; return; }
    Guard guard(false);
    if (!guard.acquired) return;
    snapshot.observed = true; snapshot.initialized = true;
    for (uint8_t u = 0; u < SOC_ADC_PERIPH_NUM; ++u) {
        const auto& unit = units[u];
        if (!unit.handle) continue;
        auto& target = snapshot.units[snapshot.unitCount++];
        target.unit = u; target.retained = true; target.cleanupFailed = unit.cleanupFailed; target.lastError = unit.lastError;
        for (uint8_t c = 0; c < SOC_ADC_CHANNEL_NUM(u); ++c) {
            const auto& channel = unit.channels[c];
            if (!channel.seen) continue;
            auto& entry = snapshot.channels[snapshot.channelCount++];
            entry.unit = u; entry.channel = c; entry.pin = channel.pin;
            entry.attenuation = channel.attenuation; entry.configurationKnown = channel.known;
        }
    }
    for (const auto& item : consumers) {
        if (!item.used) continue;
        auto& target = snapshot.consumers[snapshot.consumerCount++];
        target.identity = item.owner; target.pin = item.pin; target.unit = item.unit; target.channel = item.channel;
        target.attenuation = item.attenuation; target.resolution = item.resolution;
        target.transient = item.transient; target.retiring = item.retiring; target.lastError = item.lastError;
    }
    for (const auto& item : calibrations) if (item.handle) {
        ++snapshot.calibrationCount;
        if (item.cleanupFailed) ++snapshot.calibrationCleanupPending;
    }
}
bool adcPinUnavailable(uint8_t pin) {
    if (!getMutex(false)) return false;
    Guard guard(false);
    if (!guard.acquired) return true;
    for (const auto& item : consumers) if (item.used && item.pin == pin) return true;
    for (const auto& unit : units) if (unit.handle)
        for (const auto& channel : unit.channels) if (channel.seen && channel.pin == pin) return true;
    return false;
}
const char* adcStatusName(AdcStatus status) {
    switch (status) {
        case AdcStatus::Ok: return "ADC_OK";
        case AdcStatus::Busy: return "ADC_BUSY";
        case AdcStatus::NoMemory: return "ADC_NO_MEMORY";
        case AdcStatus::Invalid: return "ADC_INVALID";
        case AdcStatus::Full: return "ADC_CONSUMER_CAPACITY";
        case AdcStatus::NotOwned: return "ADC_NOT_OWNED";
        case AdcStatus::CreateFailed: return "ADC_CREATE_FAILED";
        case AdcStatus::ConfigureFailed: return "ADC_CONFIGURE_FAILED";
        case AdcStatus::ReadFailed: return "ADC_READ_FAILED";
        case AdcStatus::ReleaseFailed: return "ADC_RELEASE_FAILED";
        default: return "ADC_CALIBRATION_FAILED";
    }
}
}
