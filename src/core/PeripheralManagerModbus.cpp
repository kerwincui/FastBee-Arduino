/** Source-bound virtual Modbus peripheral dispatch. */
#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/MotorControlService.h"
#include "systems/LoggerSystem.h"

namespace {
using ModbusContext=FastBee::OrdinaryControlContext;
using ModbusResult=FastBee::OrdinaryControlResult;
bool modbusRefuse(ModbusResult& result,const char* reason) {
    result.accepted=false;result.evaluated=true;result.reason=reason;return false;
}
bool modbusContextValid(const ModbusContext& context) {
    return context.request.operation && context.request.actor.identity && !context.modbusMaintenance &&
        FastBee::SensorConfigEpoch::matches(context.configEpoch) &&
        (!context.ruleBound || FastBee::RuleOutputIdentity::matches(context.ruleEpoch)) &&
        (context.request.actor.source!=FastBee::OrdinaryControl::Source::Screen ||
            FastBee::ScreenOutputIdentity::matches(context.request.actor.identity)) &&
        (!context.operationDeadlineAt || static_cast<int32_t>(
            static_cast<uint32_t>(millis())-context.operationDeadlineAt)<0);
}
bool executeVirtualMotorRegister(const PeripheralConfig& config,uint16_t address,uint16_t value,
    const ModbusContext& context,ModbusResult& result) {
    auto& motors=MotorControlService::instance();
    const String ref=motors.referenceForSlave(config.params.modbus.slaveAddress);
    FastBee::ModbusControlTarget target;
    if(ref.isEmpty() || !motors.snapshotTarget(ref,target) ||
        target.kind!=FastBee::ModbusControlTarget::Kind::Motor ||
        target.slave!=config.params.modbus.slaveAddress ||
        std::memcmp(target.motorRegisters,config.params.modbus.motorRegs,sizeof(target.motorRegisters)))
        return modbusRefuse(result,"CONTROL_MOTOR_MAPPING_UNCONFIRMED");
    int matched=-1;
    for(int i=0;i<5;++i)if(target.motorRegisters[i]==address) {
        if(matched>=0)return modbusRefuse(result,"CONTROL_MOTOR_ENDPOINT_AMBIGUOUS");
        matched=i;
    }
    if(matched<0 || (matched==2 && value!=target.stopValue))
        return modbusRefuse(result,"CONTROL_MOTOR_ENDPOINT_UNCONFIRMED");
    static const char* const actions[]={"forward","reverse","stop","setSpeed","setPulse"};
    uint32_t budget=30000,started=static_cast<uint32_t>(millis());
    if(context.operationDeadlineAt) {
        const int32_t remaining=static_cast<int32_t>(context.operationDeadlineAt-started);
        if(remaining<=0)return modbusRefuse(result,"CONTROL_SOURCE_STALE");
        if(static_cast<uint32_t>(remaining)<budget)budget=static_cast<uint32_t>(remaining);
    }
    char requestId[80];
    std::snprintf(requestId,sizeof(requestId),"pm-%llu-%u-%u",
        static_cast<unsigned long long>(context.request.operation),address,value);
    uint32_t operation=0;String error;
    const bool accepted=motors.submitRule(ref,String(actions[matched]),value,String(requestId),
        budget,1000,operation,error,context.uartOrigin,&context);
    result.backendOperationId=operation;result.attemptKnown=!accepted;
    target.key(result.targetId);
    result.actor=context.request.actor;result.operation=context.request.operation;
    if(!accepted)return modbusRefuse(result,"CONTROL_MOTOR_ADMISSION_REJECTED");
    bool terminal=false,success=false;
    auto observe=[&]() {
        JsonDocument record;
        if(!motors.operation(operation,record.to<JsonObject>()))return false;
        uint16_t acknowledged=0;size_t count=0;
        for(JsonObjectConst step:record["outputSteps"].as<JsonArrayConst>()) {
            if(++count>8)break;
            result.attempted |= step["attempted"] | false;
            if(step["commandAcknowledged"] | false)++acknowledged;
        }
        if(acknowledged>result.acknowledgedSteps)result.acknowledgedSteps=acknowledged;
        if(record["pending"] | true)return false;
        terminal=true;success=record["success"] | false;return true;
    };
    while(static_cast<uint32_t>(millis())-started<budget) {
        if(observe())break;
        if(!modbusContextValid(context))break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if(!terminal)observe();
    result.evaluated=true;result.accepted=success;result.attemptKnown=terminal;
    result.phase=terminal?FastBee::OrdinaryControl::Phase::Idle:FastBee::OrdinaryControl::Phase::Executing;
    result.reason=success?"CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY":
        terminal?"CONTROL_MOTOR_FAILED":"CONTROL_MOTOR_RESULT_UNCONFIRMED";
    // The real worker retains ownership if the caller runs out of budget.
    // Never infer pulse/position from ACKs or cancel an unknown physical move.
    return success;
}
}

void PeripheralManager::setModbusCallbacks(ModbusCoilWriteFunc coilWrite,ModbusRegWriteFunc regWrite) {
    RecursiveMutexGuard lock(_mutex);
    if(!lock.isLocked())return;
    _modbusCoilWrite=std::move(coilWrite);_modbusRegWrite=std::move(regWrite);
}
void PeripheralManager::clearModbusCallbacks() {
    RecursiveMutexGuard lock(_mutex);
    if(!lock.isLocked())return;
    _modbusCoilWrite=nullptr;_modbusRegWrite=nullptr;
}

// Contextless legacy entry points cannot invent a caller, deadline or takeover.
bool PeripheralManager::writeModbusPin(const String&,const PeripheralConfig&,GPIOState) { return false; }
bool PeripheralManager::writeModbusPWM(const String&,const PeripheralConfig&,uint32_t) { return false; }
bool PeripheralManager::writeModbusCoil(const String&,uint16_t,bool) { return false; }
bool PeripheralManager::writeModbusReg(const String&,uint16_t,uint16_t) { return false; }

bool PeripheralManager::writeModbusCoil(const String& id,uint16_t address,bool value,
    const ModbusContext& context,ModbusResult& result) {
    result={};result.evaluated=true;
    ModbusCoilWriteFunc callback;
    PeripheralConfig config;
    {
        RecursiveMutexGuard lock(_mutex,0);
        if(!lock.isLocked())return modbusRefuse(result,"CONTROL_BUSY");
        if(!modbusContextValid(context) || !isUartEventOriginCurrent(context.uartOrigin))
            return modbusRefuse(result,"CONTROL_SOURCE_STALE");
        auto found=peripherals.find(id);
        if(found==peripherals.end() || !found->second.enabled || !found->second.isModbusPeripheral() ||
            found->second.params.modbus.deviceType!=0 || found->second.params.modbus.controlProtocol!=0)
            return modbusRefuse(result,"CONTROL_MAPPING_UNCONFIRMED");
        config=found->second;
        try {callback=_modbusCoilWrite;}catch(...){return modbusRefuse(result,"CONTROL_ALLOCATION_FAILED");}
    }
    if(!callback)return modbusRefuse(result,"CONTROL_BACKEND_UNAVAILABLE");
    // The callback pins the handler and PM reserves the actual saved mapping.
    // Do not hold PM while acquiring protocol lifetime/bus/response locks.
    return callback(config.params.modbus.slaveAddress,address,value,context,result);
}
bool PeripheralManager::writeModbusReg(const String& id,uint16_t address,uint16_t value,
    const ModbusContext& context,ModbusResult& result) {
    result={};result.evaluated=true;
    ModbusRegWriteFunc callback;
    PeripheralConfig config;
    {
        RecursiveMutexGuard lock(_mutex,0);
        if(!lock.isLocked())return modbusRefuse(result,"CONTROL_BUSY");
        if(!modbusContextValid(context) || !isUartEventOriginCurrent(context.uartOrigin))
            return modbusRefuse(result,"CONTROL_SOURCE_STALE");
        auto found=peripherals.find(id);
        if(found==peripherals.end() || !found->second.enabled || !found->second.isModbusPeripheral())
            return modbusRefuse(result,"CONTROL_MAPPING_UNCONFIRMED");
        config=found->second;
        if(config.params.modbus.deviceType!=3)
            try {callback=_modbusRegWrite;}catch(...){return modbusRefuse(result,"CONTROL_ALLOCATION_FAILED");}
    }
    if(config.params.modbus.deviceType==3)
        return executeVirtualMotorRegister(config,address,value,context,result);
    if(!callback)return modbusRefuse(result,"CONTROL_BACKEND_UNAVAILABLE");
    return callback(config.params.modbus.slaveAddress,address,value,context,result);
}
bool PeripheralManager::writeModbusPin(const String& id,GPIOState state,
    const ModbusContext& context,ModbusResult& result) {
    result={};result.evaluated=true;
    PeripheralConfig config;
    if(!getPeripheralSnapshot(id,config) || !config.enabled || !config.isModbusPeripheral() ||
        config.params.modbus.deviceType!=0 || config.params.modbus.controlProtocol>1 ||
        (state!=GPIOState::STATE_HIGH && state!=GPIOState::STATE_LOW))
        return modbusRefuse(result,"CONTROL_PARAMETER_INVALID");
    bool value=state==GPIOState::STATE_HIGH;
    if(config.params.modbus.ncMode)value=!value;
    return config.params.modbus.controlProtocol==0?
        writeModbusCoil(id,config.params.modbus.coilBase,value,context,result):
        writeModbusReg(id,config.params.modbus.coilBase,value?1:0,context,result);
}
bool PeripheralManager::writeModbusPWM(const String& id,uint32_t value,
    const ModbusContext& context,ModbusResult& result) {
    result={};result.evaluated=true;
    PeripheralConfig config;
    if(!getPeripheralSnapshot(id,config) || !config.enabled || !config.isModbusPeripheral() ||
        config.params.modbus.deviceType!=1 || !config.params.modbus.pwmResolution ||
        config.params.modbus.pwmResolution>16 || value>((1UL<<config.params.modbus.pwmResolution)-1UL))
        return modbusRefuse(result,"CONTROL_PARAMETER_INVALID");
    return writeModbusReg(id,config.params.modbus.pwmRegBase,static_cast<uint16_t>(value),context,result);
}
