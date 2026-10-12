#include <Arduino.h>
#include "network/LiteralStringMatch.h"
#include <ArduinoJson.h>
#include "core/ModbusOrdinaryControl.h"
#include "protocols/MotorProtocol.h"
#include "protocols/ModbusMotorReference.h"
#include <cassert>
#include <vector>

#define pdMS_TO_TICKS(x) (x)
constexpr int ONESHOT_SUCCESS = 0;
struct OneShotResult { int error = ONESHOT_SUCCESS; bool outputAttempted = true; };
struct ModbusSubDevice {
    String deviceType = "relay";
    char sensorId[32] = "relay";
    bool enabled = true, ncMode = false;
    uint8_t slaveAddress = 2, channelCount = 4, controlProtocol = 0, batchRegType = 0;
    uint16_t coilBase = 100, batchRegister = 0, pwmRegBase = 200;
    uint16_t pidAddrs[8]{}, motorRegs[5]{1,2,3,4,5};
    uint8_t motorDecimals=0, pwmResolution=8;
    MotorProtocol motorProtocol;
};
struct Call { bool reg; uint8_t slave; uint16_t address, value; };
struct ModbusHandler {
    struct SensorIdMatch { uint8_t deviceIndex = 0, channel = 0; String action = "all"; bool matched = true; } match;
    ModbusSubDevice dev;
    bool found = true, emptyId = false;
    bool snapshotBusy = false;
    int failAt = -1;
    std::vector<Call> calls;
    bool findBySensorIdEx(const String&, SensorIdMatch& out) { out = match; return found; }
    bool snapshotControlSensor(const String&, SensorIdMatch& out, ModbusSubDevice& copy,
        FastBee::ModbusControlTarget& target) {
        out=match; out.matched=found; copy=dev;
        if(snapshotBusy) return false;
        if(emptyId) copy.sensorId[0]='\0';
        target.instance=1; target.configuration=1; target.slave=dev.slaveAddress;
        target.first=dev.coilBase; target.count=dev.channelCount; target.function=dev.controlProtocol?6:5;
        target.batch=dev.batchRegister; target.batchType=dev.batchRegType;
        return true; // Callback fixture only; actual snapshot/PM/bus has its own harness.
    }
    bool snapshotOrdinaryFrame(const FastBee::ModbusOutputFrame&, FastBee::ModbusControlTarget&) {
        assert(false && "Relay fixture entered scalar mapping"); return false;
    }
    const ModbusSubDevice& getSubDevice(uint8_t) const { return dev; }
    String buildSensorId(uint8_t, uint16_t channel) { return emptyId ? String() : "relay_ch" + String(channel); }
    OneShotResult write(bool reg, uint8_t slave, uint16_t address, uint16_t value,
                        bool control, uint16_t timeout, uint8_t retries) {
        assert(control && timeout == 0 && retries == 0); // No implicit transport retries.
        const int index = static_cast<int>(calls.size());
        calls.push_back({reg, slave, address, value});
        return {index == failAt ? 1 : ONESHOT_SUCCESS};
    }
    OneShotResult writeCoilOnce(uint8_t slave,uint16_t address,bool value,bool control,uint16_t timeout,uint8_t retries) {
        return write(false,slave,address,value?1:0,control,timeout,retries);
    }
    OneShotResult writeRegisterOnce(uint8_t slave,uint16_t address,uint16_t value,bool control,uint16_t timeout,uint8_t retries) {
        return write(true,slave,address,value,control,timeout,retries);
    }
};
struct Lease { ModbusHandler* bus; ModbusHandler* get() const { return bus; } };
struct ProtocolManager {
    ModbusHandler bus; bool available = true;
    Lease acquireModbusHandler(int) { return {available ? &bus : nullptr}; }
};
struct PeripheralManager {
    static PeripheralManager& getInstance() { static PeripheralManager instance; return instance; }
    OneShotResult submitOrdinaryModbusRelay(ModbusHandler& handler, const FastBee::ModbusControlTarget& target,
        uint8_t function, uint16_t address, uint16_t value, const FastBee::OrdinaryControlContext&,
        FastBee::OrdinaryControlResult& result) {
        if(reject) { result.reason="CONTROL_CONFLICT"; result.attempted=false; return {7,false}; }
        const auto answer=handler.write(function==6,target.slave,address,function==5?(value?1:0):value,true,0,0);
        result.attempted=true; result.accepted=answer.error==ONESHOT_SUCCESS;
        result.reason=result.accepted?"CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY":"CONTROL_RESULT_UNKNOWN";
        return answer;
    }
    OneShotResult submitOrdinaryModbusFrame(ModbusHandler&, const FastBee::ModbusControlTarget&,
        const FastBee::ModbusOutputFrame&, const FastBee::OrdinaryControlContext&,
        FastBee::OrdinaryControlResult&) {
        assert(false && "Relay fixture entered scalar submission"); return {7,false};
    }
    bool reject = false;
    bool writeModbusReg(const String&,uint16_t,uint16_t) { assert(false && "Relay bypassed explicit single-attempt transport"); return false; }
};
struct MotorControlService {
    unsigned submissions=0;bool reject=false;
    String action,property;uint16_t value=0,requested=0;
    FastBee::OrdinaryControlContext context;
    static MotorControlService& instance(){static MotorControlService result;return result;}
    bool snapshotTarget(const String& ref,FastBee::ModbusControlTarget& target) {
        assert(ref.startsWith("motor."));target.kind=FastBee::ModbusControlTarget::Kind::Motor;
        target.instance=1;target.configuration=1;target.slave=2;target.function=6;return true;
    }
    bool submitMqtt(const String& ref,const String& command,int raw,const String& request,const String& reportId,
        uint16_t reportValue,uint32_t& operation,String& error,const FastBee::OrdinaryControlContext& control) {
        assert(ref.startsWith("motor.") && request.startsWith("mqtt-"));
        assert(control.request.operation==123 && control.request.actor.identity==77);
        ++submissions;action=command;value=raw;property=reportId;requested=reportValue;context=control;
        if(reject){error="CONTROL_CONFLICT";operation=0;return false;}operation=42;return true;
    }
};
struct PeriphExecManager {
    unsigned events = 0;
    String payload;
    static PeriphExecManager& getInstance() { static PeriphExecManager instance; return instance; }
    void triggerEventById(const String& id,const String& data) { assert(id == "mc:0"); ++events; payload=data; }
};

int main(int argc,char** argv) {
    assert(argc == 2); const String scenario = argv[1];
    ProtocolManager protocols; auto* pm = &protocols; auto& bus = protocols.bus;
    PeripheralManager::getInstance().reject = scenario == "source_rejected" || scenario == "single_source_rejected";
    bus.snapshotBusy = scenario == "mapping_busy";
    String input = "ON";
    const bool single = scenario.startsWith("single_") || scenario == "invalid_bool_single" || scenario == "invalid_channel";
    const bool batch = scenario.startsWith("batch_") || scenario == "invalid_batch_mask";
    if (single) { bus.match.action = ""; bus.match.channel = 1; }
    if (batch) { bus.dev.batchRegister = 500; bus.dev.batchRegType = scenario == "batch_command_pass" ? 1 : 0; }
    if (scenario.startsWith("register")) bus.dev.controlProtocol = 1;
    if (scenario == "coil_fail_first" || scenario == "batch_fail" || scenario == "single_fail") bus.failAt = 0;
    if (scenario == "register_fail_middle") bus.failAt = 1;
    if (scenario == "coil_fail_last") bus.failAt = 3;
    if (scenario == "single_nc" || scenario == "invalid_bool_nc" || scenario == "off_nc") bus.dev.ncMode = true;
    if (scenario.startsWith("invalid_bool")) input = "not-a-boolean";
    if (scenario == "empty_bool") input = "";
    if (scenario == "embedded_bool") input = String("on\0extra", 8);
    const bool off = scenario == "off_pass" || scenario == "off_nc";
    if (off) input = "fAlSe";
    if (scenario == "disabled") bus.dev.enabled = false;
    if (scenario == "zero_channels") bus.dev.channelCount = 0;
    if (scenario == "overflow") bus.dev.coilBase = 65535;
    if (scenario == "invalid_channel") bus.match.channel = 4;
    if (scenario == "invalid_batch_mask") bus.dev.channelCount = 17;
    if (scenario == "fallback_id") bus.emptyId = true;
    if (scenario == "unmatched") bus.found = false;
    if (scenario == "unavailable") protocols.available = false;
    const bool motor=scenario.startsWith("motor_");
    auto& motors=MotorControlService::instance();
    if(motor) {
        bus.dev.deviceType="motor";strcpy(bus.dev.sensorId,"motor");bus.match.action="";bus.match.channel=0;input="7";
        if(scenario.startsWith("motor_oper")){bus.match.action="oper";input=scenario=="motor_oper_reverse"?"2":scenario=="motor_oper_stop"?"0":"1";}
        if(scenario=="motor_speed" || scenario=="motor_spd"){bus.match.action=scenario=="motor_spd"?"spd":"speed";bus.match.channel=3;input="65535";}
        if(scenario=="motor_pulse" || scenario=="motor_pls"){bus.match.action=scenario=="motor_pls"?"pls":"pulse";bus.match.channel=4;input="123";}
        if(scenario=="motor_bad_enum"){bus.match.action="oper";input="3";}
        if(scenario=="motor_bad_number")input="65536";
        if(scenario=="motor_bad_text")input="1x";
        if(scenario=="motor_negative")input="-1";
        if(scenario=="motor_embedded")input=String("1\0x",3);
        if(scenario=="motor_disabled")bus.dev.enabled=false;
        motors.reject=scenario=="motor_rejected";
    }
    FastBee::OrdinaryControlContext source;source.request.operation=123;
    source.request.actor={FastBee::OrdinaryControl::Source::Mqtt,77};
    JsonDocument report; JsonArray items = report.to<JsonArray>();
    // @callback@
    const bool handled = callback(motor ? String("motor_property") : String("relay_all"), input, items, source);
    const auto& events = PeriphExecManager::getInstance();
    if(motor) {
        const bool invalid=scenario=="motor_bad_enum" || scenario=="motor_bad_number" || scenario=="motor_bad_text" ||
            scenario=="motor_negative" || scenario=="motor_embedded" || scenario=="motor_disabled";
        const bool accepted=!invalid && !motors.reject;
        assert(handled && items.size()==1 && bus.calls.empty() && !events.events);
        assert(motors.submissions==(invalid?0:1));
        assert(items[0]["accepted"]==accepted && items[0]["pending"]==accepted && items[0]["value"].isNull());
        assert(items[0]["commandAcknowledged"]==false && items[0]["physicalOutputVerified"]==false);
        if(accepted) {
            assert(items[0]["attemptKnown"]==false && items[0]["attempted"].isNull() && items[0]["operationId"]==42);
            const char* action=scenario=="motor_oper_stop"?"stop":scenario=="motor_oper_reverse"?"reverse":
                scenario=="motor_speed" || scenario=="motor_spd"?"setSpeed":scenario=="motor_pulse" || scenario=="motor_pls"?"setPulse":"forward";
            assert(motors.action==action && motors.property=="motor_property");
            const uint16_t original=static_cast<uint16_t>(std::stoul(input.c_str()));
            assert(motors.requested==original && motors.value==(bus.match.action=="oper"?1:original));
        } else assert(items[0]["attemptKnown"]==true && items[0]["attempted"]==false);
        return 0;
    }
    if (scenario == "unmatched" || scenario == "unavailable") {
        assert(!handled && items.size() == 0 && bus.calls.empty() && events.events == 0); return 0;
    }
    assert(handled); // Failure never becomes "unmatched", allowing a second output path.
    if (scenario == "source_rejected" || scenario == "single_source_rejected" || scenario == "mapping_busy") {
        assert(bus.calls.empty() && events.events==0 && !items.isNull());
        assert(items.size()==(single || scenario=="mapping_busy"?1U:4U));
        for (JsonObject item : items) assert(item["value"].isNull() && item["attempted"]==false && item["commandAcknowledged"]==false);
        return 0;
    }
    const bool invalidBool = scenario.startsWith("invalid_bool") || scenario == "empty_bool" || scenario == "embedded_bool";
    const bool invalidConfig = scenario == "disabled" || scenario == "zero_channels" || scenario == "overflow" ||
        scenario == "invalid_channel" || scenario == "invalid_batch_mask";
    if (invalidBool || invalidConfig) {
        assert(bus.calls.empty() && events.events == 0 && items.size() == 1);
        assert(items[0]["value"].isNull() && items[0]["attempted"] == false);
        assert(items[0]["remark"] == (invalidBool ? "modbus_boolean_invalid" : "modbus_config_invalid"));
        return 0;
    }
    assert(items.size() == (single ? 1U : 4U));
    const bool failed = bus.failAt >= 0;
    assert(bus.calls.size() == (single || batch ? 1U : failed ? static_cast<unsigned>(bus.failAt+1) : 4U));
    assert(events.events == (failed ? 0U : 1U));
    for (size_t i=0; i<bus.calls.size(); ++i) {
        const auto& call=bus.calls[i];
        assert(call.slave == 2 && call.reg == (batch || bus.dev.controlProtocol != 0));
        assert(call.address == static_cast<uint16_t>(batch ? 500 : 100 + (single ? 1 : i)));
        const bool physical = bus.dev.ncMode ? off : !off;
        assert(call.value == (physical ? batch && bus.dev.batchRegType == 0 ? 15 : 1 : 0));
    }
    for (size_t i=0; i<items.size(); ++i) {
        const bool attempted = single || batch || !failed || i <= static_cast<size_t>(bus.failAt);
        const bool ack = !failed || (!single && !batch && i < static_cast<size_t>(bus.failAt));
        JsonObject item = items[i];
        assert(item["attempted"] == attempted && item["commandAcknowledged"] == ack);
        assert(item["physicalOutputVerified"] == false && item["requestedValue"] == (off ? "0" : "1"));
        if (ack) assert(item["value"] == (off ? "0" : "1")); else assert(item["value"].isNull());
        assert(item["confirmationScope"] == (attempted ? batch ? "batch" : "channel" : "none"));
        const char* expected = !attempted ? "modbus_not_attempted" : ack
            ? batch ? "modbus_batch_acknowledged" : "modbus_command_acknowledged"
            : batch ? "modbus_batch_result_unknown" : "modbus_result_unknown";
        assert(item["remark"] == expected);
        if (scenario == "fallback_id") assert(item["id"].as<std::string>() == "relay_all_ch" + std::to_string(i));
    }
}
