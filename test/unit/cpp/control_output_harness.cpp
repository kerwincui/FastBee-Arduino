#include "core/ControlOutput.h"
#include "core/PeripheralManager.h"
#include <driver/gpio.h>
#include "peripherals/drivers/H1ControlOutputs.h"
#include <cassert>
#include <future>
#include <new>
#include <string>

int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];
    using FastBee::ControlOutput;
    using Command=FastBee::ControlOutputCommand;
    auto& pm=PeripheralManager::getInstance();
    pm.peripherals["control"]=PeripheralConfig{};pm.runtimeStates["control"]=PeripheralRuntimeState{};
    auto& cfg=pm.peripherals["control"];auto& state=pm.runtimeStates["control"];
#if defined(ARDUINO_ARCH_ESP32)
    if(mode=="external_retirement") {
        AtomicSdkMock::allInternal=false;
        ControlOutput output;
        assert(!output.begin("control") && output.pin()==-1);
        assert(!state.controlOutputClaimed && !pm.controlOutputIsClaimed());
        assert(setCalls==0 && readCalls==0 && directionCalls==0 && waitedUs==0);
        assert(AtomicSdkMock::allocations==0);
        AtomicSdkMock::allInternal=true;
        assert(output.begin("control"));
        assert(output.end() && !state.controlOutputClaimed);
        return 0;
    }
#endif
    if(mode=="resource_snapshot") {
        FastBee::ControlOutputResourceSnapshot snapshot;
        pm.snapshotControlOutputResources(snapshot);
        assert(snapshot.status==FastBee::ResourceObservation::Observed && snapshot.count==0 && setCalls==0 && readCalls==0);
        auto* output=new ControlOutput;
        failDirection=true;
        assert(!output->begin("control", "pump-pwm", FastBee::ResourceConsumerRole::PcaGate));
        pm.snapshotControlOutputResources(snapshot);
        assert(snapshot.count==1 && snapshot.leases[0].fault && !snapshot.leases[0].retiring);
        controlBusy=true;delete output;controlBusy=false;
        pm.peripherals.clear();pm.runtimeStates.clear();
        pm.retireControlOutputs(); // Failed cleanup must remain observable without config/runtime records.
        const int writes=setCalls, directions=directionCalls;
        const unsigned reads=readCalls, waits=waitedUs;
        for(unsigned i=0;i<1000;++i) {
            pm.snapshotControlOutputResources(snapshot);
            assert(snapshot.count==1 && snapshot.leases[0].retiring && snapshot.leases[0].fault);
            assert(snapshot.leases[0].retirementAttempted && snapshot.leases[0].pin==4);
            assert(std::string(snapshot.leases[0].id)=="control");
            assert(std::string(snapshot.leases[0].consumer.id)=="pump-pwm");
            assert(snapshot.leases[0].consumer.role==FastBee::ResourceConsumerRole::PcaGate);
        }
        assert(setCalls==writes && directionCalls==directions && readCalls==reads && waitedUs==waits);
        std::promise<void> entered,release;auto ready=release.get_future();
        std::thread other([&]{controlMutex.lock();entered.set_value();ready.wait();controlMutex.unlock();});
        entered.get_future().wait();
        pm.snapshotControlOutputResources(snapshot);
        assert(snapshot.status==FastBee::ResourceObservation::Busy && snapshot.count==0);
        release.set_value();other.join();
        pm.snapshotControlOutputResources(snapshot);
        assert(snapshot.status==FastBee::ResourceObservation::Observed && snapshot.count==1);return 0;
    }
    if(mode=="consumer_identity") {
        using Role=FastBee::ResourceConsumerRole;
        ControlOutput output;
        assert(!output.begin("control", String(std::string(41,'x').c_str()), Role::McpReset));
        assert(!output.begin("control", "reset-consumer", static_cast<Role>(255)));
        assert(setCalls==0 && !state.controlOutputClaimed);
        String name="io-expander";
        assert(output.begin("control", name, Role::McpReset)); name="replaced";
        FastBee::ControlOutputResourceSnapshot first;
        pm.snapshotControlOutputResources(first);
        assert(first.count==1 && std::string(first.leases[0].consumer.id)=="io-expander");
        assert(first.leases[0].consumer.role==Role::McpReset);
        assert(output.end());
        assert(output.begin("control", "new-consumer", Role::PcaGate));
        FastBee::ControlOutputResourceSnapshot second; pm.snapshotControlOutputResources(second);
        assert(std::string(second.leases[0].consumer.id)=="new-consumer");
        assert(std::string(first.leases[0].consumer.id)=="io-expander");
        return 0;
    }
    if(mode=="retire_init_failure") {
        auto* output=new ControlOutput;failDirection=true;
        assert(!output->begin("control") && state.controlOutputClaimed);
        delete output;commanded.clear();pm.retireControlOutputs();
        assert((commanded==std::vector<int>{1}) && state.controlOutputRetiring && state.controlOutputRetirementFailed);
        assert(pm.controlOutputIsClaimed("control"));
        failDirection=false;controlNow=1000;pm.retireControlOutputs();
        assert(!pm.controlOutputIsClaimed("control") && state.controlOutputRetirementAttempts==2);
        assert(state.controlOutputRetirementFailed && !state.controlOutputFault);return 0;
    }
    if(mode=="retire_all_slots") {
        ControlOutput* outputs[4];
        for(int i=0;i<4;++i) {
            String id=String("extra")+String(i);pm.peripherals[id]=PeripheralConfig{};
            pm.peripherals[id].pins[0]=static_cast<uint8_t>(5+i);pm.runtimeStates[id]=PeripheralRuntimeState{};
            outputs[i]=new ControlOutput;assert(outputs[i]->begin(id));
        }
        controlBusy=true;for(auto* output:outputs)delete output;controlBusy=false;
        commanded.clear();failSetAt=setCalls+1;pm.retireControlOutputs();
        assert((commanded==std::vector<int>{1,1,1,1}));
        assert(pm.controlOutputIsClaimed("extra0"));
        for(int i=1;i<4;++i)assert(!pm.controlOutputIsClaimed(String("extra")+String(i)));
        const int previous=setCalls;pm.retireControlOutputs();assert(setCalls==previous);
        controlNow=1000;pm.retireControlOutputs();assert(!pm.controlOutputIsClaimed());return 0;
    }
    if(mode.rfind("retire_",0)==0) {
        alignas(ControlOutput) unsigned char storage[sizeof(ControlOutput)];
        auto* first=new(storage) ControlOutput;
        assert(first->begin("control") && first->low());
        if(mode=="retire_live_owner") {
            forcedRead=0;assert(!first->end());commanded.clear();
            pm.retireControlOutputs();assert(commanded.empty() && state.controlOutputClaimed);
            assert(state.controlOutputRetirementAttempts==0 && !state.controlOutputRetiring);
            forcedRead=-1;first->~ControlOutput();return 0;
        }
        if(mode=="retire_thread_busy") {
            std::promise<void> entered,release;auto ready=release.get_future();
            std::thread other([&]{controlMutex.lock();entered.set_value();ready.wait();controlMutex.unlock();});
            entered.get_future().wait();first->~ControlOutput();
            release.set_value();other.join();
        } else {
            controlBusy=true;first->~ControlOutput();controlBusy=false;
        }
        commanded.clear();
        if(mode=="retire_busy") {
            controlBusy=true;pm.retireControlOutputs();controlBusy=false;
            assert(commanded.empty() && state.controlOutputRetirementAttempts==0);
        }
        if(mode=="retire_address_reuse") {
            auto* next=new(storage) ControlOutput;
            assert(!next->begin("control"));int pin=-1;
            assert(!pm.controlOutput("control",next,Command::Owned,pin));
            assert(!pm.controlOutput("control",next,Command::Low,pin));
            assert(!pm.controlOutput("control",next,Command::PulseReset,pin));
            assert(commanded.empty());pm.retireControlOutputs();
            assert((commanded==std::vector<int>{1}));
            assert(next->begin("control") && next->owned());next->~ControlOutput();return 0;
        }
        if(mode=="retire_saturation")state.controlOutputRetirementAttempts=UINT32_MAX;
        if(mode=="retire_wrap")controlNow=UINT32_MAX-500u;
        const bool failed=mode=="retire_feedback"||mode=="retire_direction"||mode=="retire_write"||
            mode=="retire_wrap"||mode=="retire_drift"||mode=="retire_disabled"||mode=="retire_missing";
        if(mode=="retire_feedback"||mode=="retire_wrap")forcedRead=0;
        if(mode=="retire_direction")failDirection=true;
        if(mode=="retire_write")failSetAt=setCalls+1;
        if(mode=="retire_drift")cfg.pins[0]=5;
        if(mode=="retire_disabled")cfg.enabled=false;
        if(mode=="retire_missing")pm.runtimeStates.erase("control");
        pm.retireControlOutputs();
        if(failed) {
            assert(pm.controlOutputIsClaimed("control"));
            if(mode=="retire_drift"||mode=="retire_disabled"||mode=="retire_missing")assert(commanded.empty());
            else assert((commanded==std::vector<int>{1}));
            if(mode!="retire_missing") {
                assert(state.controlOutputRetiring && state.controlOutputFault && state.controlOutputRetirementFailed);
                assert(state.controlOutputRetirementAttempts==1);
            }
            const int previous=setCalls;
            pm.retireControlOutputs();controlNow=static_cast<uint32_t>(controlNow+999u);pm.retireControlOutputs();
            assert(setCalls==previous && pm.controlOutputIsClaimed("control"));
            forcedRead=-1;failDirection=false;cfg.pins[0]=4;cfg.enabled=true;
            if(mode=="retire_missing")pm.runtimeStates["control"]=PeripheralRuntimeState{};
            controlNow=static_cast<uint32_t>(controlNow+1u);pm.retireControlOutputs();
            assert(!pm.controlOutputIsClaimed("control") && levels[4]==1);
            auto& restored=pm.runtimeStates["control"];
            assert(!restored.controlOutputRetiring && !restored.controlOutputFault && !restored.controlOutputClaimed);
            if(mode!="retire_missing")assert(restored.controlOutputRetirementFailed && restored.controlOutputRetirementAttempts==2);
        } else {
            assert((commanded==std::vector<int>{1}) && !pm.controlOutputIsClaimed("control"));
            assert(state.controlOutputRetirementAttempts==(mode=="retire_saturation"?UINT32_MAX:1u));
            assert(!state.controlOutputRetiring && !state.controlOutputRetirementFailed);
        }
        const int previous=setCalls;pm.retireControlOutputs();assert(setCalls==previous);
        ControlOutput next;assert(next.begin("control") && next.owned() && next.end());return 0;
    }
    if(mode=="destructor_quarantine") {
        auto* first=new ControlOutput;assert(first->begin("control"));
        controlBusy=true;delete first;controlBusy=false;
        const int previous=setCalls;ControlOutput next;
        assert(!next.begin("control") && pm.controlOutputIsClaimed("control"));
        assert(state.controlOutputClaimed && setCalls==previous);return 0;
    }
    if(mode=="reset_adapter") {
        FastBee::MCP23017NativeReset reset;
        assert(reset.begin("control") && reset.owned() && reset.pin()==4);
        commanded.clear();assert(reset.pulse() && (commanded==std::vector<int>{0,1}));
        FastBee::MCP23017ResetLease& lease=reset;
        assert(lease.release() && !state.controlOutputClaimed);return 0;
    }
    if(mode=="gate_adapter") {
        FastBee::PCA9685NativeGate gate;
        assert(gate.begin("control") && gate.owned() && gate.disabled());
        assert(gate.enable() && !gate.disabled());assert(gate.disable() && gate.disabled());
        FastBee::PCA9685OutputLease& lease=gate;
        assert(lease.release() && !gate.owned() && !gate.disabled() && !state.controlOutputClaimed);return 0;
    }
    if(mode=="disabled")cfg.enabled=false;
    if(mode=="uninitialized")state.status=PeripheralStatus::PERIPHERAL_ERROR;
    if(mode=="suspended")state.memorySuspended=true;
    if(mode=="expander")cfg.expander=true;
    if(mode=="passive")cfg.params.gpio.passive=true;
    if(mode=="inverted")cfg.params.gpio.invertLogic=true;
    if(mode=="initial_low")cfg.params.gpio.initialState=GPIOState::STATE_LOW;
    if(mode=="wrong_type")cfg.type=PeripheralType::GPIO_PWM_OUTPUT;
    if(mode=="reserved")pm.reserved=true;
    if(mode=="invalid")pm.valid=false;
    if(mode=="input_only")pm.inputOnly=true;
    if(mode=="conflict")pm.conflicting=true;
    if(mode=="ticker")pm.actionTickers["control"]=1;
    if(mode=="ordinary_owner")pm.ordinaryClaimed=true;
    if(mode=="busy")controlBusy=true;
    const bool reject=mode=="disabled"||mode=="uninitialized"||mode=="suspended"||mode=="expander"||
        mode=="passive"||mode=="inverted"||mode=="initial_low"||mode=="wrong_type"||mode=="reserved"||
        mode=="invalid"||mode=="input_only"||mode=="conflict"||mode=="ticker"||mode=="busy"||mode=="ordinary_owner";
    ControlOutput output;
    if(reject) {
        assert(!output.begin("control") && setCalls==0 && directionCalls==0);
        controlBusy=false;assert(!state.controlOutputClaimed);return 0;
    }
    if(mode=="init_failure") {
        failDirection=true;assert(!output.begin("control"));
        assert(state.controlOutputClaimed && state.controlOutputFault && !output.owned());
        const int previous=setCalls;assert(!output.low() && !output.pulseReset() && setCalls==previous);
        failDirection=false;assert(output.end() && !state.controlOutputClaimed);return 0;
    }
    assert(output.begin("control") && output.pin()==4 && output.owned() && output.isHigh());
    assert(pm.controlOutputIsClaimed("control") && pm.controlOutputPinIsClaimed(4));
    assert(!pm.controlOutputIsClaimed("other") && !pm.controlOutputPinIsClaimed(5));
    if(mode=="pulse") {
        commanded.clear();waitedUs=0;
        assert(output.pulseReset() && (commanded==std::vector<int>{0,1}) && waitedUs==4);
        assert(output.isHigh());
    } else if(mode=="owner") {
        ControlOutput other;const int previous=setCalls;assert(!other.begin("control"));
        int wrong,pin=-1;assert(!pm.controlOutput("control",&wrong,Command::Low,pin));
        assert(setCalls==previous && output.owned());
    } else if(mode=="pulse_failure") {
        commanded.clear();failSetAt=setCalls+1;
        assert(!output.pulseReset() && (commanded==std::vector<int>{0,1}));
        assert(levels[4]==1 && state.controlOutputFault && !output.owned());
        assert(output.high() && output.owned());
    } else if(mode=="release_failure") {
        forcedRead=0;assert(!output.end() && state.controlOutputClaimed && state.controlOutputFault);
        ControlOutput other;assert(!other.begin("control"));forcedRead=-1;
    } else if(mode=="readback") {
        forcedRead=1;assert(!output.low() && state.controlOutputFault);
        assert(state.state.gpio.currentState==GPIOState::STATE_UNDEFINED);forcedRead=-1;
    } else if(mode=="capacity") {
        ControlOutput extra[4];
        for(int i=0;i<4;++i) {
            String id=String("extra")+String(i);pm.peripherals[id]=PeripheralConfig{};
            pm.peripherals[id].pins[0]=static_cast<uint8_t>(5+i);pm.runtimeStates[id]=PeripheralRuntimeState{};
            assert(extra[i].begin(id)==(i<3));
        }
    } else if(mode=="thread_busy") {
        std::promise<void> entered,release;auto ready=release.get_future();
        std::thread other([&]{controlMutex.lock();entered.set_value();ready.wait();controlMutex.unlock();});
        entered.get_future().wait();const int previous=setCalls;
        assert(!output.low() && !output.end() && pm.controlOutputIsClaimed());
        assert(setCalls==previous);release.set_value();other.join();assert(output.owned());
    } else if(mode=="drift") {
        cfg.pins[0]=5;const int previous=setCalls;assert(!output.low() && !output.pulseReset());
        assert(setCalls==previous);cfg.pins[0]=4;
    } else if(mode=="unknown_command") {
        const int previous=setCalls;int pin=-1;
        assert(!pm.controlOutput("control",&output,static_cast<Command>(255),pin));assert(setCalls==previous);
    } else { assert(mode=="normal");assert(output.low() && !output.isHigh() && output.high()); }
    assert(output.end() && !state.controlOutputClaimed && !pm.controlOutputIsClaimed() && levels[4]==1);
    assert(controlDepth==0);
}
