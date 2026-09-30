#include "protocols/ModbusReadDiagnostics.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
using String = std::string;
namespace Protocols { constexpr int MODBUS_ONESHOT_BUFFER_SIZE = 32; }
enum OneShotError { ONESHOT_SUCCESS, ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR,
    ONESHOT_EXCEPTION, ONESHOT_NOT_INITIALIZED, ONESHOT_BUSY };
struct OneShotResult { OneShotError error=ONESHOT_SUCCESS; uint16_t count=1, data[32]{}; uint8_t exceptionCode=2; };
struct AsyncWebServerRequest {
    std::map<std::string,int> params;
    int status=0;
    std::string body;
    void send(int code, const char*, const std::string& value) { status=code; body=value; }
};
struct WebHandlerContext {
    bool requireAuth(AsyncWebServerRequest*) { return true; }
    int getParamInt(AsyncWebServerRequest* request,const char* key,int fallback) {
        const auto found=request->params.find(key);
        return found==request->params.end()?fallback:found->second;
    }
    void sendBadRequest(AsyncWebServerRequest* request,const char* message) { request->send(400,"application/json",message); }
};
namespace HandlerUtils {
void sendJsonStream(AsyncWebServerRequest* request, const JsonDocument& doc) {
    std::string body; serializeJson(doc,body); request->send(200,"application/json",body);
}
}
struct ModbusHandler {
    unsigned calls=0;
    OneShotError next=ONESHOT_SUCCESS;
    OneShotResult readRegistersOnce(uint8_t slave,uint8_t fc,uint16_t start,uint16_t quantity,
                                    bool,uint16_t,uint8_t,String*,ModbusReadDiagnostics* trace) {
        ++calls; assert(trace);
        const uint8_t tx[]={slave,fc,static_cast<uint8_t>(start>>8),static_cast<uint8_t>(start),0,static_cast<uint8_t>(quantity),0,0};
        trace->reset(tx,8); trace->beginAttempt(); trace->txWritten=8;
        trace->reason=next==ONESHOT_SUCCESS?"FRAME_VALIDATED":"NO_RESPONSE_BYTES";
        OneShotResult result; result.error=next; result.count=quantity; result.data[0]=255; return result;
    }
    bool getDebugSnapshot(String& tx,String& rx) { tx="OTHER REQUEST"; rx="OTHER REQUEST"; return true; }
} bus;
struct Lease { ModbusHandler* get() { return &bus; } };
Lease getModbusMaster(WebHandlerContext*,AsyncWebServerRequest*) { return {}; }
bool rejectModbusHeavyRequest(AsyncWebServerRequest*,const char*,unsigned) { return false; }
struct ModbusRouteHandler { WebHandlerContext* ctx; void handleModbusRegisterRead(AsyncWebServerRequest*); };
#include "modbus_read_route_production.inc"
int main() {
    WebHandlerContext context; ModbusRouteHandler handler{&context};
    for (auto bad : {std::pair<const char*,int>{"slaveAddress",264}, {"slaveAddress",-1},
            {"startAddress",-1},{"startAddress",65536},{"functionCode",259},
            {"quantity",-1},{"quantity",0},{"quantity",33},{"quantity",65537}}) {
        AsyncWebServerRequest request; request.params[bad.first]=bad.second;
        bus.calls=0; handler.handleModbusRegisterRead(&request);
        assert(request.status==400 && bus.calls==0);
    }
    AsyncWebServerRequest boundary; boundary.params={{"startAddress",65535},{"quantity",2}};
    bus.calls=0; handler.handleModbusRegisterRead(&boundary);
    assert(boundary.status==400 && bus.calls==0);
    boundary.params["quantity"]=1; handler.handleModbusRegisterRead(&boundary);
    assert(boundary.status==200 && bus.calls==1);
    for (const auto code : {ONESHOT_SUCCESS,ONESHOT_TIMEOUT,ONESHOT_EXCEPTION}) {
        bus.next=code;
        AsyncWebServerRequest request; request.params={{"slaveAddress",8},{"startAddress",5}};
        handler.handleModbusRegisterRead(&request);
        assert(request.status==(code==ONESHOT_SUCCESS?200:code==ONESHOT_TIMEOUT?504:422));
        JsonDocument doc; assert(!deserializeJson(doc,request.body));
        assert(doc["diagnostic"]["txPrefix"]=="08 03 00 05 00 01 00 00");
        assert(doc["diagnostic"]["rxPrefix"]=="");
        assert(doc["diagnostic"]["scope"]=="request:lastAttempt");
        if (code==ONESHOT_SUCCESS) assert(doc["data"]["values"][0]==255);
        else assert(doc["success"]==false);
    }
    std::puts("read route validation, request-owned evidence and HTTP codes passed");
}
