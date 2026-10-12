// Complete real status serializer, frozen original, real AJ/helper/retirement types.
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "protocols/MqttReportRetirement.h"
#include "serializer_trace_allocator.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define MQTT_CLIENT_H
#include "mqtt_status_types.inc"
static JsonDocument* observedDocument = nullptr;
static SerializerTraceAllocator* observedAllocator = nullptr;
static MQTTClient::QueueSnapshot* observedSnapshot = nullptr;
static FastBee::MqttReportRetirement::Snapshot history;
static std::vector<std::string> effects;
static unsigned scenario = 0, historyReads = 0;
FastBee::MqttReportRetirement::Snapshot MQTTClient::getReportRetirementSnapshot() {
    ++historyReads;
    std::string before;
    serializeJson(*observedDocument, before);
    effects.push_back("history:" + std::to_string(observedAllocator->calls) + ":" + before);
    // Observes the first two writes, then changes an externally mutable snapshot.
    // This detects movement of the history read across the queue field reads.
    if (scenario & 1) {
        observedSnapshot->depth = 9;
        observedSnapshot->pendingAttempts = 107;
        observedSnapshot->stats.enqueueTotal = 109;
    }
    return history;
}
#include "protocols/MqttReportQueueStatus.h"
namespace FastBee {
#include "mqtt_status_original.inc"
}

static MQTTClient::QueueSnapshot prepare(unsigned value) {
    scenario = value;
    history = {};
    MQTTClient::QueueSnapshot snapshot;
    if (value != 0) {
        snapshot.depth = value == 9 ? UINT8_MAX : 9;
        snapshot.ringDepth = value == 9 ? UINT8_MAX : 8;
        snapshot.pendingReport = (value & 1) != 0;
        snapshot.consumerActive = (value & 2) != 0;
        snapshot.pendingAttempts = value == 9 ? UINT32_MAX : 23;
        snapshot.retryDelayMs = value == 9 ? UINT32_MAX : 29;
        auto& s = snapshot.stats;
        s.enqueueTotal = value == 9 ? UINT32_MAX : 31;
        s.dropTotal = value == 9 ? UINT32_MAX : 37;
        s.publishTotal = value == 9 ? UINT32_MAX : 41;
        s.throttleDrops = value == 9 ? UINT32_MAX : 43;
        s.invalidDrops = value == 9 ? UINT32_MAX : 47;
        s.evictedOldest = value == 9 ? UINT32_MAX : 53;
        s.publishRejected = value == 9 ? UINT32_MAX : 59;
        s.allocationDeferred = value == 9 ? UINT32_MAX : 61;
        s.testModeRejected = value == 9 ? UINT32_MAX : 67;
        s.configuredTopicAccepted = value == 9 ? UINT32_MAX : 71;
        history.instances = value == 9 ? UINT32_MAX : 73;
        history.instancesWithLoss = value == 9 ? UINT32_MAX : 79;
        history.discardedReports = value == 9 ? UINT32_MAX : 83;
        history.unconfirmedInstances = value == 9 ? UINT32_MAX : 89;
        history.lastLossRingDepth = value == 9 ? UINT8_MAX : 8;
        history.lastLossPendingReport = (value & 1) != 0;
        history.saturated = value == 9;
        for (uint8_t i = 0; i < FastBee::MqttReportRetirement::ReasonCount; ++i)
            history.byReason[i] = value == 9 ? UINT32_MAX : 97 + i;
    }
    history.lastLossReason = static_cast<FastBee::MqttRetirementReason>(value == 8 ? 255 : value);
    return snapshot;
}
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
    } else {
        rows.push_back("scalar:" + std::to_string(value.isNull()) + ":" + std::to_string(value.is<bool>()) +
            ":" + std::to_string(value.is<int64_t>()) + ":" + std::to_string(value.is<uint64_t>()) +
            ":" + std::to_string(value.is<double>()));
    }
}
struct Result {
    std::string json;
    bool overflowed = false;
    unsigned calls = 0;
    JsonAllocationTrace allocations;
    std::vector<std::string> types, reads;
    bool operator==(const Result& other) const {
        return json == other.json && overflowed == other.overflowed && calls == other.calls &&
            allocations == other.allocations && types == other.types && reads == other.reads;
    }
};
static Result run(bool original, const std::string& mode, unsigned value, unsigned budget) {
    auto snapshot = prepare(value);
    SerializerTraceAllocator allocator(UINT_MAX);
    Result result;
    {
        JsonDocument doc(&allocator);
        JsonObject out = doc.to<JsonObject>();
        if (mode == "reused") {
            out["kept"] = std::string("existing owned Unicode 中文 field");
            if (value % 3 == 0) {
                out["reportQueue"]["available"] = std::string("replace wrong type");
                out["reportQueue"]["retirement"]["byReason"]["old"] = UINT32_MAX;
                out["reportQueue"]["depth"] = std::string(100, 'x');
            } else if (value % 3 == 1) {
                out["reportQueue"].to<JsonArray>().add(std::string(100, 'a'));
            } else out["reportQueue"] = std::string(100, 's');
        }
        assert(!doc.overflowed());
        allocator.calls = 0; allocator.limit = budget; allocator.trace.clear();
        observedDocument = &doc; observedAllocator = &allocator; observedSnapshot = &snapshot;
        effects.clear(); historyReads = 0;
        const MQTTClient::QueueSnapshot* supplied = mode == "absent" ? nullptr : &snapshot;
        const JsonObject destination = mode == "unbound" ? JsonObject() : out;
        if (original) FastBee::addMqttReportQueueStatusOriginal(destination, supplied);
        else FastBee::addMqttReportQueueStatus(destination, supplied);
        assert(historyReads == 1);
        result.overflowed = doc.overflowed();
        serializeJson(doc, result.json); ownership(doc.as<JsonVariantConst>(), result.types);
        snapshot = {}; history = {};
        std::string after; serializeJson(doc, after); assert(after == result.json);
        result.reads = effects;
        observedDocument = nullptr; observedAllocator = nullptr; observedSnapshot = nullptr;
    }
    assert(allocator.live.empty());
    result.calls = allocator.calls; result.allocations = allocator.trace;
    return result;
}
static void check(const std::string& mode, unsigned value, unsigned budget) {
    const auto actual = run(false, mode, value, budget);
    const auto original = run(true, mode, value, budget);
    if (!(actual == original)) {
        std::fprintf(stderr, "mismatch mode=%s scenario=%u budget=%u\n", mode.c_str(), value, budget);
        std::abort();
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    assert(mode == "present" || mode == "absent" || mode == "reused" || mode == "unbound");
    unsigned oomCases = 0;
    for (unsigned value = 0; value < 10; ++value) {
        const auto successful = run(false, mode, value, UINT_MAX);
        assert(!successful.overflowed);
        check(mode, value, UINT_MAX);
        for (unsigned budget = 0; budget <= successful.calls; ++budget) {
            check(mode, value, budget); ++oomCases;
        }
    }
    std::printf("PASS %s scenarios=10 allocation-budgets=%u\n", mode.c_str(), oomCases);
}

