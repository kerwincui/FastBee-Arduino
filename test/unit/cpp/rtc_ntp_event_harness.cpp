// Production static event catalog, scheduler route and manager matcher. Queue,
// MQTT and mutex are explicit seams; no action or physical write is executed.
#include <Arduino.h>
#include "network/LiteralStringMatch.h"
#include <ArduinoJson.h>
#include "core/PeriphExecConfigValidation.h"
#include "core/UartEventOrigin.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
inline void convertFromJson(JsonVariantConst value,String& out) { out=value.as<const char*>(); }
static unsigned lockDepth=0;
struct MutexGuard { explicit MutexGuard(int) { ++lockDepth; } ~MutexGuard() { --lockDepth; } bool isLocked() const { return true; } };
struct Logger { template<class... T> void infof(T...) {} template<class... T> void warningf(T...) {} } LOGGER;
struct PeripheralManager {
    static PeripheralManager& getInstance() { static PeripheralManager instance;return instance; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const { return origin.unbound(); }
};
struct PeriphExecManager {
    bool initialized=true;
    int _rulesMutex=1;
    unsigned queued=0,published=0;
    std::map<String,PeriphExecRule> rules;
    bool isInitialized() const { return initialized; }
    void dispatchEventMatchedRules(const String&,const String&,const FastBee::UartEventOrigin&);
    void dispatchAndCommitTrigger(const String& id,size_t index,const String& data,
        const char* originKind,const FastBee::UartEventOrigin& origin,uint64_t revision) {
        assert(lockDepth==0 && origin.unbound() && std::strcmp(originKind,"event")==0);
        const auto& rule=rules.at(id.c_str());
        assert(rule.enabled && index==0 && revision==rule.runtimeRevision && data=="network-time");
        assert(rule.actions.size()==1);
        const auto& action=rule.actions[0];
        assert(action.actionType==static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET));
        assert(action.targetPeriphId=="rtc_demo" && action.actionValue=="system-local");
        assert(!action.useReceivedValue && !action.syncDelayMs);
        ++queued;
    }
    void notifyMqttEventPublish(const String&,const String&,const String&,
        const FastBee::UartEventOrigin&) { ++published; }
};
struct PeriphExecScheduler {
    PeriphExecManager* _manager;
    void triggerEvent(EventType,const String&,const FastBee::UartEventOrigin&,bool = true);
};
static unsigned long eventMillis() { return 10000; }
#define millis eventMillis
#include "rtc_event_under_test.inc"
#undef millis
int main(int argc,char** argv) {
    assert(argc==3);
    const std::string mode=argv[1];
    std::ifstream input(argv[2]);assert(input);
    const std::string json{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    JsonDocument doc;assert(!deserializeJson(doc,json));
    assert(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    JsonObjectConst saved=doc["rules"][0];
    assert(saved["enabled"].is<bool>() && !saved["enabled"].as<bool>());
    assert(!saved["reportAfterExec"].as<bool>() && !saved["recordHistory"].as<bool>());
    assert(!saved["allowPanelControl"].as<bool>());
    assert(saved["triggers"].size()==1 && saved["actions"].size()==1);
    PeriphExecRule rule;rule.id=saved["id"].as<const char*>();rule.name=saved["name"].as<const char*>();
    rule.enabled=saved["enabled"].as<bool>();rule.runtimeRevision=19;
    ExecTrigger trigger;trigger.triggerType=saved["triggers"][0]["triggerType"].as<uint8_t>();
    trigger.eventId=saved["triggers"][0]["eventId"].as<const char*>();
    assert(trigger.triggerType==static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER));
    const auto* event=findStaticEvent(trigger.eventId.c_str());assert(event);
    assert(event->type==EventType::EVENT_NTP_SYNCED);
    rule.triggers.push_back(trigger);
    ExecAction action;JsonObjectConst savedAction=saved["actions"][0];
    action.actionType=savedAction["actionType"].as<uint8_t>();
    action.targetPeriphId=savedAction["targetPeriphId"].as<const char*>();
    action.actionValue=savedAction["actionValue"].as<const char*>();
    action.useReceivedValue=savedAction["useReceivedValue"].as<bool>();
    action.syncDelayMs=savedAction["syncDelayMs"].as<uint16_t>();
    rule.actions.push_back(action);
    assert(!FastBee::PeriphExecConfig::validateRule(rule));
    if(mode!="disabled") rule.enabled=true; // Fixture only; the file remains disabled.
    if(mode=="cooldown") rule.triggers[0].lastTriggerTime=9500;
    PeriphExecManager manager;manager.rules[rule.id.c_str()]=rule;
    if(mode=="uninitialized") manager.initialized=false;
    PeriphExecScheduler scheduler{&manager};
    scheduler.triggerEvent(mode=="wrong_event"?EventType::EVENT_SYSTEM_BOOT:EventType::EVENT_NTP_SYNCED,
                           "network-time",{});
    assert(manager.queued==(mode=="matched"?1U:0U));
    assert(manager.published==(mode=="uninitialized"?0U:1U));
    assert(lockDepth==0);
}
