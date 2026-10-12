
#define millis unused_virtual_millis
#include <Arduino.h>
#include "network/LiteralStringMatch.h"
#undef millis
#include <ArduinoJson.h>
#include "core/ModbusOrdinaryControl.h"
#include "core/PeripheralExecution.h"
#include <map>
#include <functional>
#include <cassert>
#include <cstring>
#include <cstdio>
static uint32_t clockNow=100;
static unsigned pmDepth=0,calls=0;
static bool failLock=false,callbackReject=false,uartCurrent=true;
static uint16_t sentAddress=0,sentValue=0;
static unsigned long millis(){return clockNow;}
static unsigned pdMS_TO_TICKS(unsigned value){return value;}
static void vTaskDelay(unsigned value){clockNow+=value;}
struct RecursiveMutexGuard {
    bool held;
    explicit RecursiveMutexGuard(void*,unsigned=0):held(!failLock){if(held)++pmDepth;}
    ~RecursiveMutexGuard(){if(held)--pmDepth;}
    bool isLocked()const{return held;}
};
enum class GPIOState { STATE_LOW,STATE_HIGH,STATE_UNDEFINED };
struct PeripheralConfig {
    bool enabled=true,modbus=true;
    struct {struct {
        uint8_t slaveAddress=2,deviceType=0,controlProtocol=0,channelCount=4,pwmResolution=8;
        uint16_t coilBase=10,pwmRegBase=20,motorRegs[5]{10,11,12,13,14};
        bool ncMode=false;
        int32_t motorCurrentPosition=321,motorLastPulse=42;
    }modbus;}params;
    bool isModbusPeripheral()const{return modbus;}
};
// @callbacks@
struct PeripheralManager {
    void* _mutex=nullptr;
    std::map<String,PeripheralConfig> peripherals;
    ModbusCoilWriteFunc _modbusCoilWrite;
    ModbusRegWriteFunc _modbusRegWrite;
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin&)const{return uartCurrent;}
    bool getPeripheralSnapshot(const String& id,PeripheralConfig& out) {
        RecursiveMutexGuard lock(_mutex);
        if(!lock.isLocked())return false;
        const auto found=peripherals.find(id);if(found==peripherals.end())return false;
        out=found->second;return true;
    }
    void setModbusCallbacks(ModbusCoilWriteFunc,ModbusRegWriteFunc);
    void clearModbusCallbacks();
    bool writeModbusPin(const String&,const PeripheralConfig&,GPIOState);
    bool writeModbusPWM(const String&,const PeripheralConfig&,uint32_t);
    bool writeModbusCoil(const String&,uint16_t,bool);
    bool writeModbusReg(const String&,uint16_t,uint16_t);
    bool writeModbusPin(const String&,GPIOState,const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
    bool writeModbusPWM(const String&,uint32_t,const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
    bool writeModbusCoil(const String&,uint16_t,bool,const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
    bool writeModbusReg(const String&,uint16_t,uint16_t,const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
};
struct MotorControlService {
    bool rejected=false,pending=false,failed=false,changed=false;
    unsigned queued=0;String action;
    FastBee::OrdinaryControlContext captured;
    static MotorControlService& instance(){static MotorControlService value;return value;}
    String referenceForSlave(uint8_t slave){assert(!pmDepth && slave==2);return "configured";}
    bool snapshotTarget(const String&,FastBee::ModbusControlTarget& target){
        assert(!pmDepth);target={};target.kind=FastBee::ModbusControlTarget::Kind::Motor;
        target.instance=target.configuration=1;target.slave=2;target.function=6;
        for(unsigned i=0;i<5;++i)target.motorRegisters[i]=10+i;
        if(changed)target.motorRegisters[0]=99;
        return true;
    }
    bool submitRule(const String&,const String& selected,int,const String&,uint32_t,uint16_t,
        uint32_t& id,String&,const FastBee::UartEventOrigin&,const FastBee::OrdinaryControlContext* source){
        assert(!pmDepth && source);captured=*source;action=selected;id=77;++queued;return !rejected;
    }
    bool operation(uint32_t id,JsonObject result){
        assert(!pmDepth && id==77);result["pending"]=pending;result["success"]=!pending&&!failed;
        auto step=result["outputSteps"].to<JsonArray>().add<JsonObject>();
        step["attempted"]=true;step["commandAcknowledged"]=true;
        if(failed){auto suffix=result["outputSteps"].as<JsonArray>().add<JsonObject>();
            suffix["attempted"]=true;suffix["commandAcknowledged"]=false;}
        return true;
    }
};
// @production@
// @aggregate@
static bool executeTyped(PeripheralManager& pm,ExecActionType actType,const String& effectiveValue,
    const FastBee::OrdinaryControlContext* control,FastBee::OrdinaryControlResult* controlResult) {
    ExecAction action;action.targetPeriphId="device";
    PeripheralConfig digitalTarget;
    const bool digitalTargetFound=pm.getPeripheralSnapshot(action.targetPeriphId,digitalTarget);
    // @typed@
    return false;
}
struct NamedCommand { String strParam="device",subAction="HIGH";int32_t intParam=0; };
static bool executeNamed(PeripheralManager& pm,const NamedCommand& cmd,
    const FastBee::OrdinaryControlContext* control,FastBee::OrdinaryControlResult* controlResult) {
    PeripheralConfig nativeTarget;
    auto sourceCurrent=[&](){return control && pm.isUartEventOriginCurrent(control->uartOrigin);};
    do {
        // @script@
        return false;
    }while(false);
    return true;
}
static bool callback(uint8_t slave,uint16_t address,uint16_t value,
    const FastBee::OrdinaryControlContext& source,FastBee::OrdinaryControlResult& result) {
    assert(!pmDepth && slave==2 && source.request.operation==17 && source.request.actor.identity==9);
    ++calls;sentAddress=address;sentValue=value;result.evaluated=true;result.attempted=true;
    result.accepted=!callbackReject;result.reason=callbackReject?"CONTROL_RESULT_UNKNOWN":"CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY";
    return result.accepted;
}
int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];
    PeripheralManager pm;pm.peripherals["device"]={};
    pm.setModbusCallbacks([](uint8_t s,uint16_t a,bool v,const FastBee::OrdinaryControlContext& c,
        FastBee::OrdinaryControlResult& r){return callback(s,a,v?1:0,c,r);},callback);
    auto& config=pm.peripherals["device"];
    FastBee::OrdinaryControlContext source;
    source.request.operation=17;source.request.actor={FastBee::OrdinaryControl::Source::Rule,9};
    source.configEpoch=FastBee::SensorConfigEpoch::current();
    FastBee::OrdinaryControlResult result;
    if(mode=="legacy") {
        assert(!pm.writeModbusCoil("device",10,true) && !pm.writeModbusReg("device",10,1));
        assert(!pm.writeModbusPin("device",config,GPIOState::STATE_HIGH));
        assert(!pm.writeModbusPWM("device",config,12) && !calls);return 0;
    }
    if(mode.rfind("motor_",0)==0) {
        config.params.modbus.deviceType=3;
        auto& motor=MotorControlService::instance();
        motor.rejected=mode=="motor_reject";motor.pending=mode=="motor_pending";
        motor.failed=mode=="motor_failure";motor.changed=mode=="motor_mapping";
        if(mode=="motor_pending")source.operationDeadlineAt=150;
        const bool ok=pm.writeModbusReg("device",10,200,source,result);
        assert(!calls && config.params.modbus.motorCurrentPosition==321 && config.params.modbus.motorLastPulse==42);
        assert(ok==(mode=="motor_success"));
        if(mode=="motor_mapping"){assert(!motor.queued);return 0;}
        assert(motor.queued==1 && motor.action=="forward" && motor.captured.request.operation==17);
        assert(result.backendOperationId==77 && result.attemptKnown==(mode!="motor_pending"));
        if(mode!="motor_reject")assert(result.attempted && result.acknowledgedSteps==1);
        return 0;
    }
    if(mode.rfind("script_",0)==0) {
        NamedCommand cmd;
        config.params.modbus.ncMode=true;
        if(mode=="script_low")cmd.subAction="LOW";
        if(mode=="script_pwm" || mode=="script_pwm_invalid"){
            config.params.modbus.deviceType=1;cmd.subAction="PWM";cmd.intParam=mode=="script_pwm"?125:-1;
        }
        if(mode=="script_unsupported")cmd.subAction="BLINK";
        if(mode=="script_failure")callbackReject=true;
        const bool ok=executeNamed(pm,cmd,mode=="script_context"?nullptr:&source,&result);
        const bool invalid=mode=="script_context" || mode=="script_pwm_invalid" || mode=="script_unsupported";
        assert(ok==(!invalid && mode!="script_failure"));
        assert(calls==(invalid?0:1) && result.evaluated);
        if(mode=="script_high")assert(sentValue==0);
        if(mode=="script_low")assert(sentValue==1);
        if(mode=="script_pwm")assert(sentAddress==20 && sentValue==125);
        return 0;
    }
    if(mode.rfind("typed_",0)==0) {
        // This suite intentionally models ESP32's C-string comparison boundary.
        assert(String("t\0ue",4).equalsIgnoreCase("true"));
        ExecActionType action=ExecActionType::ACTION_MODBUS_COIL_WRITE;String text="2:on";
        config.params.modbus.ncMode=true;
        if(mode=="typed_register"){action=ExecActionType::ACTION_MODBUS_REG_WRITE;text="31:600";}
        if(mode=="typed_reg_negative"){action=ExecActionType::ACTION_MODBUS_REG_WRITE;text="31:-1";}
        if(mode=="typed_overflow")text="65535:1";
        if(mode=="typed_bad_boolean")text="2:7";
        if(mode=="typed_nul_true")text=String("t\0ue",4);
        if(mode=="typed_nul_false")text=String("f\0lse",5);
        if(mode=="typed_pwm"){action=ExecActionType::ACTION_SET_PWM;text="120";config.params.modbus.deviceType=1;}
        if(mode=="typed_high")action=ExecActionType::ACTION_HIGH;
        if(mode=="typed_unsupported")action=ExecActionType::ACTION_BLINK;
        const bool ok=executeTyped(pm,action,text,mode=="typed_context"?nullptr:&source,&result);
        const bool invalid=mode=="typed_reg_negative" || mode=="typed_overflow" || mode=="typed_bad_boolean" ||
            mode=="typed_nul_true" || mode=="typed_nul_false" ||
            mode=="typed_unsupported" || mode=="typed_context";
        assert(ok==!invalid && calls==(invalid?0:1));
        if(mode=="typed_coil")assert(sentAddress==12 && sentValue==0);
        if(mode=="typed_register")assert(sentAddress==31 && sentValue==600);
        if(mode=="typed_high")assert(sentAddress==10 && sentValue==0);
        return 0;
    }
    if(mode=="disabled")config.enabled=false;
    if(mode=="stale")FastBee::SensorConfigEpoch::advance();
    if(mode=="uart")uartCurrent=false;
    if(mode=="busy")failLock=true;
    if(mode=="failure")callbackReject=true;
    if(mode=="cleared")pm.clearModbusCallbacks();
    if(mode=="nc")config.params.modbus.ncMode=true;
    if(mode=="register")config.params.modbus.controlProtocol=1;
    if(mode=="pwm" || mode=="pwm_overflow")config.params.modbus.deviceType=1;
    const bool ok=mode=="pwm" || mode=="pwm_overflow"?
        pm.writeModbusPWM("device",mode=="pwm"?128:65536,source,result):
        pm.writeModbusPin("device",GPIOState::STATE_HIGH,source,result);
    const bool attempted=mode=="pin" || mode=="register" || mode=="nc" || mode=="pwm" || mode=="failure";
    assert(ok==(attempted && mode!="failure") && calls==(attempted?1:0));
    assert(result.evaluated && result.attempted==attempted);
    if(mode=="register")assert(sentValue==1);
    if(mode=="nc")assert(sentValue==0);
    if(mode=="pwm")assert(sentValue==128 && sentAddress==20);
    assert(!pmDepth);
}
