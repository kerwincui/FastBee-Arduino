#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
using String = std::string;
namespace Protocols { constexpr int MODBUS_MAX_WRITE_COILS = 32; }
enum OneShotError { ONESHOT_SUCCESS, ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR };
struct OneShotResult {
    OneShotError error = ONESHOT_SUCCESS;
    uint16_t count = 4;
    uint16_t data[32] = {1, 0, 1, 0};
};
struct AsyncWebServerRequest {
    std::map<String, int> params{{"slaveAddress", 6}, {"channelCount", 4}, {"coilBase", 0}};
    String mode = "coil", body;
    int status = 0;
};
struct WebHandlerContext {
    bool requireAuth(AsyncWebServerRequest*) { return true; }
    int getParamInt(AsyncWebServerRequest* r, const char* key, int fallback) {
        return r->params.count(key) ? r->params.at(key) : fallback;
    }
    String getParamValue(AsyncWebServerRequest* r, const char*, const char*) { return r->mode; }
    void sendBadRequest(AsyncWebServerRequest* r, const char*) { r->status = 400; }
};
struct ModbusHandler {
    OneShotResult reply;
    int calls = 0;
    uint8_t function = 0;
    uint16_t base = 0, quantity = 0;
    OneShotResult readRegistersOnce(uint8_t, uint8_t fc, uint16_t address, uint16_t count, bool control) {
        assert(control); ++calls; function = fc; base = address; quantity = count; return reply;
    }
} bus;
struct Lease { ModbusHandler* get() { return &bus; } };
Lease getModbusMaster(WebHandlerContext*, AsyncWebServerRequest*) { return {}; }
bool rejectModbusHeavyRequest(AsyncWebServerRequest*, const char*, size_t) { return false; }
void addModbusDebug(JsonDocument&, ModbusHandler*) {}
void sendOneShotError(WebHandlerContext*, AsyncWebServerRequest* r, const OneShotResult&) {
    r->status = 502; r->body = "{\"success\":false}";
}
namespace HandlerUtils {
void sendJsonStream(AsyncWebServerRequest* r, JsonDocument& doc) {
    r->status = 200; serializeJson(doc, r->body);
}
}
struct ModbusRouteHandler {
    WebHandlerContext* ctx;
    void handleModbusCoilStatus(AsyncWebServerRequest*);
};
#include "relay_status.inc"
int main(int argc, char** argv) {
    assert(argc == 2);
    const String test = argv[1];
    WebHandlerContext context; ModbusRouteHandler route{&context};
    if (test == "exact") {
        for (const char* mode : {"coil", "register"}) {
            AsyncWebServerRequest r; r.mode = mode; r.params["coilBase"] = 65532;
            route.handleModbusCoilStatus(&r);
            assert(bus.quantity == 4 && bus.base == 65532);
            assert(bus.function == (r.mode == "coil" ? 1 : 3));
            assert(r.status == 200);
            JsonDocument result; assert(!deserializeJson(result, r.body));
            assert(result["data"]["states"].size() == 4);
            assert(result["data"]["states"][0].as<bool>());
            assert(!result["data"]["states"][1].as<bool>());
        }
    } else if (test == "bounds") {
        for (const auto& entry : std::map<String, int>{{"slaveAddress", 262}, {"channelCount", 65540}, {"coilBase", -1}}) {
            AsyncWebServerRequest r; r.params[entry.first] = entry.second; bus.calls = 0;
            route.handleModbusCoilStatus(&r); assert(r.status == 400 && bus.calls == 0);
        }
        for (int count : {0, -1, 33}) {
            AsyncWebServerRequest r; r.params["channelCount"] = count; bus.calls = 0;
            route.handleModbusCoilStatus(&r); assert(r.status == 400 && bus.calls == 0);
        }
        AsyncWebServerRequest r; r.params["coilBase"] = 65533; bus.calls = 0;
        route.handleModbusCoilStatus(&r); assert(r.status == 400 && bus.calls == 0);
        r = {}; r.params["coilBase"] = 65535; r.params["channelCount"] = 1; bus.reply.count = 1;
        route.handleModbusCoilStatus(&r); assert(r.status == 200 && bus.quantity == 1);
    } else if (test == "partial") {
        for (int count : {0, 3, 5}) {
            AsyncWebServerRequest r; bus.reply.count = count;
            route.handleModbusCoilStatus(&r);
            assert(r.status != 200 && r.body.find("states") == String::npos);
        }
    } else if (test == "errors") {
        AsyncWebServerRequest r; bus.reply.error = ONESHOT_TIMEOUT;
        route.handleModbusCoilStatus(&r); assert(r.status != 200);
        r = {}; r.mode = "invalid"; bus.calls = 0;
        route.handleModbusCoilStatus(&r); assert(r.status == 400 && bus.calls == 0);
    } else { assert(false); }
}
