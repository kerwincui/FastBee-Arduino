#include <Arduino.h>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <thread>
#include "core/Pca9685ConfigSupport.h"
#include "core/NativeOutputControl.h"
namespace OC = FastBee::OrdinaryControl;
static uint64_t clockUs = 100000;
static uint32_t providerMicros() { return static_cast<uint32_t>(clockUs); }
#define micros providerMicros
#include "peripherals/PCA9685Provider.h"
#undef micros

static int64_t esp_timer_get_time() { return static_cast<int64_t>(clockUs); }
static uint32_t esp_random() { static uint32_t next=0; return ++next; }
static bool featureEnabled=true, budgetDenied=false;
struct GateTrace {
    bool owns=false, high=true, releaseOk=true;
    unsigned claims=0, releases=0, enables=0, disables=0;
} gateTrace;
namespace FastBee {
class RuntimeFeatureManager {
public:
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager v; return v; }
    bool shouldEnable(const char*) const { return featureEnabled; }
};
class FeatureBudgetRegistry {
public:
    static FeatureBudgetRegistry& instance() { static FeatureBudgetRegistry v; return v; }
    bool isDenied(const char*) const { return budgetDenied; }
};
class PCA9685NativeGate final : public PCA9685OutputLease {
public:
    bool begin(const String&, const String& consumer) { assert(!consumer.isEmpty()); ++gateTrace.claims; gateTrace.owns=true; return true; }
    int pin() const override { return 4; }
    bool owned() const override { return gateTrace.owns; }
    bool disabled() const override { return gateTrace.owns && gateTrace.high; }
    bool disable() override { ++gateTrace.disables; if(!owned())return false; gateTrace.high=true; return true; }
    bool enable() override { ++gateTrace.enables; if(!owned())return false; gateTrace.high=false; return true; }
    bool release() override {
        ++gateTrace.releases;
        assert(I2CBusManager::instance().clientCount()==0);
        if(!gateTrace.releaseOk)return false;
        gateTrace.owns=false;return true;
    }
};
}
struct RecursiveMutexGuard {
    bool acquired;
    RecursiveMutexGuard(const bool& available, unsigned):acquired(available){}
    bool isLocked() const { return acquired; }
};
class PeripheralManager {
public:
    bool _mutex=true;
    uint64_t version=1;
    unsigned validations=0, hardwareInits=0;
    uint64_t _nextOrdinaryControlOperation=0, nextGeneration=0;
    unsigned ordinaryInspections=0, ordinaryCommits=0;
    std::map<String,PeripheralConfig> peripherals;
    std::map<String,PeripheralRuntimeState> runtimeStates;
    bool validatePca9685Dependencies(const PeripheralConfig&, String&) const;
    bool pca9685Owns(const String&) const;
    bool preparePca9685(const String&, JsonObject, String&);
    bool startPca9685(const String&, const String&, bool, String&);
    bool shutdownPca9685();
    bool stopPca9685(const String&);
    void servicePca9685();
    bool commandPca9685(const String&, const String&, uint32_t, double, String&,
        const FastBee::OrdinaryControlContext*, FastBee::OrdinaryControlResult*);
    bool ordinaryPca9685QuiescentLocked(const String&) const;
    void invalidateOrdinaryPca9685Locked(const String&);
    // Common-PM orchestration is a seam here; the actual policy is reused.
    // Production common-helper integration is covered by ordinary_control tests.
    bool inspectOrdinaryControlLocked(const String& id,const FastBee::OrdinaryControlContext& context,
        OC::Intent intent,FastBee::OrdinaryControlResult& result,uint64_t& now) {
        ++ordinaryInspections; now=clockUs/1000;
        auto request=context.request; request.intent=intent;
        result.decision=OC::inspect(runtimeStates.at(id).ordinaryControl,request,now,false);
        if(result.decision==OC::Decision::Acquire || result.decision==OC::Decision::Continue ||
           result.decision==OC::Decision::RetireOwned || result.decision==OC::Decision::RetireForOverride) return true;
        result.reason="CONTROL_CONFLICT";return false;
    }
    bool commitOrdinaryControlLocked(const String& id,const FastBee::OrdinaryControlContext& context,
        OC::Intent intent,FastBee::OrdinaryControlResult& result,uint64_t& now,bool,OC::Permit& permit) {
        ++ordinaryCommits;
        if(!inspectOrdinaryControlLocked(id,context,intent,result,now))return false;
        auto& lease=runtimeStates.at(id).ordinaryControl;
        if(result.decision==OC::Decision::RetireOwned || result.decision==OC::Decision::RetireForOverride) {
            if(!retireOrdinaryDigitalLocked(id,lease.operation))return false;
        }
        if(lease.phase==OC::Phase::Idle) {
            lease.actor=context.request.actor;lease.operation=context.request.operation;
            lease.generation=++nextGeneration;lease.holdMs=context.request.holdMs;lease.acquiredMs=now;
            lease.phase=OC::Phase::Executing;lease.invocationActive=true;
        }
        permit={lease.actor,lease.operation,lease.generation};return true;
    }
    bool ordinaryPermitCurrentLocked(const String& id,const OC::Permit& permit) const {
        return OC::permitCurrent(runtimeStates.at(id).ordinaryControl,permit,clockUs/1000);
    }
    void updateOrdinaryControlResultLocked(const String& id,uint64_t,FastBee::OrdinaryControlResult& result) const {
        const auto& lease=runtimeStates.at(id).ordinaryControl;
        result.actor=lease.actor;result.operation=lease.operation;result.generation=lease.generation;result.phase=lease.phase;
    }
    bool completeOrdinaryControlLocked(const String& id,uint64_t now,FastBee::OrdinaryControlResult& result,
        bool accepted,const char* reason) {
        result.attempted=true;result.accepted=accepted;
        if(!accepted){runtimeStates.at(id).ordinaryControl.phase=OC::Phase::Fault;result.reason=reason;}
        updateOrdinaryControlResultLocked(id,now,result);return accepted;
    }
    bool retireOrdinaryDigitalLocked(const String& id,uint64_t cutoff,bool=false,bool keepInvocation=false) {
        auto& lease=runtimeStates.at(id).ordinaryControl;
        if(!ordinaryPca9685QuiescentLocked(id)){lease.phase=OC::Phase::Fault;return false;}
        lease.phase=OC::Phase::Retiring;
        if(keepInvocation && lease.invocationActive)return true;
        uint64_t watermark=std::max(cutoff,lease.retiredThrough);
        lease=OC::Lease{};lease.retiredThrough=watermark;return true;
    }
    FastBee::OrdinaryControlContext context(uint64_t actor=1) {
        FastBee::OrdinaryControlContext result;result.request.actor={OC::Source::Http,actor};
        result.request.operation=++_nextOrdinaryControlOperation;result.request.holdMs=30000;result.configEpoch=1;return result;
    }
    // Existing driver/lifecycle cases issue a real-shaped server context.
    // The no-context regression below calls the seven-argument API explicitly.
    bool commandPca9685(const String& id,const String& command,uint32_t channel,double value,String& error) {
        auto control=context();FastBee::OrdinaryControlResult result;
        const bool ok=commandPca9685(id,command,channel,value,error,&control,&result);
        auto& lease=runtimeStates.at(id).ordinaryControl;lease.invocationActive=false;
        if(lease.phase==OC::Phase::Executing)lease.phase=OC::Phase::Held;
        else if(lease.phase==OC::Phase::Retiring)retireOrdinaryDigitalLocked(id,_nextOrdinaryControlOperation);
        return ok;
    }
    bool getPca9685Status(const String&, JsonObject) const;
    uint64_t getConfigurationReviewVersion() const { return version; }
    bool validateIoDependencies(const PeripheralConfig& config,const String&,String& error) {
        ++validations;
        // The general dispatcher accepts valid non-PCA types. The tested PCA
        // method itself must reject them before interpreting union fields.
        return config.type!=PeripheralType::PCA9685_PWM || validatePca9685Dependencies(config,error);
    }
    bool resolveI2cConfig(const PeripheralConfig&,uint8_t& sda,uint8_t& scl,uint32_t& hz,String&) const {
        sda=21;scl=22;hz=100000;return true;
    }
    bool isValidPin(uint8_t pin) const { return pin<49; }
    bool isReservedPin(uint8_t) const { return false; }
    bool isInputOnlyPin(uint8_t) const { return false; }
    bool initHardware(const String&) { ++hardwareInits;return true; }
};
#include "pca_manager_under_test.inc"

static PeripheralManager configured() {
    PeripheralManager pm;
    PeripheralConfig bus,oe,pwm;
    bus.id="bus";bus.type=PeripheralType::I2C;bus.enabled=true;bus.pinCount=2;
    bus.pins[0]=21;bus.pins[1]=22;bus.params.i2c.frequency=100000;
    bus.params.i2c.isMaster=true;bus.params.i2c.address=0;
    oe.id="oe";oe.type=PeripheralType::GPIO_DIGITAL_OUTPUT;oe.enabled=true;oe.pinCount=1;
    oe.pins[0]=4;oe.params.gpio.initialState=GPIOState::STATE_HIGH;
    oe.params.gpio.passive=false;oe.params.gpio.invertLogic=false;
    pwm.id="pwm";pwm.type=PeripheralType::PCA9685_PWM;pwm.enabled=true;pwm.pinCount=0;
    JsonDocument params;String error;
    assert(!deserializeJson(params,R"({"busPeripheralId":"bus","oePeripheralId":"oe","address":64,"channelMask":3,"pwmHz":50,"clockHz":25000000,"minAngle":0,"maxAngle":180,"minPulseUs":500,"maxPulseUs":2500,"openDrain":false})"));
    assert(FastBee::parsePca9685Config(params.as<JsonObjectConst>(),pwm,error));
    for(const auto& config:{bus,oe,pwm}) {
        pm.peripherals[config.id]=config;
        pm.runtimeStates[config.id].status=PeripheralStatus::PERIPHERAL_INITIALIZED;
    }
    return pm;
}
static String prepare(PeripheralManager& pm,const String& id="pwm") {
    JsonDocument review;String error;
    assert(pm.preparePca9685(id,review.to<JsonObject>(),error));
    assert(review["physicalPowerVerified"]==false && review["oePadHigh"]==true);
    assert(gateTrace.owns && gateTrace.high);
    return String(review["token"].as<const char*>());
}
static void ready(PeripheralManager& pm) {
    String error; const String token=prepare(pm);
    assert(pm.startPca9685("pwm",token,true,error));
    assert(!pm.startPca9685("pwm",token,true,error));
    assert(pwmSlot().provider->state()==Provider::State::Waking);
    clockUs+=500;pm.servicePca9685();
    assert(pwmSlot().provider->state()==Provider::State::ReadyDisabled && gateTrace.high);
}
int main(int argc,char** argv) {
    assert(argc==2); const std::string mode=argv[1];
    auto pm=configured();String error;
    if(mode=="wrong_type") {
        JsonDocument review;
        pm.peripherals["pwm"].type=PeripheralType::GPIO_DIGITAL_OUTPUT;
        assert(!pm.preparePca9685("pwm",review.to<JsonObject>(),error));
        assert(pm.validations==0 && gateTrace.claims==0 && pm.hardwareInits==0 && Wire.transmissions==0);
    } else if(mode=="stale_review" || mode=="expired_review") {
        const String token=prepare(pm);
        if(mode=="stale_review")++pm.version;else clockUs+=60000000;
        assert(!pm.startPca9685("pwm",token,true,error));
        assert(Wire.transmissions==0 && gateTrace.enables==0);
        pm.servicePca9685();assert(!pwmSlot().gate && pwmSlot().closed);
    } else if(mode=="busy_wake") {
        const String token=prepare(pm);assert(pm.startPca9685("pwm",token,true,error));
        std::promise<void> entered,release;auto signal=release.get_future();
        std::thread holder([&]{auto& bus=FastBee::I2CBusManager::instance();assert(bus.lock());entered.set_value();signal.wait();bus.unlock();});
        entered.get_future().wait();clockUs+=500;pm.servicePca9685();
        assert(pwmSlot().provider->state()==Provider::State::Waking && pwmSlot().failures==0);
        release.set_value();holder.join();pm.servicePca9685();
        assert(pwmSlot().provider->ready());assert(pm.stopPca9685("pwm"));
    } else {
        ready(pm);
        if(mode=="no_context") {
            const auto tx=Wire.transmissions, enables=gateTrace.enables;
            FastBee::OrdinaryControlResult result;
            assert(!pm.commandPca9685("pwm","pulse",0,1500,error,nullptr,&result));
            assert(result.evaluated && !result.attempted && error=="CONTROL_CONTEXT_REQUIRED");
            assert(!pm.ordinaryInspections && Wire.transmissions==tx && gateTrace.enables==enables);
        } else if(mode=="whole_device_conflict") {
            assert(pm.commandPca9685("pwm","pulse",0,1500,error));
            const auto tx=Wire.transmissions;auto second=pm.context(2);FastBee::OrdinaryControlResult result;
            assert(!pm.commandPca9685("pwm","pulse",1,1500,error,&second,&result));
            assert(!result.attempted && Wire.transmissions==tx && pm.runtimeStates["pwm"].ordinaryControl.actor.identity==1);
        } else if(mode=="invalid_before_admission") {
            const auto tx=Wire.transmissions;auto control=pm.context();FastBee::OrdinaryControlResult result;
            assert(!pm.commandPca9685("pwm","angle",0,181,error,&control,&result));
            assert(!pm.ordinaryInspections && !pm.ordinaryCommits && Wire.transmissions==tx);
        } else if(mode=="emergency_revokes_invocation") {
            auto control=pm.context();FastBee::OrdinaryControlResult result;
            assert(pm.commandPca9685("pwm","enable",0,0,error,&control,&result));
            FastBee::OrdinaryControlResult stopped;
            assert(pm.commandPca9685("pwm","disable",0,0,error,nullptr,&stopped));
            auto& lease=pm.runtimeStates["pwm"].ordinaryControl;
            assert(gateTrace.high && lease.phase==OC::Phase::Retiring && lease.invocationActive);
            const auto enables=gateTrace.enables;
            assert(!pm.commandPca9685("pwm","enable",0,0,error,&control,&result));
            assert(gateTrace.enables==enables && lease.retiredThrough>=control.request.operation);
        } else if(mode=="closed_keeps_failure") {
            Wire.ack=false;assert(!pm.commandPca9685("pwm","pulse",0,1500,error));Wire.ack=true;
            assert(!pm.ordinaryPca9685QuiescentLocked("pwm"));
            assert(pm.stopPca9685("pwm") && pm.ordinaryPca9685QuiescentLocked("pwm"));
            assert(pwmSlot().providerFailures!=0 && !pwmSlot().firstError.isEmpty());
            assert(pm.runtimeStates["pwm"].ordinaryControl.phase==OC::Phase::Idle);
            const auto failed=pwmSlot().providerFailures;JsonDocument status;
            assert(pm.getPca9685Status("pwm",status.to<JsonObject>()) && status["physicalOutputVerified"]==false);
            assert(status["providerFailures"]==failed && status["state"]=="closed");
        } else if(mode=="prepare_pending_invocation") {
            auto control=pm.context();FastBee::OrdinaryControlResult result;
            assert(pm.commandPca9685("pwm","pulse",0,1500,error,&control,&result));
            assert(pm.stopPca9685("pwm"));
            const auto claims=gateTrace.claims;JsonDocument review;
            assert(!pm.preparePca9685("pwm",review.to<JsonObject>(),error));
            assert(gateTrace.claims==claims && error=="CONTROL_STOP_UNCONFIRMED");
            auto& lease=pm.runtimeStates["pwm"].ordinaryControl;lease.invocationActive=false;
            assert(pm.retireOrdinaryDigitalLocked("pwm",control.request.operation));
            assert(pm.preparePca9685("pwm",review.to<JsonObject>(),error));
            assert(review["physicalPowerVerified"]==false && gateTrace.high);
        } else if(mode=="lifecycle") {
            assert(pm.commandPca9685("pwm","pulse",0,1500,error));
            assert(pm.commandPca9685("pwm","enable",0,0,error));
            assert(!gateTrace.high && gateTrace.enables==1);
            const auto before=Wire.transmissions;pm.servicePca9685();assert(Wire.transmissions==before);
            JsonDocument status;assert(pm.getPca9685Status("pwm",status.to<JsonObject>()));
            assert(status["outputsEnabled"]==true && status["memoryMeasured"]==false);
            assert(pm.commandPca9685("pwm","disable",0,0,error) && gateTrace.high);
            assert(pm.stopPca9685("pwm") && !pwmSlot().provider && !gateTrace.owns);
        } else if(mode=="gate_lost_ready" || mode=="gate_lost_enabled" || mode=="unexpected_pad") {
            if(mode=="gate_lost_enabled")assert(pm.commandPca9685("pwm","enable",0,0,error));
            if(mode=="unexpected_pad")gateTrace.high=false;else gateTrace.owns=false;
            const auto before=Wire.transmissions;pm.servicePca9685();
            assert(pwmSlot().provider->state()==Provider::State::Faulted);
            assert(pwmSlot().firstError=="PCA_CONTROL_LOST" && pwmSlot().failures==1);
            pm.servicePca9685();assert(pwmSlot().failures==1 && Wire.transmissions==before);
            assert(!pm.commandPca9685("pwm","enable",0,0,error));
            gateTrace.owns=true;assert(pm.stopPca9685("pwm"));
        } else if(mode=="changed_config") {
            assert(pm.commandPca9685("pwm","enable",0,0,error));
            ++pm.version;pm.servicePca9685();
            assert(!pwmSlot().provider && !gateTrace.owns && gateTrace.high);
            assert(!pm.commandPca9685("pwm","enable",0,0,error));
        } else if(mode=="release_retry") {
            gateTrace.releaseOk=false;
            assert(!pm.stopPca9685("pwm") && pwmSlot().provider);
            const auto before=Wire.transmissions;
            assert(!pm.stopPca9685("pwm") && Wire.transmissions==before);
            gateTrace.releaseOk=true;pm.servicePca9685();
            assert(!pwmSlot().provider && Wire.transmissions==before);
            assert(pwmSlot().firstError=="PCA_STOP_UNCONFIRMED");
        } else if(mode=="new_identity") {
            Wire.ack=false;assert(!pm.commandPca9685("pwm","pulse",0,1500,error));Wire.ack=true;
            assert(pm.stopPca9685("pwm") && pwmSlot().providerFailures!=0);
            auto next=pm.peripherals["pwm"];next.id="replacement";
            pm.peripherals[next.id]=next;pm.runtimeStates[next.id]=pm.runtimeStates["pwm"];
            prepare(pm,next.id);
            JsonDocument status;assert(pm.getPca9685Status(next.id,status.to<JsonObject>()));
            assert(status["providerFailures"]==0 && status["providerError"]==0 && status["sessionFailure"]==0);
            assert(pm.stopPca9685(next.id));
        } else if(mode=="shutdown_fence") {
            pm._mutex=false;assert(!pm.shutdownPca9685());pm._mutex=true;
            pm.servicePca9685();assert(!pwmSlot().provider);
            JsonDocument review;assert(!pm.preparePca9685("pwm",review.to<JsonObject>(),error));
        } else assert(false);
    }
    if(mode=="no_context" || mode=="whole_device_conflict" || mode=="invalid_before_admission" ||
       mode=="emergency_revokes_invocation" || mode=="closed_keeps_failure" || mode=="prepare_pending_invocation")
        assert(pm.stopPca9685("pwm"));
    assert(FastBee::I2CBusManager::instance().clientCount()==0);
}
