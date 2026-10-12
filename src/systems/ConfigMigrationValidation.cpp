#include "systems/ConfigMigrationValidation.h"
#include "systems/ConfigMigrationModbus.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_CONFIG_TRANSFER
#include "core/PeripheralManager.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"
#include "core/PeripheralActionContract.h"
#include "core/ThermostatConfig.h"
#include "core/RuntimeFeatureManager.h"
#include "core/RuleScriptConfigValidation.h"
#include "core/RuleScriptResources.h"
#include "core/SensorConfigSupport.h"
#include "core/DriverRegistry.h"
#include "core/ScriptEngine.h"
#include "core/MotorControlService.h"
#include "network/DeviceConfigValidation.h"
#include "utils/CheckedText.h"
#if FASTBEE_ENABLE_RULE_SCRIPT
#include "core/RuleScript.h"
#endif
#if FASTBEE_ENABLE_SCREEN_PANEL
#include "core/ScreenPanelManager.h"
#endif
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <memory>
#include <new>

namespace FastBee { namespace ConfigMigration { namespace {
bool fail(String& error,const char* code){error=code;return false;}
size_t indexOf(const Manifest& manifest,uint32_t id){
    for(size_t i=0;i<manifest.count;++i)if(manifest.entries[i].fileId==id)return i;
    return MaxFiles;
}
String effectivePath(const Manifest& manifest,uint32_t id){
    const size_t i=indexOf(manifest,id);if(i==MaxFiles)return {};
    const auto& row=manifest.entries[i];
    if(row.candidate.exists)return Storage::slotPath(i,Slot::Candidate);
    if(row.original.exists)return Storage::slotPath(i,Slot::Original);
    if(row.backup.exists)return Storage::slotPath(i,Slot::Backup);
    return {};
}
bool selected(const Manifest& manifest,uint32_t id){const size_t i=indexOf(manifest,id);return i<MaxFiles&&manifest.entries[i].candidate.exists;}
bool readJson(const String& path,JsonDocument& doc,size_t maxBytes,String& error){
    doc.clear();if(path.isEmpty())return true;
    File input=LittleFS.open(path,FILE_READ);
    if(!input||!input.size()||input.size()>maxBytes)return fail(error,"MIGRATION_CONFIG_SIZE");
    // Retain headroom for AsyncTCP and the runtime. JSON memory is separate
    // from the raw staged bytes; lack of space is an explicit rejection.
    if(heap_caps_get_free_size(MALLOC_CAP_8BIT)<input.size()*4+32768)
        return fail(error,"MIGRATION_MEMORY_PRESSURE");
    if(deserializeJson(doc,input)||doc.overflowed())return fail(error,"MIGRATION_CONFIG_JSON");
    return true;
}
bool text(JsonVariantConst value,size_t max,bool empty=false){
    if(!value.is<JsonString>())return false;
    const auto s=value.as<JsonString>();return (empty||s.size())&&s.size()<=max&&strlen(s.c_str())==s.size();
}
struct Source {String ref,unit;bool numeric=true;};
struct Graph {
    std::vector<PeripheralConfig> peripherals;
    std::vector<String> ruleIds;
    std::vector<String> protocolEvents;
    std::vector<String> protocolTriggers;
    ModbusReviewGraph modbus;
    std::vector<Source> sources;
    JsonDocument panel;
    JsonDocument features;
    JsonDocument remote;
    const PeripheralConfig* target(const String& id)const{
        for(const auto& p:peripherals)if(p.id==id)return &p;return nullptr;
    }
    bool feature(const char* key)const{
        auto& manager=RuntimeFeatureManager::instance();
        for(size_t i=0;i<manager.featureCount();++i){const auto* item=manager.getFeatureAt(i);
            if(!strcmp(item->key,key))return item->compiled&&
                (features["features"][key]["enabled"].is<bool>()?
                 features["features"][key]["enabled"].as<bool>():item->defaultEnabled);
        }return false;
    }
    bool addSource(const String& ref,const String& unit,bool numeric,String& error){
        if(ref.isEmpty()||ref.length()>96||unit.length()>24)return fail(error,"MIGRATION_SOURCE_INVALID");
        for(const auto& source:sources)if(source.ref==ref)
            return (source.unit==unit&&source.numeric==numeric)||fail(error,"MIGRATION_SOURCE_COLLISION");
        Source source;
        if(!copyCheckedText(source.ref,ref.c_str(),ref.length(),96)||
           !copyCheckedText(source.unit,unit.c_str(),unit.length(),24))return fail(error,"MIGRATION_MEMORY_PRESSURE");
        source.numeric=numeric;sources.push_back(std::move(source));return true;
    }
    const Source* source(const String& ref)const{for(const auto& s:sources)if(s.ref==ref)return &s;return nullptr;}
    bool triggerSource(const String& ref)const {
        if(source(ref))return true;
        for(const auto& id:protocolTriggers)if(id==ref)return true;
        return false;
    }
};
bool validateFeatures(Graph& graph,String& error){
    const auto root=graph.features.as<JsonObjectConst>();
    if(root.isNull())return true; // Absent file uses registered defaults.
    if(root["version"]!=1||!root["features"].is<JsonObjectConst>())return fail(error,"MIGRATION_FEATURE_SCHEMA");
    auto& manager=RuntimeFeatureManager::instance();
    for(JsonPairConst row:root["features"].as<JsonObjectConst>()){
        if(row.key().size()!=strlen(row.key().c_str())||!row.value().is<JsonObjectConst>()||
           row.value().size()!=1||!row.value()["enabled"].is<bool>())return fail(error,"MIGRATION_FEATURE_SCHEMA");
        const RuntimeFeatureManager::FeatureMeta* feature=nullptr;
        for(size_t i=0;i<manager.featureCount();++i)if(!strcmp(manager.getFeatureAt(i)->key,row.key().c_str()))feature=manager.getFeatureAt(i);
        if(!feature)return fail(error,"MIGRATION_FEATURE_UNKNOWN");
        if(row.value()["enabled"].as<bool>()&&(!feature->compiled||
           (feature->dependsOn&&*feature->dependsOn&&!graph.feature(feature->dependsOn))))
            return fail(error,"MIGRATION_FEATURE_UNAVAILABLE");
    }
    return true;
}
bool validateDevice(const Manifest& manifest,String& error){
    if(!selected(manifest,0))return true;
    JsonDocument candidate,original;
    if(!readJson(effectivePath(manifest,0),candidate,24576,error))return false;
    const size_t index=indexOf(manifest,0);
    const auto& row=manifest.entries[index];
    const String baseline=row.original.exists?Storage::slotPath(index,Slot::Original):
        row.backup.exists?Storage::slotPath(index,Slot::Backup):String();
    if(baseline.isEmpty()||!readJson(baseline,original,24576,error)||!original.is<JsonObjectConst>()||!candidate.is<JsonObjectConst>())
        return fail(error,"MIGRATION_DEVICE_BASELINE_REQUIRED");
    // A migration can change presentation/time settings. Identity, product,
    // account binding, developer mode and any unknown future keys stay exact.
    const auto allowed=[](const char* key){
        for(const char* name:{"deviceName","location","description","enableNTP","ntpServer1","ntpServer2","timezone","syncInterval","logLevel","cacheDuration"})
            if(!strcmp(key,name))return true;
        return false;
    };
    for(JsonPairConst entry:original.as<JsonObjectConst>())
        if(!allowed(entry.key().c_str())&&candidate[entry.key()]!=entry.value())return fail(error,"MIGRATION_IDENTITY_MUST_BE_PRESERVED");
    JsonDocument patch;
    for(JsonPairConst entry:candidate.as<JsonObjectConst>()){
        if(entry.key().size()!=strlen(entry.key().c_str()))return fail(error,"MIGRATION_DEVICE_SCHEMA");
        if(!allowed(entry.key().c_str())){
            if(!original.as<JsonObjectConst>().containsKey(entry.key().c_str())||original[entry.key()]!=entry.value())
                return fail(error,"MIGRATION_IDENTITY_MUST_BE_PRESERVED");
        }else if(!strcmp(entry.key().c_str(),"location")){
            if(!text(entry.value(),128,true))return fail(error,"MIGRATION_DEVICE_SCHEMA");
        }else patch[entry.key()]=entry.value();
    }
    if(patch.overflowed())return fail(error,"MIGRATION_MEMORY_PRESSURE");
    if(patch.size()&&validateDeviceConfigPatch(patch.as<JsonObjectConst>()))return fail(error,"MIGRATION_DEVICE_SCHEMA");
    return true;
}
bool collectRule(const PeriphExecRule& rule,void* context,String& error){
    auto& graph=*static_cast<Graph*>(context);String id;
    if(!copyCheckedText(id,rule.id.c_str(),rule.id.length(),40))return fail(error,"MIGRATION_MEMORY_PRESSURE");
    graph.ruleIds.push_back(std::move(id));return true;
}
bool reviewRule(const PeriphExecRule& rule,void* context,String& error){
    auto& graph=*static_cast<Graph*>(context);
    if(!PeriphExecManager::getInstance().validateMigrationRule(rule,graph.peripherals,graph.ruleIds,error))return false;
#if !FASTBEE_ENABLE_PERIPH_EXEC
    if(rule.enabled)return fail(error,"MIGRATION_RULE_FEATURE_UNAVAILABLE");
#endif
    for(const auto& action:rule.actions){
        const auto type=static_cast<ExecActionType>(action.actionType);
        if(type==ExecActionType::ACTION_SYS_OTA)return fail(error,"MIGRATION_OTA_ACTION_UNAVAILABLE");
        if(type==ExecActionType::ACTION_TRIGGER_EVENT&&!findStaticEvent(action.targetPeriphId.c_str())){
            const auto* target=graph.target(action.targetPeriphId);
            if(!target||target->type!=PeripheralType::DEVICE_EVENT)return fail(error,"MIGRATION_EVENT_TARGET_MISSING");
        }
        if(type==ExecActionType::ACTION_MODBUS_POLL&&rule.enabled&&!graph.feature("modbus"))
            return fail(error,"MIGRATION_MODBUS_FEATURE_DISABLED");
        if(type==ExecActionType::ACTION_MODBUS_POLL){
            if(action.useReceivedValue)return fail(error,"MIGRATION_MODBUS_DYNAMIC_ACTION_UNVERIFIED");
            if(const char* issue=validateMigrationModbusAction(action.actionValue.c_str(),action.actionValue.length(),graph.modbus,rule.enabled))
                return fail(error,issue);
        }
        if(type==ExecActionType::ACTION_SCRIPT){
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            if(rule.enabled&&!graph.feature("commandScript"))return fail(error,"MIGRATION_COMMAND_SCRIPT_DISABLED");
            const String& script=action.actionValue.isEmpty()?rule.scriptContent:action.actionValue;
            const auto commands=ScriptEngine::parse(script);
            if(!ScriptEngine::validate(commands,error))return false;
            for(const auto& command:commands)if(command.type==ScriptCmdType::CMD_PERIPH&&!graph.target(command.strParam))
                return fail(error,"MIGRATION_SCRIPT_TARGET_MISSING");
#else
            return fail(error,"MIGRATION_COMMAND_SCRIPT_UNAVAILABLE");
#endif
        }
        if(type!=ExecActionType::ACTION_SENSOR_READ)continue;
        JsonDocument value;if(deserializeJson(value,action.actionValue.c_str(), action.actionValue.length()))return fail(error,"MIGRATION_SENSOR_ACTION_INVALID");
        const auto* target=graph.target(action.targetPeriphId);if(!target)return fail(error,"MIGRATION_SENSOR_TARGET_MISSING");
        String channel=value["channelField"]|"";String unit=value["unit"]|"";
        if(target->type==PeripheralType::SENSOR_GENERIC&&target->params.sensor.driver[0]){
            const auto* metadata=DriverRegistry::getInstance().getMetadata(target->params.sensor.driver);
            if(!metadata)return fail(error,"MIGRATION_SENSOR_DRIVER_UNAVAILABLE");
            if(channel.isEmpty()){
                const char* field=value["dataField"]|"";
                if(!resolveSensorChannel(target->params.sensor.driver,target->params.sensor.mode,field,channel,error,target->params.sensor.scanMask))
                    if(!resolveSensorChannel(target->params.sensor.driver,target->params.sensor.mode,"",channel,error,target->params.sensor.scanMask))return false;
            }else {
                const String requested=channel;
                if(requested.length()!=channel.length())return fail(error,"MIGRATION_MEMORY_PRESSURE");
                if(!resolveSensorChannel(target->params.sensor.driver,target->params.sensor.mode,requested.c_str(),channel,error,target->params.sensor.scanMask))return false;
            }
            if(unit.isEmpty())for(uint8_t i=0;i<metadata->activeChannelCount(target->params.sensor.mode,target->params.sensor.scanMask);++i){
                const auto* item=metadata->getChannel(i,target->params.sensor.mode,target->params.sensor.scanMask);if(channel==item->name)unit=item->unit;
            }
        }else if(channel.isEmpty())channel=PeriphExecExecutor::migrationChannelField(value["sensorCategory"]|"analog",target->id,value["dataField"]|"");
        if(!graph.addSource(target->id+"_"+channel,unit,true,error))return false;
    }
#if FASTBEE_ENABLE_SCREEN_PANEL
    for(JsonObjectConst page:graph.panel["pages"].as<JsonArrayConst>())for(JsonObjectConst card:page["cards"].as<JsonArrayConst>())
        if(String(card["controlRef"]|"")==String("rule.")+rule.id&&!ScreenPanelManager::validateMigrationControl(rule,card,error))return false;
#endif
    return true;
}
bool validateLayout(Graph& graph,String& error){
    const String display=graph.panel["displayRef"]|"";
    if(!display.isEmpty()){
        const auto* target=graph.target(display);
        if(!target||target->type!=PeripheralType::SCREEN_DISPLAY)return fail(error,"MIGRATION_DISPLAY_REFERENCE_MISSING");
    }
    for(JsonObjectConst page:graph.panel["pages"].as<JsonArrayConst>())for(JsonObjectConst card:page["cards"].as<JsonArrayConst>()){
        const String ref=card["sourceRef"]|"";
        if(!ref.isEmpty()&&!graph.source(ref))return fail(error,"MIGRATION_SOURCE_REFERENCE_MISSING");
        const Source* first=nullptr;
        for(JsonVariantConst value:card["sourceRefs"].as<JsonArrayConst>()){
            const auto* source=graph.source(value.as<String>());
            if(!source||!source->numeric||source->unit.isEmpty()||(first&&first->unit!=source->unit))return fail(error,"MIGRATION_BAR_SOURCE_INCOMPATIBLE");
            first=source;
        }
        const String control=card["controlRef"]|"";if(control.isEmpty())continue;
        if(String(card["visualType"]|"")=="motor"){
            if(!MotorControlService::instance().validReference(control))return fail(error,"MIGRATION_MOTOR_REFERENCE_CHANGED");
        }else{
            bool found=false;for(const auto& id:graph.ruleIds)if(control==String("rule.")+id)found=true;
            if(!found)return fail(error,"MIGRATION_PANEL_RULE_MISSING");
        }
    }return true;
}
bool reviewTriggers(const PeriphExecRule& rule,void* context,String& error){
    const auto& graph=*static_cast<Graph*>(context);
    // Review temperature references only after every rule's sampling action
    // has contributed its source metadata; file order must not decide validity.
    for(const auto& action:rule.actions)if(action.actionType==static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT)){
        ThermostatConfig config;
        if(!parseThermostatConfig(action.actionValue,config,error))return false;
        const auto* source=graph.source(config.sourceRef);
        if(!source||!source->numeric||(source->unit!="°C"&&source->unit!="℃"&&source->unit!="C"))
            return fail(error,"MIGRATION_THERMOSTAT_TEMPERATURE_SOURCE_REQUIRED");
    }
    for(const auto& trigger:rule.triggers){
        const auto type=static_cast<ExecTriggerType>(trigger.triggerType);
        if(type==ExecTriggerType::PLATFORM_TRIGGER||type==ExecTriggerType::TIMER_TRIGGER)continue;
        if(type==ExecTriggerType::POLL_TRIGGER){
            // An empty poll source is used by scheduled Modbus collection.
            if(!trigger.triggerPeriphId.isEmpty()&&!graph.triggerSource(trigger.triggerPeriphId))
                return fail(error,"MIGRATION_POLL_SOURCE_UNVERIFIED");
            continue;
        }
        if(type!=ExecTriggerType::EVENT_TRIGGER)return fail(error,"MIGRATION_TRIGGER_UNSUPPORTED");
        if(trigger.eventId.startsWith("ds:")){
            if(!graph.triggerSource(trigger.eventId.substring(3)))return fail(error,"MIGRATION_EVENT_SOURCE_MISSING");
            continue;
        }
        if(trigger.eventId.startsWith("button_")&&!trigger.triggerPeriphId.isEmpty()){
            const auto* target=graph.target(trigger.triggerPeriphId);
            if(!target||!isGpioDigitalInputType(target->type))return fail(error,"MIGRATION_BUTTON_REFERENCE_MISSING");
        }
        bool found=findStaticEvent(trigger.eventId.c_str())!=nullptr;
        for(const auto& id:graph.ruleIds)found|=id==trigger.eventId;
        for(const auto& id:graph.protocolEvents)found|=id==trigger.eventId;
        const auto* peripheral=graph.target(trigger.eventId);
        found|=peripheral&&peripheral->type==PeripheralType::DEVICE_EVENT;
        if(!found)return fail(error,"MIGRATION_EVENT_REFERENCE_MISSING");
    }
    return true;
}
} // namespace

bool validateGraph(const Manifest& manifest,void* opaque,String& error){
    error="";
    if(!opaque||!valid(manifest))return fail(error,"MIGRATION_REVIEW_CONTEXT");
    auto& review=*static_cast<ReviewContext*>(opaque);
    if(!review.report)return fail(error,"MIGRATION_REVIEW_CONTEXT");
    try{
        Graph graph;JsonDocument document;
        if(!readJson(effectivePath(manifest,1),graph.features,24576,error)||!validateFeatures(graph,error)||!validateDevice(manifest,error))return false;
        if(!readJson(effectivePath(manifest,2),document,24576,error))return false;
        if(document.isNull())document["peripherals"].to<JsonArray>();
        std::unique_ptr<PreflightResult> result(new(std::nothrow) PreflightResult);
        if(!result)return fail(error,"MIGRATION_MEMORY_PRESSURE");
        const bool peripheralsValid=PeripheralManager::getInstance().prepareConfigurationImport(document,graph.peripherals,review.resources,*result,error);
        JsonArray issues=(*review.report)["issues"].to<JsonArray>();
        for(size_t i=0;i<result->issueCount;++i){const auto& issue=result->issues[i];auto row=issues.add<JsonObject>();
            row["code"]=preflightIssueCodeName(issue.code);row["severity"]=preflightSeverityName(issue.severity);
            row["message"]=issue.message;
            // Planner owner IDs borrow the candidate vector. The response must
            // own its copy after this local graph has been destroyed.
            const char* owner=issue.ownerId?issue.ownerId:"";
            String copied;if(!copyCheckedText(copied,owner,strlen(owner),96))return fail(error,"MIGRATION_MEMORY_PRESSURE");
            row["ownerId"]=copied;
        }
        (*review.report)["requiresConfirmation"]=result->requiresConfirmation;
        if(!peripheralsValid)return false;
        for(const auto& peripheral:graph.peripherals){
            if(!peripheral.enabled)continue;
            const char* feature=nullptr;
            switch(peripheral.type){
                case PeripheralType::LCD:feature="lcd";break;
                case PeripheralType::LCD1602:feature="lcd1602";break;
                case PeripheralType::DS1302_RTC:feature="ds1302";break;
                case PeripheralType::RFID_MFRC522:feature="rfid";break;
                case PeripheralType::IO_EXPANDER:feature="ioExpander";break;
                case PeripheralType::NEO_PIXEL:feature="neopixel";break;
                case PeripheralType::SEVEN_SEGMENT_TM1637:feature="sevenSegment";break;
                case PeripheralType::IR_RECEIVER:feature="irRemote";break;
                case PeripheralType::SENSOR_GENERIC:if(peripheral.params.sensor.driver[0])feature="i2cSensors";break;
                default:break;
            }
            if(feature&&!graph.feature(feature))return fail(error,"MIGRATION_PERIPHERAL_FEATURE_DISABLED");
        }
        if(review.requireAcknowledgements&&(!result->allowed||result->requiresConfirmation))return fail(error,"MIGRATION_RESOURCE_CONFIRMATION_REQUIRED");
        if(!readJson(effectivePath(manifest,5),document,20*1024,error))return false;
        if(!document.isNull()){
#if FASTBEE_ENABLE_SCREEN_PANEL
            if(document["storageVersion"]!=1||document["touchUiMigration"]!=2||!document["revision"].is<uint32_t>()||
               !document["revision"].as<uint32_t>()||!ScreenPanelManager::validateConfig(document["config"],graph.panel,error))
                return fail(error,"MIGRATION_PANEL_SCHEMA");
#else
            if(selected(manifest,5))return fail(error,"MIGRATION_PANEL_UNAVAILABLE");
#endif
        }
        if(!graph.addSource("system.uptime","s",true,error)||!graph.addSource("system.freeHeap","B",true,error)||
           !graph.addSource("system.wifiRssi","dBm",true,error)||!graph.addSource("system.network","",false,error))return false;
        // Protocol intent is an immutable dependency, never imported. Validate
        // its UART against the replacement peripheral graph and carry exact
        // configured mapping IDs into layout/history reference checks.
        if(!readJson(effectivePath(manifest,9),document,MaxFileBytes,error))return false;
        const auto rtu=document["modbusRtu"].as<JsonObjectConst>();
        if(!readModbusReviewGraph(rtu,graph.modbus))return fail(error,"MIGRATION_MODBUS_BASELINE_SCHEMA");
        if(rtu["enabled"]|false){
            const auto* uart=graph.target(rtu["peripheralId"]|"");
            if(!uart||uart->type!=PeripheralType::UART||!uart->enabled||!graph.feature("modbus"))
                return fail(error,"MIGRATION_PROTOCOL_UART_MISSING");
            for(JsonObjectConst task:rtu["master"]["tasks"].as<JsonArrayConst>()){
                if(!(task["enabled"]|true))continue;
                for(JsonObjectConst mapping:task["mappings"].as<JsonArrayConst>()){
                    if(!text(mapping["sensorId"],80))return fail(error,"MIGRATION_MODBUS_MAPPING_INVALID");
                    if(!graph.addSource(String("modbus.")+mapping["sensorId"].as<const char*>(),mapping["unit"]|"",true,error))return false;
                    String triggerId;
                    const auto id=mapping["sensorId"].as<JsonString>();
                    if(!copyCheckedText(triggerId,id.c_str(),id.size(),80))return fail(error,"MIGRATION_MEMORY_PRESSURE");
                    graph.protocolTriggers.push_back(std::move(triggerId));
                }
                if((rtu["transferType"]|0)==1&&task["mappings"].size()==0)
                    graph.protocolTriggers.push_back(String("modbus_raw_")+String(task["slaveAddress"]|0));
            }
            size_t index=0;
            for(JsonObjectConst device:rtu["master"]["devices"].as<JsonArrayConst>()){
                const String type=device["deviceType"]|"";
                if((device["enabled"]|true)&&(type=="relay"||type=="pwm"||type=="pid"||type=="motor"))
                    graph.protocolEvents.push_back(String("mc:")+String(index));
                ++index;
            }
        }
#if FASTBEE_ENABLE_REMOTE_RESOURCES
        RemoteResourceManager::instance().appendChoices(graph.remote["sources"].to<JsonArray>(),graph.remote["controls"].to<JsonArray>());
        if(graph.remote.overflowed())return fail(error,"MIGRATION_MEMORY_PRESSURE");
        for(JsonObjectConst source:graph.remote["sources"].as<JsonArrayConst>())
            if(!graph.addSource(source["sourceRef"]|"",source["unit"]|"",String(source["type"]|"")!="weather",error))return false;
#endif
        const String rulesPath=effectivePath(manifest,3);
        if(!rulesPath.isEmpty()&&(!PeriphExecManager::reviewConfigurationFile(rulesPath.c_str(),collectRule,&graph,error)||
           !PeriphExecManager::reviewConfigurationFile(rulesPath.c_str(),reviewRule,&graph,error)||
           !PeriphExecManager::reviewConfigurationFile(rulesPath.c_str(),reviewTriggers,&graph,error)))return false;
        if(!validateLayout(graph,error))return false;
        if(!readJson(effectivePath(manifest,6),document,4096,error))return false;
        if(!document.isNull()){
            if(document["schemaVersion"]!=1||!document["enabled"].is<bool>()||!document["sourceAllowlist"].is<JsonArrayConst>()||
               document["sourceAllowlist"].size()>24||(!document["revision"].isNull()&&!document["revision"].is<uint32_t>()))return fail(error,"MIGRATION_HISTORY_SCHEMA");
            std::vector<String> seen;
            for(JsonVariantConst entry:document["sourceAllowlist"].as<JsonArrayConst>()){
                if(!text(entry,96))return fail(error,"MIGRATION_HISTORY_SOURCE_INVALID");
                const String source=entry.as<String>();const auto* metadata=graph.source(source);
                if(!metadata||!metadata->numeric)return fail(error,"MIGRATION_HISTORY_SOURCE_MISSING");
                for(const auto& prior:seen)if(prior==source)return fail(error,"MIGRATION_HISTORY_SOURCE_DUPLICATE");
                seen.push_back(source);
            }
#if !FASTBEE_ENABLE_POINT_HISTORY
            if(selected(manifest,6))return fail(error,"MIGRATION_HISTORY_UNAVAILABLE");
#endif
        }
        if(!readJson(effectivePath(manifest,4),document,RuleScriptImportMaxBytes,error))return false;
        if(!document.isNull()){
#if FASTBEE_ENABLE_RULE_SCRIPT
            RuleScriptMap<String,RuleScript> scripts;
            if(!ruleScriptCandidateMemory<decltype(scripts)>(document.as<JsonVariantConst>(),true))return fail(error,"MIGRATION_RULE_SCRIPT_MEMORY");
            if(!parseRuleScriptImport<RuleScript,String>(document.as<JsonVariantConst>(),scripts,error))return false;
            for(const auto& entry:scripts)if(entry.second.enabled&&!graph.feature("ruleScript"))return fail(error,"MIGRATION_RULE_SCRIPT_DISABLED");
#else
            if(selected(manifest,4))return fail(error,"MIGRATION_RULE_SCRIPT_UNAVAILABLE");
#endif
        }
        if(selected(manifest,7)){
#if FASTBEE_ENABLE_IR_REMOTE
            if(rulesPath.isEmpty())return fail(error,"MIGRATION_IR_EXECUTION_CONFIG_REQUIRED");
            String legacy,current;
            if(!PeriphExecManager::readConfigurationIrKeys(effectivePath(manifest,7).c_str(),true,legacy,error)||
               (!rulesPath.isEmpty()&&!PeriphExecManager::readConfigurationIrKeys(rulesPath.c_str(),false,current,error)))return false;
            if(legacy.length()>24576)return fail(error,"MIGRATION_IR_KEYS_TOO_LARGE");
            JsonDocument keys,existing;
            if(legacy.isEmpty())legacy="[]";
            if(deserializeJson(keys,legacy.c_str(), legacy.length())||!keys.is<JsonArrayConst>())return fail(error,"MIGRATION_IR_KEYS_INVALID");
            if(!current.isEmpty()&&current!="[]"){
                if(deserializeJson(existing,current.c_str(), current.length())||existing.as<JsonVariantConst>()!=keys.as<JsonVariantConst>())
                    return fail(error,"MIGRATION_IR_KEYS_AMBIGUOUS");
            }
            if(keys.size()>64)return fail(error,"MIGRATION_IR_KEYS_TOO_LARGE");
            for(JsonVariantConst key:keys.as<JsonArrayConst>())
                if(!key.is<JsonObjectConst>()||!text(key["name"],128)||!text(key["code"],64)||!text(key["protocol"],64))
                    return fail(error,"MIGRATION_IR_KEYS_INVALID");
#else
            return fail(error,"MIGRATION_IR_UNAVAILABLE");
#endif
        }
        (*review.report)["ruleCount"]=graph.ruleIds.size();
        (*review.report)["peripheralCount"]=graph.peripherals.size();
        (*review.report)["identityPreserved"]=true;
        (*review.report)["activation"]="next_boot";
        error="";
        return !review.report->overflowed()||fail(error,"MIGRATION_MEMORY_PRESSURE");
    }catch(const std::bad_alloc&){return fail(error,"MIGRATION_MEMORY_PRESSURE");}
}
} }
#endif
