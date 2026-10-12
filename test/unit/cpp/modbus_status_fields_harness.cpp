// Full status callback/statistics methods; only allocation/platform seams differ.
#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "serializer_trace_allocator.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
inline void convertToJson(const String& text, JsonVariant value) { value.set(std::string(text)); }
static SerializerTraceAllocator* activeAllocator = nullptr;
static JsonDocument* activeDocument = nullptr;
static std::vector<std::string> effects;
static uint32_t fakeNow = 0;
static unsigned scenario = 0;
static void observe(const char* kind) {
    std::string json;
    if (activeDocument) serializeJson(*activeDocument, json);
    effects.push_back(std::string(kind) + ":" + std::to_string(activeAllocator->calls) + ":" + json);
}
static unsigned long fixtureMillis() {
    observe("millis");
    const uint32_t result = fakeNow;
    if (scenario & 1) ++fakeNow;
    return result;
}
#define millis fixtureMillis
#include "modbus_status_types.inc"
#include "modbus_status_dependencies.inc"
String ModbusFields::getStatus() const { observe("status"); return getStatusBody(); }
uint32_t ModbusFields::getLastPollAgeSec() const { observe("age"); return getLastPollAgeSecBody(); }
static void ownership(JsonVariantConst value, std::vector<std::string>& rows) {
    if (value.is<JsonObjectConst>()) {
        rows.emplace_back("object");
        for (JsonPairConst pair : value.as<JsonObjectConst>()) {
            rows.push_back("key:" + std::string(pair.key().c_str()) + ":" + std::to_string(pair.key().isStatic()));
            ownership(pair.value(), rows);
        }
    } else if (value.is<JsonArrayConst>()) {
        rows.emplace_back("array");
        for (JsonVariantConst item : value.as<JsonArrayConst>()) ownership(item, rows);
    } else if (value.is<const char*>()) {
        const auto string = value.as<JsonString>();
        rows.push_back("text:" + std::to_string(string.size()) + ":" + std::to_string(string.isStatic()));
    } else rows.push_back("scalar:" + std::to_string(value.isNull()) + ":" + std::to_string(value.is<bool>()) +
        ":" + std::to_string(value.is<int64_t>()) + ":" + std::to_string(value.is<uint64_t>()) +
        ":" + std::to_string(value.is<double>()));
}
struct Result {
    std::vector<std::string> json, payloads, types, reads;
    std::vector<bool> overflow;
    JsonAllocationTrace allocations;
    unsigned calls = 0;
    uint32_t lastHash = 0;
    bool operator==(const Result& other) const {
        return json == other.json && payloads == other.payloads && types == other.types &&
            reads == other.reads && overflow == other.overflow && allocations == other.allocations &&
            calls == other.calls && lastHash == other.lastHash;
    }
};
static Result* activeResult = nullptr;
struct DocumentCapture {
    JsonDocument& document;
    explicit DocumentCapture(JsonDocument& value) : document(value) {
        assert(!activeDocument); activeDocument = &document;
    }
    ~DocumentCapture() {
        std::string json; serializeJson(document, json);
        activeResult->json.push_back(json);
        activeResult->overflow.push_back(document.overflowed());
        activeResult->types.emplace_back("document");
        ownership(document.as<JsonVariantConst>(), activeResult->types);
        activeDocument = nullptr;
    }
};
struct ModbusHandler : ModbusFields {
    void _notifyStatusChange();
    String getPollStatistics() const;
};
struct OracleModbusHandler : ModbusFields {
    void _notifyStatusChange();
    String getPollStatistics() const;
};
#include "modbus_status_actual.inc"
#include "modbus_status_original.inc"
#undef millis

template<class Manager>
static void prepare(Manager& manager, unsigned value) {
    scenario = value;
    fakeNow = value == 14 ? UINT32_MAX - 3 : value == 15 ? 3 : 60000;
    manager.isInitialized = value != 1;
    manager.config.mode = value & 1 ? MODBUS_MASTER : MODBUS_SLAVE;
    manager.config.slaveAddress = value ? 247 : 1;
    manager.config.baudRate = value ? 115200 : 9600;
    manager.config.txPin = value ? 43 : 0; manager.config.rxPin = value ? 44 : 0;
    manager.config.dePin = value & 1 ? 21 : 255;
    manager.config.master.taskCount = value ? 8 : 0;
    manager.config.master.deviceCount = value ? 8 : 0;
    manager._totalPollCount = value == 15 ? UINT32_MAX : 100 + value;
    manager._successPollCount = value == 15 ? UINT32_MAX : 67;
    manager._failedPollCount = value == 15 ? UINT32_MAX : 23;
    manager._timeoutPollCount = value == 15 ? UINT32_MAX : 11;
    manager._lastPollTime = value == 0 ? 0 : value >= 14 ? UINT32_MAX - 1000 : 30000;
    manager._slaveBackoffSkips = value == 15 ? UINT32_MAX : 103;
    manager._slaveBackoffEvictions = value == 15 ? UINT32_MAX : 107;
    if (value == 2) manager._lastStatusHash = manager._totalPollCount ^ manager._successPollCount ^
        (manager._failedPollCount << 8) ^ (manager._timeoutPollCount << 16) ^ 0x80000000u;
    for (uint8_t i = 0; i < Manager::SLAVE_BACKOFF_SLOT_COUNT; ++i) {
        auto& s = manager._slaveBackoff[i];
        s.occupied = value == 0 ? false : value == 15 || i < value;
        s.slaveAddress = i + 1;
        s.consecutiveFailures = value == 15 ? UINT8_MAX : i;
        s.backoffLevel = value == 15 ? UINT8_MAX : i % 7;
        s.lastError = value == 15 ? UINT8_MAX : i % 8;
        s.halfOpen = (i & 1) != 0;
        s.cooldownUntil = i % 3 == 0 ? 0 : i % 3 == 1 ? fakeNow + 900 : fakeNow - 900;
        s.lastTouched = fakeNow;
        s.timeoutCount = value == 15 ? UINT32_MAX : 11 + i;
        s.crcErrorCount = value == 15 ? UINT32_MAX : 23 + i;
        s.exceptionCount = value == 15 ? UINT32_MAX : 37 + i;
        s.successCount = value == 15 ? UINT32_MAX : 41 + i;
        s.skippedCount = value == 15 ? UINT32_MAX : 53 + i;
        s.halfOpenProbeCount = value == 15 ? UINT32_MAX : 67 + i;
    }
}
template<class Manager>
static Result run(const std::string& mode, unsigned value, unsigned budget) {
    SerializerTraceAllocator allocator(budget);
    Result result;
    Manager manager{};
    prepare(manager, value);
    effects.clear(); activeAllocator = &allocator; activeResult = &result;
    if (mode == "notify") {
        if (value != 0) manager._statusChangeCallback = [&](const String& json) {
            observe("callback"); result.payloads.emplace_back(json.c_str());
        };
        manager._notifyStatusChange();
        manager._notifyStatusChange(); // Same hash suppresses a second observation.
        if (value >= 8) { ++manager._totalPollCount; manager._notifyStatusChange(); }
    } else {
        const String json = manager.getPollStatistics();
        result.payloads.emplace_back(json.c_str());
    }
    assert(!activeDocument && allocator.live.empty());
    result.lastHash = manager._lastStatusHash;
    result.reads = effects; result.calls = allocator.calls; result.allocations = allocator.trace;
    activeResult = nullptr; activeAllocator = nullptr;
    return result;
}
static void check(const std::string& mode, unsigned value, unsigned budget) {
    const auto actual = run<ModbusHandler>(mode, value, budget);
    const auto original = run<OracleModbusHandler>(mode, value, budget);
    if (!(actual == original)) {
        std::fprintf(stderr, "mismatch mode=%s scenario=%u budget=%u\n", mode.c_str(), value, budget);
        std::abort();
    }
}
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode = argv[1];
    assert(mode == "notify" || mode == "statistics");
    unsigned oomCases = 0;
    for (unsigned value = 0; value < 16; ++value) {
        const auto successful = run<ModbusHandler>(mode, value, UINT_MAX);
        for (bool overflow : successful.overflow) assert(!overflow);
        check(mode, value, UINT_MAX);
        for (unsigned budget = 0; budget <= successful.calls; ++budget) {
            check(mode, value, budget); ++oomCases;
        }
    }
    std::printf("PASS %s scenarios=16 allocation-budgets=%u\n", mode.c_str(), oomCases);
}
