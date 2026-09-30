#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include "protocols/FastBeeOTAProtocol.h"
#include "network/OtaConfigStorage.h"

struct Request { std::map<std::string, String> parameters; };
struct Result { bool mqtt = false, modbus = false; int normalized = 0, error = 0; } result;
struct Context { void sendError(Request*, int code, const String&) { result.error = code; } } context;
static bool hasProtocolParam(Request* request, const char* name) { return request->parameters.count(name) != 0; }
static String getParam(Request* request, const char* name, const char* fallback) {
    const auto found = request->parameters.find(name);
    return found == request->parameters.end() ? String(fallback) : found->second;
}
static void normalizeProtocolConfig(JsonDocument& doc) { ++result.normalized; doc["normalized"] = true; }
static bool validateOtaBaseUrl(const String& base, String& error) { return FastBeeOtaProtocol::validateBaseUrl(base, error); }
constexpr int PROTOCOL_CONFIG_VERSION = 2;
static void apply(JsonDocument& doc, Request* request) {
    Context* ctx = &context;
    #define GP(key, fallback) getParam(request, key, fallback)
    #include "production_ota_patch.inc"
    #undef GP
    result.mqtt = updateMqtt;
    result.modbus = updateModbusRtu;
}
static std::string serialized(const JsonDocument& doc) { std::string text; serializeJson(doc, text); return text; }

int main() {
    const char* original = R"({"version":1,"mqtt":{"enabled":false,"scheme":"mqtts","server":"private-broker","port":8883,"username":"existing-user","password":"existing-secret","otaBaseUrl":"https://old/releases","publishTopics":[{"topic":"custom","enabled":false}]},"modbusRtu":{"enabled":true,"master":{"tasks":[{"id":"soil"}]}},"custom":{"keep":42}})";
    for (const char* base : {"http://lan:8080/fw", "https://platform/fw", ""}) {
        JsonDocument doc, expected;
        assert(!deserializeJson(doc, original));
        assert(!deserializeJson(expected, original));
        expected["mqtt"]["otaBaseUrl"] = base;
        Request request{{{"mqtt_otaBaseUrl", base}}};
        result = Result{};
        apply(doc, &request);
        assert(result.error == 0 && !result.mqtt && !result.modbus && result.normalized == 0);
        assert(serialized(doc) == serialized(expected));

        // Reopen persisted data, as both the OTA form and MQTT command do.
        LittleFS.content = serialized(doc);
        JsonDocument loaded;
        String error;
        assert(FastBee::OtaConfigStorage::load(loaded, error));
        assert(String(loaded["mqtt"]["otaBaseUrl"] | "") == base);
        assert(loaded["mqtt"]["password"].isNull());
        assert(loaded["modbusRtu"].isNull());
        String firmware;
        assert(FastBeeOtaProtocol::resolveFirmwareUrl("/file.bin", base, "broker", firmware, error));
        assert(firmware.startsWith(base[0] ? base : "https://broker"));
    }

    for (const char* base : {"ftp://bad", "http:///bad", "https://host?query", "http://user@host"}) {
        JsonDocument doc;
        assert(!deserializeJson(doc, original));
        const std::string before = serialized(doc);
        Request request{{{"mqtt_otaBaseUrl", base}}};
        result = Result{};
        apply(doc, &request);
        assert(result.error == 400 && serialized(doc) == before);
    }
    // A MQTT save without the field cannot erase a previously saved OTA base.
    JsonDocument doc;
    assert(!deserializeJson(doc, original));
    Request mqttSave{{{"mqtt_server", "new-broker"}}};
    result = Result{};
    apply(doc, &mqttSave);
    assert(result.mqtt && String(doc["mqtt"]["otaBaseUrl"] | "") == "https://old/releases");

    JsonDocument loaded;
    String error;
    for (const char* broken : {"{broken", "null", "[]", "{\"mqtt\":{\"otaBaseUrl\":42}}"}) {
        LittleFS.content = broken;
        assert(!FastBee::OtaConfigStorage::load(loaded, error));
    }
    LittleFS.readable = false;
    assert(!FastBee::OtaConfigStorage::load(loaded, error));
    LittleFS.present = false;
    assert(FastBee::OtaConfigStorage::load(loaded, error));
    assert(loaded["mqtt"]["otaBaseUrl"].isNull());
}
