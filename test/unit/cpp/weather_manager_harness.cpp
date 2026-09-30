#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <ctime>
#include <vector>
#include "protocols/RemoteResourceModel.h"
#define private public
#include "protocols/RemoteResourceManager.h"
#undef private
inline void convertFromJson(JsonVariantConst value, String& output) { output=value.as<std::string>(); }
using namespace FastBee;
using namespace FastBee::WeatherSchedule;
static bool tlsPsram=false;
namespace FastBee { namespace TlsMemory { inline bool ensureAllocator() { return true; } inline bool usesPsram() { return tlsPsram; } }}
static uint32_t clockMs=1000;
static time_t clockUtc=0;
#define millis() clockMs
#define time(pointer) clockUtc
enum class NetworkType { NET_WIFI, NET_4G };
struct FBNetworkManager {
    bool connected=true;
    NetworkType type=NetworkType::NET_WIFI;
    bool isNetworkConnected() const { return connected; }
    NetworkType getNetworkType() const { return type; }
};
struct FastBeeFramework {
    FBNetworkManager network;
    static FastBeeFramework* getInstance() { static FastBeeFramework f; return &f; }
    FBNetworkManager* getNetworkManager() { return &network; }
};
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
static size_t freeMemory=100000, largestBlock=60000;
static size_t heap_caps_get_free_size(int) { return freeMemory; }
static size_t heap_caps_get_largest_free_block(int) { return largestBlock; }
static bool ota=false, taskSuccess=true;
static bool otaBusy() { return ota; }
static unsigned tasks=0, fetches=0;
static void (*queued)(void*)=nullptr;
static void* queuedArg=nullptr;
static int xTaskCreate(void (*fn)(void*),const char*,size_t bytes,void* arg,int,void*) {
    assert(bytes==8192);
    if (!taskSuccess) return pdFAIL;
    ++tasks; queued=fn; queuedArg=arg; return pdPASS;
}
static void vTaskDelete(void*) {}
static unsigned uxTaskGetStackHighWaterMark(void* task) { assert(!task); return 2048; }
static Reason fetchReason=Reason::None;
static bool checkWorkerAdmission=false;
static RemoteResourceManager* mutateManager=nullptr;
static bool fetchWeather(JsonObjectConst source,JsonDocument& output,Reason& reason,Diagnostics& memory) {
    memory.phase="request_completed"; memory.workerFree=90000; memory.workerLargest=52000;
    memory.requestFree=85000; memory.requestLargest=48000;
    ++fetches; assert(source["provider"]=="seniverse"); reason=fetchReason;
    if (checkWorkerAdmission) {
        memory.workerFree=freeMemory; memory.workerLargest=largestBlock;
        memory.requiredFree=requiredFree(false,tlsPsram);
        memory.requiredLargest=requiredLargest(false,tlsPsram);
        reason=admission(true,true,false,freeMemory,largestBlock,false,false,tlsPsram);
        if (reason!=Reason::None) memory.phase="worker_blocked";
    }
    if (mutateManager) { ++mutateManager->generation_; mutateManager=nullptr; }
    if (reason!=Reason::None) return false;
    output["temperature"]=22; output["unit"]="°C"; output["city"]="上海"; output["condition"]="sunny";
    return true;
}
#include "manager.inc"
static void runWorker() {
    assert(queued); auto fn=queued; auto arg=queuedArg; queued=nullptr; fn(arg);
}
static JsonDocument status(RemoteResourceManager& m) {
    JsonDocument out; assert(m.getWeatherTestStatus("local",out.to<JsonObject>())); return out;
}
int main() {
    RemoteResourceManager m;
    m.mutex_=xSemaphoreCreateRecursiveMutex();
    JsonDocument config;
    config["remoteChannels"].to<JsonArray>();
    auto w=config["weatherSources"].to<JsonArray>().add<JsonObject>();
    RemoteResourceModel::fixedWeather(w,JsonObjectConst());
    w["enabled"]=true; w["apiKey"]="not-for-output";
    m.applyConfig(config.as<JsonVariantConst>());
    JsonDocument value;
    assert(m.getValue("weather.local",value.to<JsonObject>()));
    assert(value["quality"]=="unknown" && value["unit"].isNull());
    assert(value["weather"]["temperature"].isNull());
    // Waiting for NTP never starts a worker nor spends the 30-minute interval.
    m.tick(); assert(tasks==0 && fetches==0);
    auto out=status(m); assert(out["reason"]=="time_unsynced");
    assert(out["memory"]["lastAdmissionPhase"]=="tick_blocked");
    assert(out["memory"]["tickFree"]==100000U && out["memory"]["requiredFree"]==73192U);
    assert(out["automaticRetryAfterMs"].isNull());
    clockMs=1234; m.tick(); assert(tasks==0);
    clockUtc=1700000000; m.tick(); assert(tasks==1);
    String inFlightError; assert(!m.requestRead("local",inFlightError));
    assert(status(m)["pending"]==true);
    runWorker();
    out=status(m); assert(out["status"]=="ready" && out["automaticRetryAfterMs"]==1800000U);
    assert(m.getValue("weather.local",value.to<JsonObject>()) && value["unit"]=="°C");
    assert(value["weather"]["temperature"]==22);
    assert(m.getValue("weather.local.temperature",value.to<JsonObject>()) && value["unit"]=="°C");
    assert(value["value"]==22);
    assert(out["manualRetryAfterMs"]==300000U && out["preview"]["temperature"]==22);
    assert(out["memory"]["lastAdmissionPhase"]=="request_completed");
    assert(out["memory"]["workerLargest"]==52000U && out["memory"]["requestLargest"]==48000U);
    assert(out["memory"]["releasedLargest"]==60000U);
    assert(out["memory"]["workerStackHighWaterBytes"]==2048U);
    clockMs+=1000; out=status(m); assert(out["automaticRetryAfterMs"]==1799000U);
    assert(out["manualRetryAfterMs"]==299000U);
    String error; assert(!m.requestRead("local",error));
    // Repeated unchanged saves preserve cached data and manual cooldown.
    m.applyConfig(config.as<JsonVariantConst>()); assert(m.weather_[0].known);
    assert(!m.requestRead("local",error));
    // Offline/OTA/low and fragmented memory block before worker allocation.
    clockMs+=1800000; auto* network=FastBeeFramework::getInstance()->getNetworkManager();
    network->connected=false; m.tick(); assert(tasks==1);
    out=status(m); assert(out["reason"]=="network_offline");
    network->connected=true; ota=true; m.tick(); assert(tasks==1);
    ota=false; freeMemory=73000; m.tick(); assert(tasks==1);
    freeMemory=100000; largestBlock=32000; m.tick(); assert(tasks==1);
    assert(status(m)["reason"]=="memory_unavailable");
    largestBlock=60000; m.tick(); assert(tasks==2); runWorker();
    // Transport error retains the valid cache; automatic 30s != manual cooldown.
    clockMs+=300000; assert(m.requestRead("local",error));
    fetchReason=Reason::Transport; m.tick(); runWorker();
    out=status(m); assert(out["automaticRetryAfterMs"]==30000U);
    assert(out["manualRetryAfterMs"]==300000U && out["preview"]["temperature"]==22);
    clockMs+=1234; out=status(m); assert(out["automaticRetryAfterMs"]==28766U);
    assert(out["manualRetryAfterMs"]==298766U);
    assert(!m.requestRead("local",error));
    // A preflight race NTP failure recovers immediately, not after normal interval.
    clockMs+=28766; fetchReason=Reason::Time; m.tick(); runWorker();
    assert(status(m)["blocked"]==true);
    fetchReason=Reason::None; m.tick(); runWorker(); assert(status(m)["status"]=="ready");
    // Provider errors use the configured interval; they are not transport retries.
    clockMs+=1800000; fetchReason=Reason::Authentication; m.tick(); runWorker();
    out=status(m); assert(out["reason"]=="authentication");
    assert(out["automaticRetryAfterMs"]==1800000U);
    // Stale in-flight results are rejected by the production generation fence.
    const uint32_t prior=m.weather_[0].updatedMs;
    clockMs+=1800000; fetchReason=Reason::None; mutateManager=&m;
    m.tick(); runWorker(); assert(m.weather_[0].updatedMs==prior);
    taskSuccess=false; m.tick(); assert(status(m)["reason"]=="worker_unavailable");
    auto count=tasks; m.tick(); assert(tasks==count);
    clockMs+=30000; taskSuccess=true; m.tick(); runWorker();
    // Stack allocation can reduce the largest block below TLS admission. That
    // race must not recreate a worker on every tick with unchanged memory.
    clockMs+=1800000; fetchReason=Reason::Memory; m.tick(); runWorker();
    count=tasks; m.tick(); m.tick(); assert(tasks==count);
    clockMs+=30000; fetchReason=Reason::None; m.tick(); runWorker();
    // Timer wrap and uptime zero are valid samples, not missing timestamps.
    assert(manualRemaining(0,true,0,false,0)==300000U);
    assert(manualRemaining(15,true,0xfffffff0U,false,0)==300000U-31U);
    assert(remaining(0xfffffff0U,20U)==36U && due(20U,20U));
    for (Reason r : {Reason::Time,Reason::Memory,Reason::Offline,Reason::Transport}) {
        assert(failureDelay(1,r,1800000)==30000U);
        assert(failureDelay(6,r,1800000)==300000U);
    }
    // Provider codes are classified even when returned in a non-200 JSON body.
    JsonDocument body; body["status_code"]="AP010010";
    assert(RemoteResourceModel::providerFailure(body.as<JsonVariantConst>())==Reason::Location);
    body["status_code"]="AP010014";
    assert(RemoteResourceModel::providerFailure(body.as<JsonVariantConst>())==Reason::RateLimit);
    body["status_code"]="AP010003";
    assert(RemoteResourceModel::providerFailure(body.as<JsonVariantConst>())==Reason::Authentication);
    std::string json; serializeJson(status(m),json);
    assert(json.find("not-for-output")==std::string::npos && json.find("apiKey")==std::string::npos);
    // Production tick must admit the captured pre-stack heap, then retain the
    // worker's admission thresholds and stack headroom in its public status.
    RemoteResourceManager psramManager;
    psramManager.mutex_=xSemaphoreCreateRecursiveMutex();
    psramManager.applyConfig(config.as<JsonVariantConst>());
    tlsPsram=true; checkWorkerAdmission=true;
    freeMemory=39364; largestBlock=16383; count=tasks;
    psramManager.tick(); assert(tasks==count);
    out=status(psramManager);
    assert(out["memory"]["requiredFree"]==32192U && out["memory"]["requiredLargest"]==16384U);
    largestBlock=17396; freeMemory=32191;
    psramManager.tick(); assert(tasks==count);
    freeMemory=39364; psramManager.tick(); assert(tasks==count+1);
    freeMemory=30152; largestBlock=9204; runWorker();
    out=status(psramManager); assert(out["status"]=="ready" && out["tlsUsesPsram"]==true);
    assert(out["memory"]["tickLargest"]==17396U && out["memory"]["workerLargest"]==9204U);
    assert(out["memory"]["requiredLargest"]==8192U && out["memory"]["workerStackHighWaterBytes"]==2048U);
    clockMs+=1800000; freeMemory=32192; largestBlock=16384;
    psramManager.tick(); assert(tasks==count+2);
    freeMemory=24000; largestBlock=8191; runWorker();
    assert(status(psramManager)["reason"]=="memory_unavailable");
    assert(status(psramManager)["memory"]["lastAdmissionPhase"]=="worker_blocked");
    clockMs+=30000; freeMemory=39364; largestBlock=17396;
    psramManager.tick(); freeMemory=23999; largestBlock=9204; runWorker();
    assert(status(psramManager)["reason"]=="memory_unavailable");
    clockMs+=60000; tlsPsram=false; freeMemory=39364; largestBlock=17396; count=tasks;
    psramManager.tick(); assert(tasks==count);
    out=status(psramManager);
    assert(out["memory"]["requiredFree"]==73192U && out["memory"]["requiredLargest"]==32768U);
}
