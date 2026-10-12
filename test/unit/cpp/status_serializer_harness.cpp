// Compares the actual three serializers with their frozen old bodies.
#include <Arduino.h>
#undef malloc
#undef free
#include "network/TaskStackSnapshotJson.h"
#include "network/SDCardStatusJson.h"
#include "network/SystemTimeStatus.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

static FastBee::SystemTimeQuality::Snapshot fixtureTime;
static FastBee::SystemTimeZone::LocalSnapshot fixtureLocal;
static unsigned snapshotCalls = 0, localCalls = 0, reasonCalls = 0;

FastBee::SystemTimeQuality::Snapshot FastBee::SystemTimeQuality::snapshot() {
    ++snapshotCalls;
    return fixtureTime;
}
FastBee::SystemTimeZone::LocalSnapshot FastBee::SystemTimeZone::localSnapshot(int64_t epoch) {
    ++localCalls;
    assert(epoch == (fixtureTime.timeValid ? fixtureTime.epochMs : -1));
    return fixtureLocal;
}
const char* FastBee::taskStackCaptureReason(FastBee::TaskStackCaptureStatus state) {
    ++reasonCalls;
    static const char* const reasons[] = {"unavailable", "observed", "busy", "cooldown",
        "memory_pressure", "capacity_exceeded", "allocation_failed", "sdk_failed", "clock_unavailable"};
    const auto index = static_cast<unsigned>(state);
    assert(index < sizeof(reasons) / sizeof(reasons[0]));
    return reasons[index];
}

#include "status_serializer_oracle.inc"

using Trace = std::vector<std::tuple<char, size_t, bool>>;
struct TraceAllocator : ArduinoJson::Allocator {
    explicit TraceAllocator(unsigned limit) : limit(limit) {}
    unsigned limit, allocationCalls = 0;
    Trace trace;
    std::map<void*, size_t> live;
    void* allocate(size_t bytes) override {
        const bool reject = allocationCalls++ >= limit;
        trace.emplace_back('a', bytes, reject);
        if (reject) return nullptr;
        void* pointer = std::malloc(bytes);
        assert(pointer);
        live[pointer] = bytes;
        return pointer;
    }
    void deallocate(void* pointer) override {
        if (!pointer) {
            trace.emplace_back('f', 0, false);
            return;
        }
        auto found = live.find(pointer);
        assert(found != live.end());
        trace.emplace_back('f', found->second, false);
        live.erase(found);
        std::free(pointer);
    }
    void* reallocate(void* pointer, size_t bytes) override {
        const size_t previous = pointer ? live.at(pointer) : 0;
        // ArduinoJson StringBuilder requires shrink reallocations to succeed.
        const bool grows = bytes > previous;
        const bool reject = grows && allocationCalls++ >= limit;
        trace.emplace_back('r', bytes, reject);
        if (reject) return nullptr;
        if (pointer) live.erase(pointer);
        void* result = std::realloc(pointer, bytes);
        assert(result);
        live[result] = bytes;
        return result;
    }
};

struct Result {
    std::string bytes;
    Trace trace;
    bool overflow = false;
    unsigned allocations = 0, snapshots = 0, locals = 0, reasons = 0;
};

static void writeSample(JsonObject object, bool shared, unsigned scenario) {
    FastBee::TaskStackSnapshot sample;
    sample.status = static_cast<FastBee::TaskStackCaptureStatus>(scenario % 9);
    sample.beforeAvailable = scenario & 1;
    sample.afterAvailable = scenario & 2;
    sample.timeAvailable = scenario & 4;
    sample.countBefore = UINT32_MAX;
    sample.countAfter = 0;
    sample.scratchBytes = 8192;
    sample.retryAfterMs = UINT32_MAX;
    sample.sampledAtUptimeMs = UINT64_MAX;
    sample.captureDurationUs = 0x100000001ULL;
    const unsigned variant = scenario / 9;
    sample.count = variant == 0 ? 1 : variant == 1 ? 48 : variant == 2 ? 0 :
                   variant == 3 ? 49 : 1;
    if (variant != 4) {
        sample.entries.reset(new FastBee::TaskStackEntry[FastBee::TaskStackSnapshot::CAPACITY]);
        for (size_t i = 0; i < FastBee::TaskStackSnapshot::CAPACITY; ++i) {
            sample.entries[i].taskNumber = static_cast<uint32_t>(i);
            sample.entries[i].priority = UINT32_MAX;
            sample.entries[i].highWaterBytes = UINT32_MAX - static_cast<uint32_t>(i);
            sample.entries[i].state = static_cast<uint32_t>(i % 5);
        }
    }
    if (shared) FastBee::writeTaskStackSnapshotJson(object, sample);
    else StatusSerializerOracle::writeTaskStackSnapshotJson(object, sample);
}

static void writeSd(JsonObject object, bool shared, unsigned scenario) {
    SDCardStatus status;
    status.statusAvailable = scenario & 1;
    status.configurationAvailable = scenario & 2;
    status.configured = scenario & 4;
    status.mounted = scenario & 8;
    status.capacityValid = scenario & 16;
    status.ownerId = scenario & 32 ? "another-owner" : "expected-owner";
    status.lastError = scenario & 64 ? "" : "NACK \"quoted\" \\ retained failure";
    status.recoverySuggested = true;
    status.ioFailures = UINT32_MAX;
    status.recoveryAttempts = UINT32_MAX - 1;
    status.recoveries = 0;
    status.lastFailureCode = "WRITE_FAILED-after-partial-transfer";
    status.cardType = "SDHC-diagnostic";
    status.cardBytes = UINT64_MAX;
    status.totalBytes = 0x100000001ULL;
    status.usedBytes = 0;
    status.capacityAgeMs = UINT64_MAX;
    if (shared) FastBee::writeSdCardStatusJson(object, status, String("expected-owner"));
    else StatusSerializerOracle::writeSdCardStatusJson(object, status, String("expected-owner"));
    status.lastError = "changed";
    status.lastFailureCode = "changed";
    status.cardType = "changed";
}

static void writeTime(JsonObject object, bool shared, unsigned scenario) {
    fixtureTime = {};
    fixtureTime.timeValid = scenario & 1;
    fixtureTime.synced = scenario & 2;
    fixtureTime.transportAuthenticated = scenario & 4;
    fixtureTime.source = "https";
    fixtureTime.quality = "synced_tls";
    fixtureTime.rtcBootStatus = scenario & 1 ? "applied" : "invalid_clock";
    fixtureTime.epochMs = 1791095200123LL;
    fixtureTime.ageMs = UINT64_MAX;
    fixtureTime.sequence = UINT32_MAX;
    fixtureLocal = {};
    fixtureLocal.timezoneAvailable = scenario & 8;
    fixtureLocal.calendarAvailable = scenario & 16;
    std::strcpy(fixtureLocal.timezone, "CST-8-long-label-that-must-be-owned-by-the-json-document");
    std::strcpy(fixtureLocal.calendar, "2026-10-04 15:00:00");
    if (shared) FastBee::appendSystemTimeStatus(object);
    else StatusSerializerOracle::appendSystemTimeStatus(object);
    fixtureLocal.timezone[0] = 'X';
    fixtureLocal.calendar[0] = 'X';
}

static Result run(unsigned family, unsigned scenario, unsigned limit, bool shared) {
    TraceAllocator allocator(limit);
    Result result;
    snapshotCalls = localCalls = reasonCalls = 0;
    {
        JsonDocument doc(&allocator);
        auto object = doc.to<JsonObject>();
        object["untouched"] = "retain";
        object["mounted"] = true;
        object["capacityQuality"] = "old";
        object["usedBytes"] = 91;
        object["driverError"] = "stale";
        object["datetime"] = "stale";
        object["timestamp"] = 123;
        object["tasks"].to<JsonArray>().add(999);
        if (family == 0) writeSample(object, shared, scenario);
        else if (family == 1) writeSd(object, shared, scenario);
        else writeTime(object, shared, scenario);
        result.overflow = doc.overflowed();
        serializeJson(doc, result.bytes);
        if (!result.overflow && family == 2) {
            assert(object["rtcBootStatus"] == fixtureTime.rtcBootStatus);
            if (scenario & 8) {
                assert(object["activeTimezone"] == "CST-8-long-label-that-must-be-owned-by-the-json-document");
                assert(!object["activeTimezone"].as<JsonString>().isStatic());
            }
            if (scenario & 16) {
                assert(object["datetime"] == "2026-10-04 15:00:00");
                assert(!object["datetime"].as<JsonString>().isStatic());
            }
        }
        result.snapshots = snapshotCalls;
        result.locals = localCalls;
        result.reasons = reasonCalls;
    }
    assert(allocator.live.empty());
    result.trace = allocator.trace;
    result.allocations = allocator.allocationCalls;
    return result;
}

static void check(unsigned family) {
    const unsigned scenarios = family == 0 ? 45 : family == 1 ? 128 : 32;
    bool anyFailure = false;
    for (unsigned scenario = 0; scenario < scenarios; ++scenario) {
        bool completed = false;
        for (unsigned limit = 0; limit < 256; ++limit) {
            const Result before = run(family, scenario, limit, false);
            const Result after = run(family, scenario, limit, true);
            assert(before.bytes == after.bytes);
            assert(before.overflow == after.overflow);
            assert(before.trace == after.trace);
            assert(before.allocations == after.allocations);
            assert(before.snapshots == after.snapshots);
            assert(before.locals == after.locals);
            assert(before.reasons == after.reasons);
            assert(after.snapshots == (family == 2 ? 1U : 0U));
            assert(after.locals == (family == 2 ? 1U : 0U));
            assert(after.reasons == (family == 0 ? 1U : 0U));
            if (!after.overflow) { completed = true; break; }
            anyFailure = true;
        }
        assert(completed);
    }
    assert(anyFailure);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode(argv[1]);
    if (mode == "task") check(0);
    else if (mode == "sd") check(1);
    else if (mode == "time") check(2);
    else assert(false);
}
