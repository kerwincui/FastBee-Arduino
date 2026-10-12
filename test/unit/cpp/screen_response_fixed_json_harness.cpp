// Real production response bodies and snapshot/capability types, with SDK seams.
// The independent oracle is frozen before the 119 fixed-field replacements.
#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "serializer_trace_allocator.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
inline void convertToJson(const String& text, JsonVariant value) { value.set(std::string(text)); }
inline void convertFromJson(JsonVariantConst value, String& text) { text = value.as<std::string>(); }
#include "screen_response_types.inc"
static std::vector<std::string> sideEffects;
static unsigned scenario = 0;
static time_t deviceEpoch = 0;
static bool localAvailable = true;
time_t fixtureTime(time_t*) { sideEffects.emplace_back("time"); return deviceEpoch; }
tm* fixtureLocalTime(const time_t*, tm* value) {
    sideEffects.emplace_back("localtime");
    if (!localAvailable) return nullptr;
    *value = {}; value->tm_year=131; value->tm_mon=3; value->tm_mday=5;
    value->tm_hour=2; value->tm_min=3; return value;
}
namespace FastBee { namespace Screen {
class ScreenRuntime {
public:
    ScreenSystemActionCapabilities capabilities{};
    String error;
    static ScreenRuntime& instance() { static ScreenRuntime value; return value; }
    ScreenSystemActionCapabilities systemActionCapabilities() {
        sideEffects.emplace_back("capabilities"); return capabilities;
    }
    bool running() { sideEffects.emplace_back("running"); return (scenario & 1) != 0; }
    bool sleeping() { sideEffects.emplace_back("sleeping"); return (scenario & 2) != 0; }
    size_t allocatedBytes() { sideEffects.emplace_back("allocatedBytes"); return scenario ? SIZE_MAX : 0; }
    void getLastError(String& output) { sideEffects.emplace_back("runtimeError"); output=error; }
};
}}
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_SPIRAM=4;
size_t heap_caps_get_free_size(int caps) {
    sideEffects.emplace_back("free:" + std::to_string(caps)); return scenario ? UINT32_MAX : 0;
}
size_t heap_caps_get_largest_free_block(int caps) {
    sideEffects.emplace_back("largest:" + std::to_string(caps)); return scenario ? 1024 : 0;
}
struct PanelLock {
    explicit PanelLock(void*) { sideEffects.emplace_back("lock"); }
    bool locked() const { return scenario != 8; }
    ~PanelLock() { sideEffects.emplace_back("unlock"); }
};
static ScreenPanelSnapshot snapshotTemplate;
struct PanelFields {
    void* _mutex = nullptr;
    bool _persisted=false, _applied=false, _restartRequired=false;
    uint32_t _revision=0;
    String _applicationError;
    bool getSnapshot(ScreenPanelSnapshot& output) const {
        sideEffects.emplace_back("snapshot");
        if (scenario == 8) return false;
        output=snapshotTemplate; return true;
    }
};
struct ScreenPanelManager : PanelFields {
    void appendCapabilities(JsonObject) const;
    void appendApplication(JsonObject) const;
    void appendState(JsonObject) const;
};
struct OracleScreenPanelManager : PanelFields {
    void appendCapabilities(JsonObject) const;
    void appendApplication(JsonObject) const;
    void appendState(JsonObject) const;
};
#include "screen_quality.inc"
#define time fixtureTime
#define localtime_r fixtureLocalTime
#define localtime_s(output, input) (fixtureLocalTime(input, output) ? 0 : 1)
#include "screen_response_actual.inc"
#include "screen_response_original.inc"
#undef time
#undef localtime_r
#undef localtime_s

template<size_t N>
void text(char (&destination)[N], const std::string& value) {
    std::snprintf(destination, N, "%s", value.c_str());
}
static void prepare(unsigned value) {
    scenario=value; deviceEpoch=value & 1 ? 1790000000 : 0; localAvailable=value != 3;
    auto& runtime=FastBee::Screen::ScreenRuntime::instance();
    runtime.error=value & 4 ? String(std::string(140, 'e')) : String();
    auto& c=runtime.capabilities; c={};
    c.displayReady=(value & 1)!=0; c.touchConfigured=(value & 2)!=0;
    c.touchReady=(value & 4)!=0; c.brightness=(value & 8)!=0;
    c.sleep=(value & 1)!=0; c.buzzer=(value & 2)!=0; c.restart=(value & 4)!=0;
    c.brightnessContinuous=(value & 1)!=0; c.touchWake=(value & 8)!=0;
    c.minVisibleBrightnessPercent=value ? 100 : 0;
    if (value & 1) text(c.touchError, "touch \"error\" 中文");
    snapshotTemplate={};
    auto& s=snapshotTemplate;
    s.cardCount=value==15 ? ScreenPanelSnapshot::MAX_CARDS : value % 5;
    s.revision=value ? UINT32_MAX : 0; s.activePageIndex=value ? 7 : 0;
    s.brightnessPercent=value ? 100 : 0; s.idleSleepSeconds=value ? UINT32_MAX : 0;
    s.controlConfirmIdleSeconds=value ? 600 : 0; s.touchBeepEnabled=(value & 1)!=0;
    for (unsigned i=0;i<s.cardCount;++i) {
        auto& card=s.cards[i];
        text(card.id,"card-"+std::to_string(i));
        text(card.value,"value-"+std::to_string(i)+"-\"\\ 中文");
        if (i % 4 != 3) text(card.sourceRef,i % 2 ? "source.shared" : "sensor."+std::to_string(i));
        if (i % 3 == 0) text(card.controlRef,"control."+std::to_string(i));
        text(card.unit,"°C"); text(card.error,"sample error"); text(card.historyError,"history error");
        text(card.systemActionId,"brightness"); text(card.motorReadError,"motor error");
        card.kind=i % 2 ? ScreenCardKind::WEATHER : ScreenCardKind::DATA;
        card.visualType=i % 2 ? ScreenVisualType::MOTOR_CONTROL : ScreenVisualType::BAR;
        card.quality=static_cast<ScreenValueQuality>(i % 6);
        card.updatedMs=UINT32_MAX-i; card.operationId=i+1; card.pending=i%2;
        card.motorSpeed=UINT16_MAX-i; card.motorPulse=i; card.motorPrecision=i;
        card.motorKnown=true; card.motorCommandKnown=true; card.motorCommandSuccess=i%2;
        card.motorCommandUpdatedMs=UINT32_MAX-i; card.motorSoftLimitEnabled=i%2;
        card.motorReferenceKnown=true; card.motorStationaryConfirmed=i%2;
        card.motorForwardReady=true; card.motorReverseReady=i%2;
        card.historyCount=10; card.historyStale=i%2;
        if (i%2) {
            text(card.weatherCondition,"clear"); text(card.weatherCity,"Shanghai");
            text(card.weatherReason,"no recent observation"); card.hasWeatherTemperature=true;
            card.weatherTemperature=value==15 ? NAN : -12.75f;
        }
        card.seriesCount=i%2 ? 4 : 0;
        for (unsigned j=0;j<card.seriesCount;++j) {
            auto& series=card.series[j];
            text(series.sourceRef,"series."+std::to_string(i)+"."+std::to_string(j));
            text(series.label,"Label \" 中文"); text(series.unit,"°C"); text(series.value,"12.50");
            series.quality=static_cast<ScreenValueQuality>(j);
            series.updatedMs=UINT32_MAX-j;
        }
    }
}
static void ownership(JsonVariantConst value, std::vector<std::string>& rows) {
    if (value.is<JsonObjectConst>()) {
        rows.emplace_back("object");
        for (JsonPairConst pair : value.as<JsonObjectConst>()) {
            rows.push_back("key:"+std::string(pair.key().c_str())+":"+std::to_string(pair.key().isStatic()));
            ownership(pair.value(), rows);
        }
    } else if (value.is<JsonArrayConst>()) {
        rows.emplace_back("array");
        for (JsonVariantConst item : value.as<JsonArrayConst>()) ownership(item,rows);
    } else if (value.is<const char*>()) {
        auto string=value.as<JsonString>();
        rows.push_back("text:"+std::to_string(string.size())+":"+std::to_string(string.isStatic()));
    } else {
        rows.push_back("scalar:"+std::to_string(value.isNull())+":"+std::to_string(value.is<bool>())+
            ":"+std::to_string(value.is<int64_t>())+":"+std::to_string(value.is<uint64_t>())+
            ":"+std::to_string(value.is<double>()));
    }
}
struct Result {
    std::string json;
    bool overflowed=false;
    unsigned calls=0;
    JsonAllocationTrace allocations;
    std::vector<std::string> types,effects;
    bool operator==(const Result& b) const {
        return json==b.json && overflowed==b.overflowed && calls==b.calls &&
            allocations==b.allocations && types==b.types && effects==b.effects;
    }
};
template<class Manager>
void response(Manager& manager, const std::string& mode, JsonObject out);
template<>
void response(ScreenPanelManager& manager, const std::string& mode, JsonObject out) {
    if (mode=="clock") appendPanelClock(out);
    else if (mode=="capabilities") manager.appendCapabilities(out);
    else if (mode=="application") manager.appendApplication(out);
    else manager.appendState(out);
}
template<>
void response(OracleScreenPanelManager& manager, const std::string& mode, JsonObject out) {
    if (mode=="clock") appendPanelClockOracle(out);
    else if (mode=="capabilities") manager.appendCapabilities(out);
    else if (mode=="application") manager.appendApplication(out);
    else manager.appendState(out);
}
template<class Manager>
Result run(const std::string& mode,unsigned value,unsigned budget,bool unbound) {
    prepare(value);
    SerializerTraceAllocator allocator(UINT_MAX);
    Result result;
    {
        JsonDocument doc(&allocator);
        JsonObject out=doc.to<JsonObject>();
        if (value & 1) {
            out["kept"]=std::string("existing owned field");
            out["available"]=false; out["revision"]=17; out["quality"]="old";
            out["runtime"]["old"]="retained"; out["items"]["old"]["value"]=17;
        }
        assert(!doc.overflowed());
        allocator.calls=0; allocator.limit=budget; allocator.trace.clear();
        sideEffects.clear();
        Manager manager;
        manager._persisted=(value&1)!=0; manager._applied=(value&2)!=0;
        manager._restartRequired=(value&4)!=0; manager._revision=value ? UINT32_MAX : 0;
        manager._applicationError=value&8 ? String(std::string(180,'f')) : String();
        response(manager,mode,unbound ? JsonObject() : out);
        result.overflowed=doc.overflowed();
        serializeJson(doc,result.json); ownership(doc.as<JsonVariantConst>(),result.types);
        // Snapshot temporaries have already been poisoned/destroyed. Subsequent
        // mutable source changes must not alter any copied JSON text.
        manager._applicationError="changed";
        auto& runtime=FastBee::Screen::ScreenRuntime::instance();runtime.error="changed";
        text(runtime.capabilities.touchError,"changed");
        std::string after;serializeJson(doc,after);assert(after==result.json);
        result.effects=sideEffects;
    }
    assert(allocator.live.empty());
    result.calls=allocator.calls;result.allocations=allocator.trace;return result;
}
static void check(const std::string& mode,unsigned value,unsigned budget,bool unbound) {
    auto actual=run<ScreenPanelManager>(mode,value,budget,unbound);
    auto original=run<OracleScreenPanelManager>(mode,value,budget,unbound);
    if (!(actual==original)) {
        std::fprintf(stderr,"mismatch mode=%s scenario=%u budget=%u unbound=%d\n",
                     mode.c_str(),value,budget,unbound);
        std::abort();
    }
}
int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];
    assert(mode=="clock" || mode=="capabilities" || mode=="application" || mode=="state");
    unsigned oomCases=0;
    for (unsigned value=0;value<16;++value) {
        auto successful=run<ScreenPanelManager>(mode,value,UINT_MAX,false);
        assert(!successful.overflowed);
        check(mode,value,UINT_MAX,false);check(mode,value,0,true);check(mode,value,UINT_MAX,true);
        for (unsigned budget=0;budget<=successful.calls;++budget) {
            check(mode,value,budget,false);++oomCases;
        }
    }
    std::printf("PASS %s scenarios=16 allocation-budgets=%u\n",mode.c_str(),oomCases);
}
