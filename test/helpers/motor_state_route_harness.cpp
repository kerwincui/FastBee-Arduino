#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <map>
#include <cstdio>
constexpr int HTTP_GET=1, HTTP_POST=2;
struct AsyncWebServerRequest {
    int verb=HTTP_POST, status=0;
    std::map<String,String> params;
    std::string body;
    int method() const { return verb; }
};
struct WebHandlerContext {
    bool authorized=true;
    bool requireAuth(AsyncWebServerRequest* r) { if(!authorized) r->status=401; return authorized; }
    String getParamValue(AsyncWebServerRequest* r,const char* k,const char* fallback) { auto i=r->params.find(k);return i==r->params.end()?String(fallback):i->second; }
    int getParamInt(AsyncWebServerRequest* r,const char* k,int fallback) { auto i=r->params.find(k);return i==r->params.end()?fallback:std::stoi(i->second.c_str()); }
    void sendError(AsyncWebServerRequest* r,int code,const char*) { r->status=code; }
    void sendBadRequest(AsyncWebServerRequest* r,const String&) { r->status=400; }
};
STATE_TYPE
struct MotorControlService {
    static MotorControlService& instance() { static MotorControlService s;return s; }
    int submitted=0, lookedUp=0;
    bool busy=false, exists=true;
    String referenceForSlave(uint8_t slave) { return slave==9?String("motor.9.configured"):String(); }
    bool validReference(const String& ref) { return ref=="motor.9.configured"; }
    bool state(const String&,MotorControlState& out) {
        out.known=true;out.speed=70;out.pulse=300;
        strcpy(out.lastAcknowledgedAction,"reverse");return true;
    }
    bool submit(const String&,const String&,int,const String&,uint32_t& id,String&) { ++submitted;id=42;return true; }
    bool operation(uint32_t id,JsonObject out,bool* isBusy=nullptr) {
        ++lookedUp;if(isBusy)*isBusy=busy;if(busy || !exists)return false;
        out["operationId"]=id;out["pending"]=false;out["success"]=true;return true;
    }
};
namespace HandlerUtils { void sendJsonStream(AsyncWebServerRequest* r,const JsonDocument& doc) {r->status=200;serializeJson(doc,r->body);} }
bool rejectModbusCriticalControl(AsyncWebServerRequest*,const char*,unsigned) { return false; }
struct ModbusRouteHandler { WebHandlerContext* ctx;void handleModbusMotorControl(AsyncWebServerRequest*); };
#include "motor_route_production.inc"
int main() {
    WebHandlerContext ctx;ModbusRouteHandler route{&ctx};auto& service=MotorControlService::instance();
    AsyncWebServerRequest state;state.verb=HTTP_GET;state.params={{"slaveAddress","9"},{"action","forward"}};
    route.handleModbusMotorControl(&state);
    JsonDocument doc;assert(state.status==200 && !deserializeJson(doc,state.body));
    assert(service.submitted==0 && service.lookedUp==0);
    assert(doc["data"]["speed"]==70 && doc["data"]["pulse"]==300);
    assert(doc["data"]["lastAcknowledgedAction"]=="reverse" && doc["data"]["motionKnown"]==false);
    ctx.authorized=false;state.status=0;route.handleModbusMotorControl(&state);assert(state.status==401);ctx.authorized=true;
    state.params["slaveAddress"]="6";route.handleModbusMotorControl(&state);assert(state.status==404 && !service.submitted);
    AsyncWebServerRequest poll;poll.params={{"action","operation"},{"operationId","42"}};
    service.busy=true;route.handleModbusMotorControl(&poll);assert(poll.status==503);
    service.busy=false;service.exists=false;route.handleModbusMotorControl(&poll);assert(poll.status==404);
    service.exists=true;route.handleModbusMotorControl(&poll);assert(poll.status==200);
    AsyncWebServerRequest launch;launch.params={{"action","forward"},{"slaveAddress","9"},{"requestId","one"}};
    service.busy=true;route.handleModbusMotorControl(&launch);
    assert(launch.status==200 && service.submitted==1 && !deserializeJson(doc,launch.body));
    assert(doc["data"]["operationId"]==42 && doc["data"]["pending"]==true && doc["data"]["accepted"]==true);
    std::puts("Motor route passed: cached GET cannot dispatch, auth, configured identity, busy503 versus unknown404 and durable acceptance ID.");
}
