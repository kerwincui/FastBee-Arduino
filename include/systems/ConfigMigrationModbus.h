#pragma once
#include <ArduinoJson.h>
#include "core/SystemConstants.h"
#include <cstring>

namespace FastBee { namespace ConfigMigration {
// Immutable metadata from the target protocol baseline. No transport or live
// manager is consulted while reviewing actions from another configuration.
struct ModbusReviewGraph {
    static_assert(Protocols::MODBUS_MAX_POLL_TASKS<=32,"Migration poll membership uses a 32-bit mask");
    struct Device {
        char type[8] = {};
        bool enabled = false;
        uint16_t channels = 0, base = 0;
        uint8_t resolution = 0;
    };
    bool master = false;
    bool tasks[Protocols::MODBUS_MAX_POLL_TASKS] = {};
    size_t taskCount = 0, deviceCount = 0;
    Device devices[Protocols::MODBUS_MAX_SUB_DEVICES];
};
inline bool migrationUnsigned(JsonVariantConst value,uint32_t max,uint32_t fallback,uint32_t& output) {
    if(value.isNull()){output=fallback;return true;}
    if(!value.is<uint32_t>()||value.as<uint32_t>()>max)return false;
    output=value.as<uint32_t>();return true;
}
inline bool readModbusReviewGraph(JsonObjectConst rtu,ModbusReviewGraph& graph) {
    graph={};
    if((!rtu["mode"].isNull()&&!rtu["mode"].is<JsonString>())||
       (!rtu["enabled"].isNull()&&!rtu["enabled"].is<bool>()))return false;
    const char* mode=rtu["mode"]|"slave";
    if(std::strcmp(mode,"slave")&&std::strcmp(mode,"master"))return false;
    graph.master=(rtu["enabled"]|false)&&!std::strcmp(mode,"master");
    const auto tasks=rtu["master"]["tasks"].as<JsonArrayConst>();
    const auto devices=rtu["master"]["devices"].as<JsonArrayConst>();
    if((!rtu["master"]["tasks"].isNull()&&tasks.isNull())||(!rtu["master"]["devices"].isNull()&&devices.isNull())||
       tasks.size()>Protocols::MODBUS_MAX_POLL_TASKS||devices.size()>Protocols::MODBUS_MAX_SUB_DEVICES)return false;
    for(JsonVariantConst task:tasks){
        if(!task.is<JsonObjectConst>()||(!task["enabled"].isNull()&&!task["enabled"].is<bool>()))return false;
        graph.tasks[graph.taskCount++]=task["enabled"]|true;
    }
    for(JsonVariantConst source:devices){
        if(!source.is<JsonObjectConst>()||(!source["enabled"].isNull()&&!source["enabled"].is<bool>()))return false;
        auto& device=graph.devices[graph.deviceCount++];
        const auto type=source["deviceType"].as<JsonString>();
        if(!type||type.size()!=std::strlen(type.c_str())||type.size()>=sizeof(device.type))return false;
        std::memcpy(device.type,type.c_str(),type.size()+1);device.enabled=source["enabled"]|true;
        uint32_t channels=0,base=0,resolution=0,protocol=0;
        if(!migrationUnsigned(source["channelCount"],255,2,channels)||!channels||
           !migrationUnsigned(source["pwmResolution"],16,8,resolution)||!resolution||
           !migrationUnsigned(source["controlProtocol"],1,0,protocol)||
           !migrationUnsigned(source[!std::strcmp(device.type,"pwm")?"pwmRegBase":"coilBase"],65535,0,base))return false;
        device.channels=channels;device.base=base;device.resolution=resolution;
    }
    return true;
}
inline const char* validateMigrationModbusControl(JsonObjectConst control,const ModbusReviewGraph& graph,bool enabled) {
    if(control.isNull()||!control["d"].is<uint8_t>())return "MIGRATION_MODBUS_DEVICE_INDEX";
    const uint8_t index=control["d"].as<uint8_t>();
    if(index>=graph.deviceCount)return "MIGRATION_MODBUS_DEVICE_INDEX";
    const auto& device=graph.devices[index];
    if(enabled&&(!graph.master||!device.enabled))return "MIGRATION_MODBUS_DEVICE_DISABLED";
    for(JsonPairConst field:control){
        const char* key=field.key().c_str();
        if(field.key().size()!=std::strlen(key)||
           (std::strcmp(key,"d")&&std::strcmp(key,"a")&&std::strcmp(key,"c")&&std::strcmp(key,"v")&&std::strcmp(key,"p")))
            return "MIGRATION_MODBUS_CONTROL_FIELD";
    }
    uint32_t channel=0,value=0;
    if(!migrationUnsigned(control["c"],255,0,channel)||channel>=device.channels||
       !migrationUnsigned(control["v"],65535,0,value))return "MIGRATION_MODBUS_CONTROL_RANGE";
    const auto action=control["a"].as<JsonString>();
    if(!action||action.size()!=std::strlen(action.c_str()))return "MIGRATION_MODBUS_CONTROL_COMMAND";
    const char* command=action.c_str();
    if(!std::strcmp(device.type,"relay")){
        if(std::strcmp(command,"on")&&std::strcmp(command,"off")&&std::strcmp(command,"1")&&std::strcmp(command,"0")&&
           std::strcmp(command,"true")&&std::strcmp(command,"false"))return "MIGRATION_MODBUS_CONTROL_COMMAND";
        if(control.containsKey("v")||control.containsKey("p")||uint32_t(device.base)+channel>65535)return "MIGRATION_MODBUS_CONTROL_RANGE";
    }else if(!std::strcmp(device.type,"pwm")){
        if(std::strcmp(command,"pwm")||control["v"].isNull()||control.containsKey("p")||
           value>((uint32_t(1)<<device.resolution)-1)||uint32_t(device.base)+channel>65535)return "MIGRATION_MODBUS_CONTROL_RANGE";
    }else if(!std::strcmp(device.type,"pid")){
        const auto parameter=control["p"].as<JsonString>();
        if(std::strcmp(command,"pid")||channel||control["v"].isNull()||!parameter||parameter.size()!=1||
           (parameter.c_str()[0]!='P'&&parameter.c_str()[0]!='I'&&parameter.c_str()[0]!='D'))return "MIGRATION_MODBUS_CONTROL_COMMAND";
    }else if(!std::strcmp(device.type,"motor")){
        if(channel||control.containsKey("p")||
           (std::strcmp(command,"forward")&&std::strcmp(command,"reverse")&&std::strcmp(command,"stop")))return "MIGRATION_MODBUS_CONTROL_COMMAND";
    }else return "MIGRATION_MODBUS_DEVICE_UNSUPPORTED";
    return nullptr;
}
inline const char* validateMigrationModbusAction(const char* text,size_t length,const ModbusReviewGraph& graph,bool enabled) {
    if(!text||!length||std::strlen(text)!=length)return "MIGRATION_MODBUS_ACTION_INVALID";
    size_t start=0;while(start<length&&text[start]==' ')++start;
    if(start==length)return "MIGRATION_MODBUS_ACTION_INVALID";
    if(start&&text[start]=='{')return "MIGRATION_MODBUS_ACTION_INVALID"; // Runtime distinguishes JSON at byte zero.
    const auto task=[&](uint32_t index)->const char* {
        if(index>=graph.taskCount)return "MIGRATION_MODBUS_TASK_INDEX";
        if(enabled&&(!graph.master||!graph.tasks[index]))return "MIGRATION_MODBUS_TASK_DISABLED";
        return nullptr;
    };
    if(text[start]!='{'){
        size_t count=0;uint32_t seen=0;
        while(start<length){
            while(start<length&&text[start]==' ')++start;
            if(start==length||text[start]<'0'||text[start]>'9')return "MIGRATION_MODBUS_TASK_INDEX";
            uint32_t index=0;
            while(start<length&&text[start]>='0'&&text[start]<='9'){
                index=index*10+uint32_t(text[start++]-'0');if(index>255)return "MIGRATION_MODBUS_TASK_INDEX";
            }
            if(const char* error=task(index))return error;
            if(++count>Protocols::MODBUS_MAX_POLL_TASKS||(seen&(1U<<index)))return "MIGRATION_MODBUS_TASK_DUPLICATE";
            seen|=1U<<index;while(start<length&&text[start]==' ')++start;
            if(start==length)break;
            if(text[start++]!=','||start==length)return "MIGRATION_MODBUS_ACTION_INVALID";
        }
        return count?nullptr:"MIGRATION_MODBUS_ACTION_INVALID";
    }
    JsonDocument document;
    if(deserializeJson(document,text,length)||document.overflowed()||!document.is<JsonObjectConst>())return "MIGRATION_MODBUS_ACTION_INVALID";
    for(JsonPairConst field:document.as<JsonObjectConst>()){
        const char* key=field.key().c_str();
        if(field.key().size()!=std::strlen(key)||(std::strcmp(key,"poll")&&std::strcmp(key,"ctrl")))return "MIGRATION_MODBUS_ACTION_FIELD";
    }
    const auto poll=document["poll"].as<JsonArrayConst>(),controls=document["ctrl"].as<JsonArrayConst>();
    if((document.containsKey("poll")&&poll.isNull())||(document.containsKey("ctrl")&&controls.isNull())||
       (!poll.size()&&!controls.size())||poll.size()>Protocols::MODBUS_MAX_POLL_TASKS||controls.size()>16)return "MIGRATION_MODBUS_ACTION_SIZE";
    uint32_t seen=0;
    for(JsonVariantConst entry:poll){
        if(!entry.is<uint8_t>())return "MIGRATION_MODBUS_TASK_INDEX";
        const auto index=entry.as<uint8_t>();if(const char* error=task(index))return error;
        if(seen&(1U<<index))return "MIGRATION_MODBUS_TASK_DUPLICATE";
        seen|=1U<<index;
    }
    for(JsonVariantConst control:controls)
        if(const char* error=validateMigrationModbusControl(control.as<JsonObjectConst>(),graph,enabled))return error;
    return nullptr;
}
} }
