#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "core/SystemConstants.h"
#include <cassert>
#include <climits>
#include <string>
#include <vector>
#include "serializer_trace_allocator.h"
#include "network_serializer_types.inc"

static std::vector<std::string> reads;
static unsigned long fixtureMillis() {
    reads.push_back("millis");
    return ULONG_MAX;
}
struct NetworkFixtureEsp {
    uint32_t getFreeHeap() const { reads.push_back("heap"); return UINT32_MAX; }
};
static NetworkFixtureEsp fixtureEsp;
// Preserve actual call sites while binding SDK getters to deterministic counters.
#define millis fixtureMillis
#define ESP fixtureEsp
#include "network_serializer_actual.inc"
#include "network_serializer_oracle.inc"
#undef ESP
#undef millis

struct NetworkResult {
    std::string bytes, returnedText, ownership;
    JsonAllocationTrace allocation;
    std::vector<std::string> reads;
    bool overflow = false, returned = false;
    unsigned allocations = 0;
};
template<class Serializer> NetworkResult run(unsigned mode, unsigned scenario, unsigned limit) {
    SerializerTraceAllocator allocator(limit);
    NetworkResult result;
    reads.clear();
    {
        WiFiConfig config;
        config.mode = static_cast<NetworkMode>(scenario % 3);
        config.ipConfigType = static_cast<IPConfigType>(scenario % 3);
        config.apChannel = scenario & 1 ? UINT8_MAX : 0;
        config.apHidden = scenario & 2;
        config.enableMDNS = scenario & 4;
        const String text(scenario & 8 ? std::string(130, 'x') : "配置 \"quote\" \\ newline\n");
        config.apSSID = text; config.apPassword = text;
        config.staSSID = text; config.staPassword = text;
        config.staticIP = "192.0.2.15"; config.gateway = "192.0.2.1";
        config.subnet = "255.255.255.0"; config.dns1 = "192.0.2.2";
        config.dns2 = ""; config.customDomain = text;
        if (scenario & 1) config.backupIPs = {String("192.0.2.20"), text, String("")};
        NetworkStatusInfo status;
        status.status = static_cast<NetworkStatus>(scenario % 8);
        status.ipAddress = "198.51.100.8"; status.macAddress = "02:00:00:00:00:01";
        status.ssid = text; status.rssi = scenario & 1 ? INT_MIN : INT_MAX;
        status.currentGateway = "198.51.100.1"; status.currentSubnet = "255.255.255.0";
        status.dnsServer = text; status.internetAvailable = scenario & 2;
        status.failoverCount = UINT8_MAX; status.uptime = ULONG_MAX;
        status.lastConnectionTime = scenario & 4 ? ULONG_MAX : 0;
        JsonDocument doc(&allocator);
        auto object = doc.to<JsonObject>();
        object["existing"] = "preserved unless full path resets document";
        object["backupIPs"].to<JsonArray>().add("old");
        if (mode == 0) result.returned = Serializer::wifiConfigToJson(config, object);
        else if (mode == 1) Serializer::networkStatusToJson(status, object);
        else {
            const String output = Serializer::fullNetworkInfoToJson(config, status, doc);
            result.returnedText.assign(output.c_str(), output.length());
        }
        result.overflow = doc.overflowed();
        serializeJson(doc, result.bytes);
        // Record original borrowing/copy decisions without imposing new policies.
        auto recordStrings = [&result](JsonObject current) {
            for (JsonPair pair : current) {
                result.ownership += pair.key().isStatic() ? 'K' : 'k';
                if (pair.value().is<const char*>())
                    result.ownership += pair.value().as<JsonString>().isStatic() ? 'S' : 's';
                else result.ownership += '-';
            }
        };
        recordStrings(doc.as<JsonObject>());
        if (mode == 2) {
            recordStrings(doc["config"].as<JsonObject>());
            recordStrings(doc["status"].as<JsonObject>());
            assert(result.returnedText == result.bytes);
        }
        config.apSSID = "changed"; config.apPassword = "changed";
        config.customDomain = "changed"; config.backupIPs.clear();
        status.ssid = "changed"; status.dnsServer = "changed";
        std::string afterMutation;
        serializeJson(doc, afterMutation);
        assert(afterMutation == result.bytes);
        result.reads = reads;
    }
    assert(allocator.live.empty());
    result.allocation = allocator.trace;
    result.allocations = allocator.calls;
    return result;
}

static void equal(const NetworkResult& before, const NetworkResult& after) {
    assert(before.bytes == after.bytes && before.returnedText == after.returnedText);
    assert(before.ownership == after.ownership && before.overflow == after.overflow);
    assert(before.returned == after.returned && before.allocations == after.allocations);
    assert(before.allocation == after.allocation && before.reads == after.reads);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string argument(argv[1]);
    const unsigned mode = argument == "config" ? 0 : argument == "status" ? 1 : 2;
    assert(argument == "config" || argument == "status" || argument == "full");
    for (unsigned scenario = 0; scenario < 24; ++scenario) {
        bool failed = false, completed = false;
        for (unsigned limit = 0; limit < 256; ++limit) {
            const auto before = run<OracleNetworkSerializer>(mode, scenario, limit);
            const auto after = run<JsonSerializationHelper>(mode, scenario, limit);
            equal(before, after);
            assert((after.reads == (mode == 2 ?
                std::vector<std::string>{"millis", "heap"} : std::vector<std::string>{})));
            if (mode == 0) assert(after.returned); // Original API reports true even on JSON OOM.
            if (!after.overflow) { completed = true; break; }
            failed = true;
        }
        assert(failed && completed);
    }
}
