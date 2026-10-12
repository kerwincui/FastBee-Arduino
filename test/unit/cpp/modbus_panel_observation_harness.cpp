#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "protocols/ModbusRegisterCodec.h"

class String : public std::string {
public:
    using std::string::string;
    String() = default;
    String(const std::string& value) : std::string(value) {}
    String(double value, uint8_t decimals) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(decimals) << value;
        assign(out.str());
    }
};

enum class ObservationQuality { UNKNOWN, GOOD, ERROR };
struct ObservationUpdate {
    String sourceRef, label, unit, dataField, sourceRevision, value, error;
    bool recordable = false;
    uint32_t maxAgeMs = 0, sampleTimestamp = 0;
    ObservationQuality quality = ObservationQuality::UNKNOWN;
};
struct PeriphExecManager {
    std::vector<ObservationUpdate> updates;
    std::vector<std::pair<String, String>> invalidations;
    static PeriphExecManager& getInstance() { static PeriphExecManager instance; return instance; }
    void upsertObservation(const ObservationUpdate& update) { updates.push_back(update); }
    void invalidateObservation(const String& source, const String& reason) {
        invalidations.emplace_back(source, reason);
    }
};
struct Mapping {
    char sensorId[32] = "pm25", unit[8] = "ug/m3";
    uint8_t regOffset = 0, dataType = 0, wordOrder = 0, decimalPlaces = 0;
    float scaleFactor = 1;
};
struct Task {
    bool enabled = true;
    uint8_t mappingCount = 1, slaveAddress = 4, functionCode = 3;
    uint16_t pollInterval = 10, startAddress = 0;
    Mapping mappings[4];
};
struct Config {
    uint8_t mode = 1, rxPin = 43, txPin = 44;
    uint32_t baudRate = 9600;
    struct { uint8_t taskCount = 1; Task tasks[2]; } master;
};
struct Cache {
    bool valid = true;
    size_t count = 2;
    uint16_t values[8] = {42, 76};
    uint32_t timestamp = 1234;
};
struct TestSemaphore {
    bool available = true;
    unsigned takes = 0, gives = 0;
    uint32_t lastTimeout = 0;
};
using SemaphoreHandle_t = TestSemaphore*;
constexpr int pdTRUE = 1;
constexpr uint32_t pdMS_TO_TICKS(uint32_t milliseconds) { return milliseconds; }
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
    assert(semaphore);
    ++semaphore->takes;
    semaphore->lastTimeout = timeout;
    if (!semaphore->available) return 0;
    semaphore->available = false;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t semaphore) {
    assert(semaphore && !semaphore->available);
    ++semaphore->gives;
    semaphore->available = true;
    return pdTRUE;
}
struct Serial {
    bool stopped = false;
    TestSemaphore* semaphore;
    void end() {
        assert(semaphore && !semaphore->available);
        stopped = true;
    }
};
constexpr uint8_t MODBUS_MASTER = 1;
#define LOG_INFO(...) ((void)0)
#include "core/UartPortLease.h"
class ModbusHandler {
public:
    Config config;
    Cache _taskCache[2];
    bool isInitialized = true;
    TestSemaphore semaphore;
    SemaphoreHandle_t _oneShotSemaphore = &semaphore;
    struct { unsigned resets = 0; void reset() { ++resets; } } _motorTravel;
    Serial serial{false, &semaphore};
    Serial* modbusSerial = &serial;
    FastBee::UartPortLease _uartLease;
    unsigned delayInvalidations = 0;
    unsigned delayRetirements = 0;
    void invalidateCoilDelayTasksLocked(const char* reason) {
        assert(_oneShotSemaphore && !semaphore.available);
        assert(std::strcmp(reason, "HANDLER_STOPPED") == 0);
        ++delayInvalidations;
    }
    void retireOrdinaryDelayWork() {
        // The actual end() must finish PM work only after releasing its bus.
        assert(semaphore.available && !_uartLease.owned() && !isInitialized);
        ++delayRetirements;
    }
    void publishPollObservations(uint8_t taskIdx, bool sampled);
    void invalidatePollObservations();
    void end();
};
// Match Arduino Print.h's public number-base macro so helper identifiers
// cannot accidentally compile only in the host harness.
#define HEX 16
#include "production_modbus_observations.inc"

int main() {
    auto& manager = PeriphExecManager::getInstance();
    ModbusHandler handler;
    assert(handler._uartLease.acquire(2));
    auto& mapping = handler.config.master.tasks[0].mappings[0];
    auto& cache = handler._taskCache[0];
    auto publish = [&]() -> ObservationUpdate {
        manager.updates.clear();
        handler.publishPollObservations(0, true);
        assert(manager.updates.size() == 1);
        return manager.updates.back();
    };
    auto value = publish();
    assert(value.sourceRef == "modbus.pm25" && value.value == "42");
    assert(value.quality == ObservationQuality::GOOD && value.recordable);
    assert(value.sampleTimestamp == 1234 && value.maxAgeMs == 30000);
    const String initialRevision = value.sourceRevision;
    assert(!initialRevision.empty() && initialRevision.size() <= 64);
    assert(initialRevision.substr(0, 4) == "mb3:");
    // Every physical/source interpretation field must invalidate history,
    // while new sample values and sampling frequency must not change identity.
    const auto checkRevisionChange = [&](auto& field, const auto alternate) {
        const auto original = field;
        field = alternate;
        assert(publish().sourceRevision != initialRevision);
        field = original;
        assert(publish().sourceRevision == initialRevision);
    };
    checkRevisionChange(handler.config.rxPin, 1);
    checkRevisionChange(handler.config.txPin, 2);
    checkRevisionChange(handler.config.baudRate, 19200);
    checkRevisionChange(handler.config.master.tasks[0].slaveAddress, 5);
    checkRevisionChange(handler.config.master.tasks[0].functionCode, 4);
    checkRevisionChange(handler.config.master.tasks[0].startAddress, 32);
    checkRevisionChange(mapping.regOffset, 1);
    checkRevisionChange(mapping.dataType, 1);
    checkRevisionChange(mapping.wordOrder, 1);
    checkRevisionChange(mapping.scaleFactor, 1.25f);
    checkRevisionChange(mapping.decimalPlaces, 2);
    handler.config.master.tasks[0].pollInterval = 20;
    assert(publish().sourceRevision == initialRevision);
    handler.config.master.tasks[0].pollInterval = 10;
    cache.valid = false;
    assert(publish().sourceRevision == initialRevision);
    cache.valid = true;
    cache.values[0] = 43;
    assert(publish().sourceRevision == initialRevision);
    std::strcpy(mapping.unit, "mg/m3");
    const String changedUnitRevision = publish().sourceRevision;
    assert(changedUnitRevision != initialRevision && changedUnitRevision.size() <= 64);
    std::strcpy(mapping.unit, "ug/m3");
    assert(publish().sourceRevision == initialRevision);
    mapping.scaleFactor = 0.001f;
    assert(publish().sourceRevision != initialRevision);
    mapping.scaleFactor = 1;
    handler.config.master.tasks[0].startAddress = 100;
    assert(publish().sourceRevision != initialRevision);
    handler.config.master.tasks[0].startAddress = 0;

    mapping.dataType = 1; mapping.scaleFactor = 0.1f; mapping.decimalPlaces = 1;
    cache.values[0] = static_cast<uint16_t>(-125);
    assert(publish().value == "-12.5");
    mapping.dataType = 2; mapping.scaleFactor = 1; mapping.decimalPlaces = 0;
    cache.values[0] = 1; cache.values[1] = 2;
    assert(publish().value == "65538");
    cache.values[0] = cache.values[1] = 0xffff;
    assert(publish().value == "4294967295");
    mapping.dataType = 3; cache.values[0] = 0xffff; cache.values[1] = 0xfffe;
    assert(publish().value == "-2");
    mapping.dataType = 4; mapping.decimalPlaces = 2;
    cache.values[0] = 0x4148; cache.values[1] = 0;
    assert(publish().value == "12.50");
    const auto highWordRevision = publish().sourceRevision;
    mapping.wordOrder = 1;
    cache.values[0] = 0; cache.values[1] = 0x4148;
    assert(publish().value == "12.50");
    assert(publish().sourceRevision != highWordRevision);
    mapping.wordOrder = 0;

    mapping.scaleFactor = std::numeric_limits<float>::max(); mapping.decimalPlaces = 6;
    cache.values[0] = 0x7f7f; cache.values[1] = 0xffff;
    assert(publish().value.size() > 70);
    mapping.scaleFactor = 1; mapping.decimalPlaces = 2;

    cache.values[0] = 0x7fc0; cache.values[1] = 0;
    value = publish();
    assert(value.quality == ObservationQuality::ERROR && value.value.empty());
    assert(value.error == "MODBUS_MAPPING_INVALID" && value.sampleTimestamp == 0);
    cache.values[0] = 0x4148; cache.count = 1;
    value = publish();
    assert(value.quality == ObservationQuality::ERROR && value.value.empty());
    cache.count = 2; mapping.regOffset = 7;
    value = publish();
    assert(value.quality == ObservationQuality::ERROR && value.value.empty());
    mapping.regOffset = 0; mapping.dataType = 99;
    value = publish();
    assert(value.quality == ObservationQuality::ERROR && value.value.empty());
    mapping.dataType = 0; mapping.scaleFactor = std::numeric_limits<float>::infinity();
    value = publish();
    assert(value.quality == ObservationQuality::ERROR && value.value.empty());
    mapping.scaleFactor = 1; cache.valid = false;
    value = publish();
    assert(value.error == "MODBUS_READ_FAILED" && value.value.empty());
    assert(value.quality == ObservationQuality::ERROR && value.sampleTimestamp == 0);

    manager.updates.clear();
    handler.publishPollObservations(0, false);
    assert(manager.updates[0].quality == ObservationQuality::UNKNOWN);
    assert(manager.updates[0].value.empty());
    manager.updates.clear();
    handler.publishPollObservations(2, true);
    handler.config.master.tasks[0].enabled = false;
    handler.publishPollObservations(0, true);
    assert(manager.updates.empty());

    handler.config.master.tasks[0].enabled = true;
    handler.config.master.tasks[0].mappingCount = 2;
    std::strcpy(handler.config.master.tasks[0].mappings[1].sensorId, "pm10");
    // Execute the production guard/end bodies. Missing/busy ownership must
    // preserve UART, travel state and published observations, never fake stop.
    handler._oneShotSemaphore = nullptr;
    handler.end();
    assert(handler.isInitialized && !handler.serial.stopped);
    assert(manager.invalidations.empty() && handler._motorTravel.resets == 0);
    assert(handler.semaphore.takes == 0 && handler.semaphore.gives == 0);
    assert(handler.delayInvalidations == 0);
    assert(handler.delayRetirements == 0);
    handler._oneShotSemaphore = &handler.semaphore;
    handler.semaphore.available = false;
    handler.end();
    assert(handler.isInitialized && !handler.serial.stopped);
    assert(manager.invalidations.empty() && handler._motorTravel.resets == 0);
    assert(handler.semaphore.takes == 1 && handler.semaphore.gives == 0);
    assert(handler.semaphore.lastTimeout == 2000);
    assert(handler.delayInvalidations == 0);
    assert(handler.delayRetirements == 0);
    handler.semaphore.available = true;
    handler.end();
    assert(!handler.isInitialized && handler.serial.stopped);
    assert(handler._motorTravel.resets == 1);
    assert(handler.delayInvalidations == 1);
    assert(handler.delayRetirements == 1);
    assert(handler.semaphore.available && handler.semaphore.gives == 1);
    assert(manager.invalidations.size() == 2);
    assert(manager.invalidations[0].first == "modbus.pm25");
    assert(manager.invalidations[1].first == "modbus.pm10");
    assert(manager.invalidations[0].second == "MODBUS_CONFIGURATION_CHANGED");
    handler.end();
    assert(manager.invalidations.size() == 2);
    assert(handler._motorTravel.resets == 2);
    assert(handler.delayInvalidations == 2);
    assert(handler.delayRetirements == 2);
    assert(handler.semaphore.available && handler.semaphore.gives == 2);
    std::cout << "Modbus panel observations: mapping, quality, history and guarded stop lifecycle passed\n";
}
