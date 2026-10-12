#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <map>
#include "core/ModbusOrdinaryControl.h"
#include "core/SensorConfigEpoch.h"
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
    void sendError(AsyncWebServerRequest* r,int code,const String&) { r->status=code; }
    void sendBadRequest(AsyncWebServerRequest* r,const String&) { r->status=400; }
};
namespace FastBee {
bool beginHttpMotorOutput(WebHandlerContext&,AsyncWebServerRequest*,const String& target,uint32_t epoch,
    OrdinaryControlContext& context) {
    assert(target=="mb:1:9");context.configEpoch=epoch;context.request.operation=1;
    context.request.actor={OrdinaryControl::Source::Http,42};return true;
}
}
struct PeripheralManager {
    unsigned finishes=0;
    static PeripheralManager& getInstance(){static PeripheralManager value;return value;}
    void finishOrdinaryControl(const FastBee::OrdinaryControlContext& context,bool cancelled) {
        assert(context.request.operation==1 && !cancelled);++finishes;
    }
};
STATE_TYPE
struct MotorControlService {
    static MotorControlService& instance() { static MotorControlService s;return s; }
    int submitted=0, lookedUp=0;
    bool busy=false, exists=true;
    unsigned confirmations=0;
    bool confirmTravel(const String& ref,const String& token,bool,int32_t,bool stationary,String& error) {
        ++confirmations;
        if(ref!="motor.9.configured" || token!="fresh" || !stationary) {error="TRAVEL_STATE_CHANGED";return false;}
        return true;
    }
    String referenceForSlave(uint8_t slave) { return slave==9?String("motor.9.configured"):String(); }
    bool snapshotTarget(const String& ref,FastBee::ModbusControlTarget& target) {
        target.instance=1;target.configuration=1;target.slave=9;return ref=="motor.9.configured";
    }
    bool validReference(const String& ref) { return ref=="motor.9.configured"; }
    bool state(const String&,MotorControlState& out) {
        out.known=true;out.speed=70;out.pulse=300;
        strcpy(out.lastAcknowledgedAction,"reverse");return true;
    }
    bool submit(const String&,const String& action,int,const String&,uint32_t& id,String&,
        const FastBee::OrdinaryControlContext* context=nullptr) {
        assert(action=="readStatus" ? !context : context && context->request.actor.identity==42);
        ++submitted;id=42;return true;
    }
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
    AsyncWebServerRequest confirm;confirm.params={{"action","confirmReference"},{"motorRef","motor.9.configured"},
        {"travelToken","fresh"},{"stationaryConfirmed","true"},{"position","-79"}};
    ctx.authorized=false;route.handleModbusMotorControl(&confirm);assert(confirm.status==401 && !service.confirmations);
    ctx.authorized=true;
    for(const char* invalid : {"", "1.5", "10mm", "2147483648", "-2147483649"}) {
        confirm.params["position"]=invalid;route.handleModbusMotorControl(&confirm);
        assert(confirm.status==400 && !service.confirmations);
    }
    confirm.params["position"]="-79";confirm.params["stationaryConfirmed"]="false";
    route.handleModbusMotorControl(&confirm);assert(confirm.status==409);
    confirm.params["stationaryConfirmed"]="true";route.handleModbusMotorControl(&confirm);assert(confirm.status==200);
    confirm.params["travelToken"]="stale";route.handleModbusMotorControl(&confirm);assert(confirm.status==409);
    assert(service.submitted==1); // Reference confirmation never submits physical motion.
    assert(PeripheralManager::getInstance().finishes==1);
    AsyncWebServerRequest read;read.params={{"action","readStatus"},{"slaveAddress","9"}};
    route.handleModbusMotorControl(&read);
    assert(read.status==200 && service.submitted==2 && PeripheralManager::getInstance().finishes==1);
    std::puts("Motor route passed: cached GET, auth, finite input validation, stale confirmation, no motion on reference confirmation.");
}
