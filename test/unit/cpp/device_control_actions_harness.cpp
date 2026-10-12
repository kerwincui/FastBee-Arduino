#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <set>
#include "control_types.inc"
constexpr size_t CONTROL_ITEM_MAX_BYTES = 6144;
constexpr uint8_t CONTROL_GROUP_COUNT = 6;
struct ExecAction { uint8_t actionType=0; String targetPeriphId, actionValue; };
struct Trigger { int triggerType=0, operatorType=0; };
struct PeriphExecRule { String id, name; bool enabled=true; std::vector<ExecAction> actions; std::vector<Trigger> triggers; };
struct PeripheralConfig { String name; };
class PeripheralManager {
public:
    static PeripheralManager& getInstance() { static PeripheralManager p; return p; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& p) { p.name = id + " label\"\n"; return true; }
};
class PeriphExecManager {
public:
    static constexpr uint32_t SENSOR_CACHE_TTL_MS = 10000;
    struct SensorReadCache { String value, unit; uint64_t sampledAtMs=0; uint32_t maxAgeMs=10000; };
    std::map<String, PeriphExecRule> rules;
    std::map<String, SensorReadCache> cache;
    int cacheCopies=0;
    static PeriphExecManager& getInstance() { static PeriphExecManager m; return m; }
    int getRulesMutex() { return 0; }
    auto& getRules() { return rules; }
    auto getSensorReadCacheCopy() { ++cacheCopies; return cache; }
};
namespace FastBee {
class RuntimeFeatureManager {
public:
    bool scripts=false, dashboard=true;
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager f; return f; }
    bool shouldEnable(const char* name) { return std::string(name)=="deviceControl" ? dashboard : scripts; }
};
}
class PeriphExecExecutor {
public: static String resolveChannelField(const String&,const String&,const String& report) { return report; }
};
int64_t esp_timer_get_time() { return 1000000; }
#include "control_serializer.inc"
struct AsyncWebServerRequest { int status=0; };
struct Context {
    bool authenticated=true;
    bool requireAuth(AsyncWebServerRequest* r) { if(!authenticated) r->status=401; return authenticated; }
    void sendError(AsyncWebServerRequest* r,int status,const char*) { r->status=status; }
};
struct GroupedControlItem { uint8_t group; String json; };
static std::vector<GroupedControlItem> output;
static bool pressure=false, locks=true, stream=true;
bool shouldDegradeControlsResponse() { return pressure; }
void sendDegradedControlsResponse(AsyncWebServerRequest* r) { r->status=206; }
bool sendControlsChunked(AsyncWebServerRequest* r,std::vector<GroupedControlItem>&& items) {
    if(!stream) return false;
    output=std::move(items); r->status=200; return true;
}
struct MutexGuard { MutexGuard(int,int) {} bool isLocked() { return locks; } };
#define pdMS_TO_TICKS(v) (v)
namespace HandlerUtils { void sendJsonError(AsyncWebServerRequest* r,int status,const char*,int) { r->status=status; } }
struct PeriphExecRouteHandler { Context* ctx; void handleGetControls(AsyncWebServerRequest*); };
#include "control_route.inc"
JsonDocument decode(const String& text) { JsonDocument d; assert(!deserializeJson(d,text)); return d; }

int main() {
    auto& manager=PeriphExecManager::getInstance();
    auto& features=FastBee::RuntimeFeatureManager::instance();
    Context context; PeriphExecRouteHandler route{&context}; AsyncWebServerRequest request;
    // Every action category survives, including disabled and unsupported rules.
    for(int type=0;type<=32;++type) {
        PeriphExecRule rule; rule.id=String(type); rule.name="quoted \" \n\t\\"; rule.enabled=false;
        rule.actions.push_back({static_cast<uint8_t>(type),"target",""}); manager.rules[rule.id]=rule;
    }
    PeriphExecRule empty; empty.id="empty"; manager.rules[empty.id]=empty;
    PeriphExecRule mixed; mixed.id="mixed"; mixed.name="combined"; mixed.triggers.push_back({0,1});
    mixed.actions={{0,"relay",""},{19,"soil",R"({"periphId":"soil","dataField":"temperature","sensorLabel":"Temperature"})"},
                   {18,"modbus-task:1",R"({"poll":[1,2],"ctrl":[{"d":0},{"d":2}]})"},
                   {static_cast<uint8_t>(ExecActionType::ACTION_SYS_OTA),"",""}};
    manager.rules[mixed.id]=mixed; manager.cache["soil_temperature"]={"0", "C\"\n", 0, 10000};
    route.handleGetControls(&request); assert(request.status==200 && output.size()==manager.rules.size());
    assert(manager.cacheCopies==1);
    std::set<std::string> ids;
    for(const auto& item:output) {
        auto doc=decode(item.json); const std::string id=doc["id"].as<const char*>();
        assert(ids.insert(id).second);
        if(id=="empty") { assert(doc["actionCount"]==0); assert(!doc["unavailableReason"].isNull()); }
        else if(id=="mixed") {
            assert(doc["actionCount"]==4 && doc["hasSetMode"]==true && doc["hasSystemAction"]==true);
            assert(doc["sensors"][0]["key"]=="soil_temperature");
            assert(doc["sensors"][0]["periphId"]=="soil" && doc["sensors"][0]["value"]=="0");
            assert(doc["sensors"][0]["unit"] == "C\"\n");
            assert(doc["sensors"][0]["ageMs"] == 1000 && doc["sensors"][0]["maxAgeMs"] == 10000);
            assert(doc["pollTaskIndex"].size()==2 && doc["pollTaskIndex"][1]==2);
            bool found=false; for(JsonObject t:doc["targets"].as<JsonArray>()) if(t["id"]=="modbus:2") found=true;
            assert(found);
#if FASTBEE_ENABLE_OTA
            assert(doc["unavailableReason"].isNull());
#else
            assert(!doc["unavailableReason"].isNull());
#endif
        } else {
            assert(doc["enabled"]==false); assert(doc["name"].as<String>()==manager.rules[String(id)].name);
            if(id=="15") assert(!doc["unavailableReason"].isNull());
        }
    }
    // No sensor cache copy for non-sensor rules; invalid mapping JSON remains a runnable rule.
    manager.rules.clear(); mixed.actions={{18,"modbus:0","invalid-json"}}; manager.rules[mixed.id]=mixed;
    manager.cacheCopies=0; route.handleGetControls(&request); assert(request.status==200 && output.size()==1 && manager.cacheCopies==0);
    assert(decode(output[0].json)["pollTaskIndex"].size()==0);
    // Oversized or memory-degraded responses cannot silently discard a rule.
    manager.rules.begin()->second.name=String(std::string(CONTROL_ITEM_MAX_BYTES+1,'x'));
    route.handleGetControls(&request); assert(request.status==206);
    pressure=true; route.handleGetControls(&request); assert(request.status==206); pressure=false;
    features.dashboard=false; route.handleGetControls(&request); assert(request.status==503); features.dashboard=true;
    context.authenticated=false; route.handleGetControls(&request); assert(request.status==401); context.authenticated=true;
    manager.rules.clear(); locks=false; route.handleGetControls(&request); assert(request.status==206); locks=true;
    stream=false; route.handleGetControls(&request); assert(request.status==503);
}
