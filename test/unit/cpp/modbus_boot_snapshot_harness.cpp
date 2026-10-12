#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/PeripheralExecution.h"
#include "core/ModbusActionMode.h"
#include "core/UartEventOrigin.h"
#include "core/ModbusOrdinaryControl.h"
#include "utils/CheckedText.h"
#include <cassert>
#include <functional>

inline void convertFromJson(JsonVariantConst value, String& out) { out=value.as<const char*>(); }
struct Logger {
    template<class... T> void warning(T...) {}
    template<class... T> void warningf(T...) {}
    template<class... T> void info(T...) {}
    template<class... T> void infof(T...) {}
} LOGGER;
constexpr int MALLOC_CAP_INTERNAL=1, MODBUS_MASTER=1, ONESHOT_SUCCESS=0, ONESHOT_CONTROL_REJECTED=1;
static bool retireAtLease=false,retireAtPoll=false,retireAtDelay=false,retireAtWrite=false,retireAtEvent=false;
static bool restoreAfterRaw=false;
static bool changeMappingAtWrite=false;
static unsigned failRegisterAt=0;
struct ModbusHandler; struct OneShotResult;
struct PeripheralManager {
    bool current=true;
    OneShotResult submitOrdinaryModbusRelay(ModbusHandler&,const FastBee::ModbusControlTarget&,uint8_t,uint16_t,uint16_t,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,uint16_t);
    OneShotResult submitOrdinaryModbusFrame(ModbusHandler&,const FastBee::ModbusControlTarget&,
        const FastBee::ModbusOutputFrame&,const FastBee::OrdinaryControlContext&,
        FastBee::OrdinaryControlResult&,uint16_t);
    static PeripheralManager& getInstance() { static PeripheralManager value;return value; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        return origin.unbound() || (current && origin.matches(7,9));
    }
};
inline uint32_t heap_caps_get_free_size(int) { return 100000; }
inline unsigned pdMS_TO_TICKS(unsigned value) { return value; }
static uint32_t bootNow=100;
inline uint32_t bootMillis() { return bootNow; }
#define millis bootMillis
inline void vTaskDelay(unsigned ticks) { bootNow+=ticks;if(retireAtDelay) PeripheralManager::getInstance().current=false; }
inline uint32_t esp_random() { return 123; }
namespace FastBee { namespace MemoryBudget {
constexpr uint32_t MODBUS_POLL_MIN_DRAM=20000;
inline uint32_t dramFree() { return 100000; }
} }
namespace Protocols { constexpr size_t MODBUS_MAX_POLL_TASKS=8; }
struct HealthMonitor {
    bool isMemoryCritical() const { return false; }
    bool isMemorySevere() const { return false; }
};
struct ModbusSubDevice {
    const char* deviceType="relay";
    char sensorId[32]="old-property";
    uint8_t channelCount=4;
    bool ncMode=false;
    uint8_t slaveAddress=1,controlProtocol=0;
    uint16_t coilBase=0,pwmRegBase=0,pidAddrs[6]={};
};
struct ModbusConfig {
    uint8_t transferType=0;
    struct { uint8_t deviceCount=5; ModbusSubDevice devices[5]; } master;
};
struct OneShotResult { int error=ONESHOT_SUCCESS; const char* controlReason=nullptr; };
struct ModbusHandler {
    ModbusConfig config;
    unsigned reads=0,coils=0,registers=0;
    bool snapshotControlDevice(uint8_t index,ModbusSubDevice& device,FastBee::ModbusControlTarget& target) {
        if(index>=config.master.deviceCount)return false;
        device=config.master.devices[index];target={};
        target.instance=1;target.configuration=1;target.slave=device.slaveAddress;
        target.index=index;target.function=device.controlProtocol?6:5;target.first=device.coilBase;target.count=device.channelCount;
        return true;
    }
    bool snapshotOrdinaryFrame(const FastBee::ModbusOutputFrame& frame,FastBee::ModbusControlTarget& target) {
        if(!frame.valid())return false;
        target={};target.kind=FastBee::ModbusControlTarget::Kind::Frame;
        target.instance=target.configuration=1;target.slave=frame.bytes[0];
        target.frameSlave=frame.bytes[0];target.function=frame.bytes[1];
        target.first=frame.address();target.frameQuantity=frame.quantity();return true;
    }
    bool complete=true;
    int getMode() const { return MODBUS_MASTER; }
    uint8_t getPollTaskCount() const { return 2; }
    const ModbusConfig& getConfigRef() const { return config; }
    OneShotResult writeCoilOnce(uint8_t,uint16_t,bool,bool,uint16_t,uint8_t) {
        ++coils;if(retireAtWrite) PeripheralManager::getInstance().current=false;
        if(changeMappingAtWrite) strcpy(config.master.devices[0].sensorId,"new-property");
        return {};
    }
    OneShotResult writeRegisterOnce(uint8_t,uint16_t,uint16_t,bool,uint16_t,uint8_t) {
        ++registers; return registers==failRegisterAt?OneShotResult{1,"CONTROL_RESULT_UNKNOWN"}:OneShotResult{};
    }
    String executePollTaskBatch(const uint8_t* indices,uint8_t count,uint16_t,uint8_t,uint16_t,bool,
        uint32_t,bool* completed,const std::function<void(uint8_t,const String&)>& raw) {
        reads+=count; *completed=complete;
        for(unsigned i=0;i<count;++i) assert(indices[i]<2);
        if(!count || !complete) return "[]";
        if(retireAtPoll) PeripheralManager::getInstance().current=false;
        if(config.transferType==1) {
            raw(1,"0103020001");
            if(restoreAfterRaw) {PeripheralManager::getInstance().current=true;raw(1,"0103020002");}
            return "[]";
        }
        return R"([{"id":"temperature","value":"25"}])";
    }
};
OneShotResult PeripheralManager::submitOrdinaryModbusRelay(ModbusHandler& bus,const FastBee::ModbusControlTarget& target,
    uint8_t fc,uint16_t address,uint16_t value,const FastBee::OrdinaryControlContext& context,
    FastBee::OrdinaryControlResult& result,uint16_t timeout) {
    assert(context.request.operation);result.evaluated=result.accepted=true;
    return fc==5?bus.writeCoilOnce(target.slave,address,value!=0,true,timeout,0):
        bus.writeRegisterOnce(target.slave,address,value,true,timeout,0);
}
OneShotResult PeripheralManager::submitOrdinaryModbusFrame(ModbusHandler& bus,const FastBee::ModbusControlTarget& target,
    const FastBee::ModbusOutputFrame& frame,const FastBee::OrdinaryControlContext& context,
    FastBee::OrdinaryControlResult& result,uint16_t timeout) {
    assert(context.request.operation && context.operationDeadlineAt && frame.valid());
    assert(target.slave==frame.bytes[0] && target.first==frame.address());
    auto out=frame.bytes[1]==5?bus.writeCoilOnce(target.slave,target.first,frame.word()!=0,true,timeout,0):
        bus.writeRegisterOnce(target.slave,target.first,frame.word(),true,timeout,0);
    result.evaluated=true;result.attempted=true;result.accepted=out.error==ONESHOT_SUCCESS;
    result.reason=result.accepted?"CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY":"CONTROL_RESULT_UNKNOWN";
    return out;
}
struct Lease { ModbusHandler* value; ModbusHandler* get() const { return value; } };
enum class ModbusDataSource { PeriphExecPoll };
struct ProtocolManager {
    ModbusHandler handler;
    unsigned acquisitions=0,published=0;
    Lease acquireModbusHandler(unsigned) {
        ++acquisitions;if(retireAtLease) PeripheralManager::getInstance().current=false;return {&handler};
    }
    void dispatchModbusData(uint8_t,const String&,ModbusDataSource) { ++published; }
};
struct FastBeeFramework {
    ProtocolManager protocol;
    static FastBeeFramework* getInstance() { static FastBeeFramework value; return &value; }
    ProtocolManager* getProtocolManager() { return &protocol; }
    HealthMonitor* getHealthMonitor() { return nullptr; }
};
struct MotorControlService {
    unsigned submissions=0;
    bool rejectAdmission=false, rejectWorker=false, pending=false,prefixFailure=false,successEvidence=false;
    FastBee::UartEventOrigin receivedOrigin;
    static MotorControlService& instance() { static MotorControlService value; return value; }
    String referenceForSlave(uint8_t) { return "motor"; }
    bool submitRule(const String&,const String&,uint16_t,const String&,uint32_t,uint16_t,uint32_t& id,String& error,
                    const FastBee::UartEventOrigin& origin,const FastBee::OrdinaryControlContext* control) {
        assert(control && control->request.operation && control->uartOrigin.required==origin.required);
        ++submissions; receivedOrigin=origin; id=1;
        if(rejectAdmission) {error="uart_source_unconfirmed";return false;}
        return true;
    }
    bool operation(uint32_t,JsonObject result) {
        result["pending"]=pending;result["success"]=!rejectWorker && !prefixFailure && !pending;
        if(rejectWorker) result["error"]="uart_source_unconfirmed";
        if(prefixFailure || successEvidence) {
            auto steps=result["outputSteps"].to<JsonArray>();
            auto acknowledged=steps.add<JsonObject>();acknowledged["attempted"]=true;acknowledged["commandAcknowledged"]=true;
            if(prefixFailure){auto failed=steps.add<JsonObject>();failed["attempted"]=true;failed["commandAcknowledged"]=false;
                result["error"]="motor_write_failed";}
        }
        return true;
    }
};
struct PeriphExecManager {
    unsigned events=0;
    std::vector<FastBee::UartEventOrigin> eventOrigins;
    void triggerEventById(const String&,const String&,const FastBee::UartEventOrigin& origin) {
        eventOrigins.push_back(origin);
        if(!PeripheralManager::getInstance().isUartEventOriginCurrent(origin)) return;
        ++events;if(retireAtEvent) PeripheralManager::getInstance().current=false;
    }
};
bool tryParseBoolLike(const String& value,bool& out) { out=value=="on";return value=="on" || value=="off"; }
struct PeriphExecExecutor {
    PeriphExecManager* _manager;
    bool executeModbusPollAction(const ExecAction&,const PeriphExecRule&,std::vector<ActionExecResult>*,
                                 FastBee::ModbusActionMode,const FastBee::UartEventOrigin& = {},bool* = nullptr,
                                 const FastBee::OrdinaryControlContext* = nullptr,FastBee::OrdinaryControlResult* = nullptr);
};

// @production@

int main(int argc,char** argv) {
    assert(argc==2); const String scenario=argv[1];
    auto& protocol=FastBeeFramework::getInstance()->protocol;
    auto& handler=protocol.handler;
    handler.config.master.devices[1].controlProtocol=1;
    handler.config.master.devices[2].deviceType="pwm";
    handler.config.master.devices[3].deviceType="pid";
    handler.config.master.devices[4].deviceType="motor";
    PeriphExecManager manager;PeriphExecExecutor executor{&manager};
    ExecAction action;action.actionValue=R"({"poll":[0,1],"ctrl":[{"d":0,"a":"on"},{"d":1,"a":"off"},{"d":2,"v":123},{"d":3,"v":4},{"d":4,"a":"forward"}]})";
    PeriphExecRule rule;
    const String original=action.actionValue;
    auto mode=FastBee::ModbusActionMode::ReadOnlySnapshot;
    FastBee::UartEventOrigin origin;
    const bool sourceCase=scenario.startsWith("source_");
    const bool motorCase=scenario.startsWith("motor_");
    const bool frameCase=scenario.startsWith("frame_");
    const bool mappingCase=scenario.startsWith("mapping_");
    if(frameCase) {
        mode=FastBee::ModbusActionMode::ExecuteRule;
        action.actionValue=R"({"ctrl":[{"d":2,"v":12},{"d":3,"p":"P","v":4},{"d":0,"a":"on"}]})";
        if(scenario=="frame_pwm_failure")failRegisterAt=1;
        if(scenario=="frame_pid_failure")failRegisterAt=2;
        if(scenario=="frame_pwm_overflow") {
            handler.config.master.devices[2].pwmRegBase=65535;
            action.actionValue=R"({"ctrl":[{"d":2,"c":1,"v":12}]})";
        }
        if(scenario=="frame_negative")action.actionValue=R"({"ctrl":[{"d":2,"v":-1}]})";
        if(scenario=="frame_pid_parameter")action.actionValue=R"({"ctrl":[{"d":3,"p":"unknown","v":1}]})";
        if(scenario=="frame_device_missing")action.actionValue=R"({"ctrl":[{"d":5,"a":"on"},{"d":0,"a":"on"}]})";
        if(scenario=="frame_device_negative")action.actionValue=R"({"ctrl":[{"d":-1,"a":"on"},{"d":0,"a":"on"}]})";
        if(scenario=="frame_action_nul")action.actionValue=R"({"ctrl":[{"d":0,"a":"on\u0000extra"},{"d":0,"a":"on"}]})";
        if(scenario=="frame_pid_nul")action.actionValue=R"({"ctrl":[{"d":3,"p":"P\u0000extra","v":1},{"d":0,"a":"on"}]})";
        if(scenario=="frame_prefix_bad_device")action.actionValue=R"({"ctrl":[{"d":2,"v":12},{"d":5,"a":"on"},{"d":0,"a":"on"}]})";
    }
    if(mappingCase) {
        mode=FastBee::ModbusActionMode::ExecuteRule;
        action.actionValue=R"({"ctrl":[{"d":0,"a":"on","c":2}]})";
        changeMappingAtWrite=scenario=="mapping_frozen";
        if(scenario=="mapping_unterminated") memset(handler.config.master.devices[0].sensorId,'x',32);
        if(scenario=="mapping_unmapped") handler.config.master.devices[0].sensorId[0]='\0';
    }
    if(motorCase) {
        mode=FastBee::ModbusActionMode::ExecuteRule;
        origin=FastBee::UartEventOrigin::captured(7,9);
        action.actionValue=R"({"ctrl":[{"d":4,"a":"forward"},{"d":0,"a":"on"}]})";
        MotorControlService::instance().rejectAdmission=scenario=="motor_admission";
        MotorControlService::instance().rejectWorker=scenario=="motor_worker";
        MotorControlService::instance().pending=scenario=="motor_timeout";
        MotorControlService::instance().prefixFailure=scenario=="motor_prefix_failure";
        MotorControlService::instance().successEvidence=scenario=="motor_success_evidence";
        if(scenario=="motor_timeout" || scenario=="motor_prefix_failure" || scenario=="motor_success_evidence")
            action.actionValue=R"({"ctrl":[{"d":4,"a":"forward"}]})";
    }
    if(sourceCase) {
        mode=FastBee::ModbusActionMode::ExecuteRule;
        origin=FastBee::UartEventOrigin::captured(7,9);
        if(scenario=="source_initial") PeripheralManager::getInstance().current=false;
        if(scenario=="source_malformed") origin.required=false;
        retireAtLease=scenario=="source_lease";
        retireAtPoll=scenario=="source_poll" || scenario=="source_raw_latched";
        retireAtDelay=scenario=="source_delay";
        retireAtWrite=scenario=="source_write";
        retireAtEvent=scenario=="source_child";
        if(scenario=="source_raw_latched") {handler.config.transferType=1;restoreAfterRaw=true;}
        if(scenario=="source_unbound") {origin={};PeripheralManager::getInstance().current=false;}
    }
    if(scenario=="normal" || scenario=="normal_raw" || scenario=="normal_then_snapshot")
        mode=FastBee::ModbusActionMode::ExecuteRule;
    if(scenario=="invalid_mode") mode=static_cast<FastBee::ModbusActionMode>(255);
    if(scenario=="raw" || scenario=="normal_raw") handler.config.transferType=1;
    if(scenario=="ctrl_only") action.actionValue=R"({"ctrl":[{"d":0,"a":"on"},{"d":4,"a":"forward"}]})";
    if(scenario=="empty") action.actionValue=R"({"poll":[],"ctrl":[]})";
    if(scenario=="legacy") action.actionValue="0,1";
    if(scenario=="malformed") action.actionValue="{";
    if(scenario=="incomplete") handler.complete=false;
    if(scenario=="poll_only") action.actionValue=R"({"poll":[0,1]})";
    if(scenario=="malformed_ctrl") action.actionValue=R"({"poll":[0,1],"ctrl":{"d":0,"a":"on"}})";
    const String before=action.actionValue;
    std::vector<ActionExecResult> results;
    FastBee::OrdinaryControlContext control;control.request.operation=1;control.uartOrigin=origin;
    FastBee::OrdinaryControlResult controlResult;
    bool sourceUnconfirmed=true;
    const bool ok=executor.executeModbusPollAction(action,rule,scenario=="no_results" ? nullptr : &results,mode,
                                                   origin,&sourceUnconfirmed,&control,&controlResult);
    assert(action.actionValue==before); // Observation never rewrites the stored rule.
#if !FASTBEE_ENABLE_MODBUS
    assert(!ok && !sourceUnconfirmed && protocol.acquisitions==0 && handler.reads==0 && results.empty());
#else
    if(frameCase) {
        assert(!ok && !handler.coils && !MotorControlService::instance().submissions);
        const unsigned attempted=scenario=="frame_pid_failure"?2:
            (scenario=="frame_pwm_failure" || scenario=="frame_prefix_bad_device")?1:0;
        assert(handler.registers==attempted);
        const bool prefix=scenario=="frame_pid_failure" || scenario=="frame_prefix_bad_device";
        assert(manager.events==(prefix?1:0));
        assert(!results.empty() && !results.back().success && results.back().actualValue.isEmpty());
        if(prefix)assert(results.size()==2 && results[0].success && results[0].actualValue=="12");
        return 0;
    }
    if(mappingCase) {
        assert(ok && results.size()==1 && results[0].success && handler.coils==1);
        const String* id=FastBee::capturedReportId(results[0]);
        if(scenario=="mapping_unterminated") assert(!id);
        else {
            assert(id && *id==(scenario=="mapping_unmapped" ? "modbus:0" : "old-property_ch2"));
            assert(results[0].actualValue==(scenario=="mapping_unmapped" ? "2:1" : "1"));
        }
        if(changeMappingAtWrite) assert(strcmp(handler.config.master.devices[0].sensorId,"new-property")==0);
        return 0;
    }
    if(motorCase && (scenario=="motor_timeout" || scenario=="motor_prefix_failure" || scenario=="motor_success_evidence")) {
        const bool success=scenario=="motor_success_evidence";
        assert(ok==success && !sourceUnconfirmed && results.size()==1 && results[0].success==success);
        assert(controlResult.evaluated && controlResult.accepted==success && controlResult.backendOperationId==1);
        assert(controlResult.attemptKnown==(scenario!="motor_timeout"));
        assert(controlResult.attempted==(scenario!="motor_timeout") && controlResult.acknowledgedSteps==(scenario=="motor_timeout"?0:1));
        assert(success || results[0].actualValue.isEmpty());
        assert(!handler.coils && !handler.registers && !manager.events);return 0;
    }
    if(motorCase) {
        assert(!ok && sourceUnconfirmed && handler.reads==0 && handler.coils==0 && handler.registers==0);
        assert(MotorControlService::instance().submissions==1);
        assert(MotorControlService::instance().receivedOrigin.matches(7,9));
        assert(PeripheralManager::getInstance().isUartEventOriginCurrent(origin));
        assert(results.size()==1 && !results[0].success && results[0].remark=="uart_source_unconfirmed");
        assert(manager.events==0); return 0;
    }
    if(sourceCase) {
        const bool valid=scenario=="source_valid" || scenario=="source_unbound";
        const bool early=scenario=="source_initial" || scenario=="source_malformed" || scenario=="source_lease";
        const bool pollRejected=scenario=="source_poll" || scenario=="source_raw_latched";
        const bool firstWrite=scenario=="source_write" || scenario=="source_child";
        assert(ok==valid && sourceUnconfirmed!=valid);
        assert(handler.reads==(early ? 0U : 2U));
        assert(protocol.published==((early || pollRejected) ? 0U : 1U));
        assert(handler.coils==((valid || firstWrite) ? 1U : 0U));
        assert(handler.registers==(valid ? 3U : 0U));
        assert(MotorControlService::instance().submissions==(valid ? 1U : 0U));
        if(valid) {
            const auto& motorOrigin=MotorControlService::instance().receivedOrigin;
            assert(motorOrigin.configEpoch==origin.configEpoch && motorOrigin.receiveGeneration==origin.receiveGeneration);
            assert(motorOrigin.required==origin.required);
        }
        assert(manager.events==(valid ? 4U : scenario=="source_child" ? 1U : 0U));
        unsigned sourceFailures=0;
        for(const auto& result:results) if(result.remark=="uart_source_unconfirmed") {
            ++sourceFailures;assert(!result.success && result.actualValue.isEmpty());
        }
        assert(sourceFailures==(valid ? 0U : 1U));
        assert(results.size()==(valid ? 5U : firstWrite ? 2U : 1U));
        for(const auto& eventOrigin:manager.eventOrigins) {
            assert(eventOrigin.configEpoch==origin.configEpoch && eventOrigin.receiveGeneration==origin.receiveGeneration);
            assert(eventOrigin.required==origin.required);
        }
        if(scenario=="source_initial" || scenario=="source_malformed") assert(protocol.acquisitions==0);
        return 0;
    }
    assert(!sourceUnconfirmed);
    const bool normal=scenario=="normal" || scenario=="normal_raw" || scenario=="normal_then_snapshot";
    if(scenario=="normal_then_snapshot") {
        assert(executor.executeModbusPollAction(action,rule,nullptr,FastBee::ModbusActionMode::ReadOnlySnapshot));
        assert(action.actionValue==original);
    }
    const bool noRead=scenario=="ctrl_only" || scenario=="empty" || scenario=="malformed" || scenario=="invalid_mode";
    assert(ok==(!noRead && scenario!="incomplete"));
    assert(handler.reads==(noRead ? 0U : scenario=="normal_then_snapshot" ? 4U : 2U));
    assert(protocol.published==((noRead || scenario=="incomplete") ? 0U : scenario=="normal_then_snapshot" ? 2U : 1U));
    assert(handler.coils==(normal ? 1U : 0U));
    assert(handler.registers==(normal ? 3U : 0U));
    assert(MotorControlService::instance().submissions==(normal ? 1U : 0U));
    assert(manager.events==(normal ? 4U : 0U));
    assert(results.size()==(normal ? 5U : scenario=="incomplete" ? 1U : 0U));
    if(scenario=="invalid_mode") assert(protocol.acquisitions==0);
    if(normal) assert(before==original);
#endif
}
