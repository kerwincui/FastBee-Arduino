#include "core/ManagedPinIo.h"
#include "core/ChipConfig.h"
#include "managed_pin_mock_state.h"
#include <cstring>
#include <cstdlib>
#include <new>
#include <string>
using namespace FastBee;
// Replace the actual host allocation seam, not an implementation-only flag.
// Other throwing allocations (e.g. std::string) remain untouched.
static bool failGpioAllocation = false;
static size_t gpioAllocationAttempts = 0, gpioAllocations = 0, gpioFrees = 0;
static void* gpioAllocationsLive[GpioResourceSnapshot::CAPACITY]{};
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    assert(size == sizeof(GpioResourceSnapshot::Lease));
    ++gpioAllocationAttempts;
    if (failGpioAllocation) return nullptr;
    void* memory = std::malloc(size);
    if (!memory) return nullptr;
    for (auto& entry : gpioAllocationsLive) if (!entry) {
        entry = memory;
        ++gpioAllocations;
        return memory;
    }
    assert(false && "GPIO allocation exceeded unchanged capacity");
    std::free(memory);
    return nullptr;
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    if (!memory) return;
    for (auto& entry : gpioAllocationsLive) if (entry == memory) {
        entry = nullptr;
        ++gpioFrees;
        std::free(memory);
        return;
    }
    assert(false && "untracked GPIO release");
}
static void handler(void*) {}
static GpioResourceSnapshot gpio(ManagedPinIo& io) { GpioResourceSnapshot out;io.snapshot(out);return out; }
static DacResourceSnapshot dac(ManagedPinIo& io) { DacResourceSnapshot out;io.snapshot(out);return out; }
int main(int argc,char** argv) {
    assert(argc==2);const std::string scenario=argv[1];ManagedPinIo io;
    if (scenario == "lazy_empty") {
        failGpioAllocation = true;
        for (int i = 0; i < 1000; ++i) {
            GpioResourceSnapshot::Lease observed;
            bool high = true;
            assert(!io.hasGpio(18) && !io.conflicts("a", false, 18));
            assert(!io.observeGpio(18, observed) && !io.interruptCurrent("a", 18));
            assert(!io.read("a", 18, high) && !high);
            assert(!io.write("a", false, 18, true));
            assert(!io.attach("a", 18, handler, nullptr, GPIO_INTR_POSEDGE));
            assert(io.detach("a", 18) && io.release("a", false, 18));
            assert(gpio(io).count == 0 && dac(io).count == 0);
        }
        assert(gpioAllocationAttempts == 0 && allCalls() == 0);
    } else if (scenario == "lazy_oom") {
        failGpioAllocation = true;
        assert(!io.configure("", false, 18, ManagedGpioMode::Output, true));
        buses[18] = ESP32_BUS_TYPE_LEDC;
        assert(!io.configure("a", false, 18, ManagedGpioMode::Output, true));
        assert(gpioAllocationAttempts == 0 && allCalls() == count(Op::Periman));
        buses[18] = ESP32_BUS_TYPE_INIT;
        assert(!io.configure("a", false, 18, ManagedGpioMode::Output, true));
        assert(gpioAllocationAttempts == 1 && gpioAllocations == 0 && gpio(io).count == 0 && allCalls() == count(Op::Periman));
        failGpioAllocation = false;
        assert(io.configure("a", false, 18, ManagedGpioMode::Output, true));
        const auto attempts = gpioAllocationAttempts;
        failGpioAllocation = true;
        assert(io.configure("a", false, 18, ManagedGpioMode::Output, false));
        assert(!io.configure("different", false, 18, ManagedGpioMode::Input, false));
        assert(gpioAllocationAttempts == attempts && gpioAllocations == 1);
        assert(io.release("a", false, 18) && gpioFrees == 1);
        const auto calls = allCalls() - count(Op::Periman);
        assert(!io.configure("replacement", false, 18, ManagedGpioMode::Input, false));
        assert(gpioAllocationAttempts == attempts + 1 && allCalls() - count(Op::Periman) == calls && !gpio(io).count);
    } else if (scenario == "lazy_reuse") {
        for (unsigned cycle = 0; cycle < 128; ++cycle) {
            const std::string id = "owner-" + std::to_string(cycle);
            assert(io.configure(id.c_str(), false, 18, ManagedGpioMode::PullUp, false));
            const auto copy = gpio(io);
            assert(gpioAllocations == cycle + 1 && gpioFrees == cycle);
            assert(io.attach(id.c_str(), 18, handler, reinterpret_cast<void*>(uintptr_t(18)), GPIO_INTR_POSEDGE));
            assert(io.detach(id.c_str(), 18) && io.hasGpio(18));
            assert(gpioFrees == cycle && !io.interruptCurrent(id.c_str(), 18));
            assert(io.configure(id.c_str(), false, 18, ManagedGpioMode::PullUp, false));
            assert(gpioAllocations == cycle + 1);
            assert(io.release(id.c_str(), false, 18) && !io.hasGpio(18));
            assert(gpioFrees == cycle + 1 && !handlers[18]);
            assert(std::string(copy.leases[0].consumer.id) == id);
        }
        assert(gpioAllocations == gpioFrees && gpioAllocationAttempts == 128);
    } else if (scenario == "lazy_retained") {
        assert(io.configure("input", false, 18, ManagedGpioMode::Input, false));
        void* address = gpioAllocationsLive[0];
        failure = Op::Add;
        assert(!io.attach("input", 18, handler, reinterpret_cast<void*>(uintptr_t(18)), GPIO_INTR_POSEDGE));
        assert(gpio(io).leases[0].interruptCleanupPending && gpio(io).leases[0].fault);
        failure = Op::Remove;
        assert(!io.release("input", false, 18));
        assert(gpioAllocationsLive[0] == address && gpioFrees == 0);
        assert(!io.configure("other", false, 18, ManagedGpioMode::Input, false));
        assert(gpioAllocations == 1);
        failure = Op::Count;
        assert(io.detach("input", 18) && io.hasGpio(18) && gpioFrees == 0);
        failure = Op::Config;
        assert(!io.release("input", false, 18));
        assert(gpio(io).leases[0].cleanupPending && gpio(io).leases[0].fault);
        assert(gpioAllocationsLive[0] == address && gpioFrees == 0);
        failure = Op::Count;
        assert(io.release("input", false, 18) && gpioFrees == 1 && !gpio(io).count);
        assert(io.configure("replacement", false, 18, ManagedGpioMode::Input, false));
        assert(gpioAllocations == 2 && std::string(gpio(io).leases[0].consumer.id) == "replacement");
        assert(io.release("replacement", false, 18) && gpioFrees == 2);
    } else if (scenario == "inactive_high_release" || scenario == "inactive_high_release_failure") {
        assert(io.configure("rf", false, 18, ManagedGpioMode::Output, true));
        assert(io.write("rf", false, 18, false));
        if (scenario == "inactive_high_release_failure") failure = Op::Config;
        const bool ok = io.release("rf", false, 18, true);
        assert(ok == (scenario == "inactive_high_release") && levels[18] == 1);
        if (!ok) {
            assert(gpio(io).leases[0].cleanupPending && gpio(io).leases[0].fault);
            failure = Op::Count;
            assert(io.release("rf", false, 18, true) && levels[18] == 1);
        }
        assert(!gpio(io).count && lastConfig.mode == GPIO_MODE_INPUT);
    } else if(scenario=="admission") {
        assert(!io.hasDac(255) && !io.hasGpio(255));
        assert(!io.configure(nullptr,false,18,ManagedGpioMode::Input,false));
        assert(!io.configure("",false,18,ManagedGpioMode::Input,false));
        assert(!io.configure("named",true,18,ManagedGpioMode::Input,false));
        assert(!io.configure(std::string(41,'x').c_str(),false,18,ManagedGpioMode::Input,false));
        assert(!io.configure("a",false,255,ManagedGpioMode::Input,false));
        assert(!io.configure("a",false,CHIP_MAX_GPIO+1,ManagedGpioMode::Input,false));
        assert(allCalls()==0);
        buses[18]=ESP32_BUS_TYPE_GPIO;
        assert(!io.configure("a",false,18,ManagedGpioMode::Input,false));
        assert(gpio(io).count==0 && count(Op::Config)==0);
    } else if(scenario=="capacity") {
        for(uint8_t pin=0;pin<=CHIP_MAX_GPIO;++pin) {
            std::string id="gpio-"+std::to_string(pin);
            assert(io.configure(id.c_str(),false,pin,ManagedGpioMode::Input,false));
        }
        auto out=gpio(io);assert(out.count==CHIP_MAX_GPIO+1);
        for(uint8_t pin=0;pin<=CHIP_MAX_GPIO;++pin)
            assert(std::string(out.leases[pin].consumer.id)=="gpio-"+std::to_string(pin));
        assert(!io.release("different",false,0));
        assert(io.release("gpio-0",false,0));
        assert(io.configure("replacement",false,0,ManagedGpioMode::Input,false));
        assert(std::string(out.leases[0].consumer.id)=="gpio-0");
    } else if(scenario=="gpio_lifecycle") {
        assert(io.configure("a",false,18,ManagedGpioMode::Output,true));
        assert(levels[18]==1 && count(Op::Level)==1 && count(Op::Config)==1);
        for(int i=0;i<100;++i)assert(io.configure("a",false,18,ManagedGpioMode::Output,i%2));
        assert(count(Op::Config)==1 && !io.configure("b",false,18,ManagedGpioMode::Output,false));
        bool high=true;assert(!io.read("a",18,high) && !high);
        assert(io.release("a",false,18));assert(gpio(io).count==0 && levels[18]==0);
        assert(lastConfig.mode==GPIO_MODE_INPUT && lastConfig.intr_type==GPIO_INTR_DISABLE);
        assert(io.configure("a",false,18,ManagedGpioMode::PullUp,false));
        assert(lastConfig.pull_up_en==GPIO_PULLUP_ENABLE && io.read("a",18,high) && !high);
        levels[18]=1;assert(io.read("a",18,high) && high);
    } else if(scenario=="setup_level_failure" || scenario=="setup_config_failure") {
        failure=scenario=="setup_level_failure"?Op::Level:Op::Config;
        assert(!io.configure("a",false,18,ManagedGpioMode::Output,true));
        assert(gpio(io).count==1 && gpio(io).leases[0].fault && !gpio(io).leases[0].cleanupPending);
        failure=Op::Count;const auto before=allCalls();
        assert(!io.write("a",false,18,false) && allCalls()==before);
        assert(!io.configure("b",false,18,ManagedGpioMode::Input,false));
        assert(io.release("a",false,18));assert(gpio(io).count==0);
    } else if(scenario=="write_failure") {
        assert(io.configure("a",false,18,ManagedGpioMode::Output,true));
        failure=Op::Level;assert(!io.write("a",false,18,false));
        assert(gpio(io).leases[0].fault);failure=Op::Count;
        assert(!io.configure("a",false,18,ManagedGpioMode::Output,true));
        assert(io.release("a",false,18));
    } else if(scenario=="release_level_failure" || scenario=="release_config_failure") {
        assert(io.configure("a",false,18,ManagedGpioMode::Output,true));
        failure=scenario=="release_level_failure"?Op::Level:Op::Config;
        assert(!io.release("a",false,18));
        assert(gpio(io).leases[0].fault && gpio(io).leases[0].cleanupPending);
        failure=Op::Count;assert(!io.configure("b",false,18,ManagedGpioMode::Input,false));
        assert(io.release("a",false,18) && gpio(io).count==0);
    } else if(scenario=="foreign") {
        assert(io.configure("a",false,18,ManagedGpioMode::Output,true));
        buses[18]=ESP32_BUS_TYPE_LEDC;const auto before=count(Op::Level);
        assert(!io.write("a",false,18,false) && gpio(io).leases[0].fault);
        assert(!io.release("a",false,18) && gpio(io).leases[0].cleanupPending);
        assert(count(Op::Level)==before);buses[18]=ESP32_BUS_TYPE_INIT;
        assert(!io.write("a",false,18,true));assert(io.release("a",false,18));
    } else if(scenario=="irq" || scenario=="passive") {
        assert(io.configure("input",false,18,ManagedGpioMode::PullDown,false));
        assert(io.attach("input",18,handler,nullptr,GPIO_INTR_ANYEDGE));
        assert(io.interruptCurrent("input",18) && gpio(io).leases[0].interruptAttached);
        assert(io.configure("input",false,18,ManagedGpioMode::PullDown,false));
        assert(io.attach("input",18,handler,nullptr,GPIO_INTR_ANYEDGE));
        assert(count(Op::Add)==1 && count(Op::Config)==1);
        assert(!io.attach("input",18,handler,nullptr,GPIO_INTR_POSEDGE));
        const auto before=allCalls();for(int i=0;i<1000;++i){gpio(io);dac(io);}
        assert(allCalls()==before);assert(io.detach("input",18));
        assert(!io.interruptCurrent("input",18));assert(io.release("input",false,18));
    } else if(scenario=="irq_install_failure" || scenario=="irq_type_failure" || scenario=="irq_add_failure") {
        assert(io.configure("input",false,18,ManagedGpioMode::Input,false));
        failure=scenario=="irq_install_failure"?Op::Install:scenario=="irq_type_failure"?Op::Type:Op::Add;
        assert(!io.attach("input",18,handler,nullptr,GPIO_INTR_POSEDGE));
        const auto out=gpio(io);assert(out.count==1 && out.leases[0].fault && !out.leases[0].interruptAttached);
        assert(out.leases[0].interruptCleanupPending==(scenario=="irq_add_failure"));
        assert(!io.interruptCurrent("input",18));failure=Op::Count;
        assert(io.release("input",false,18) && !handlers[18]);
    } else if(scenario=="irq_disable_failure" || scenario=="irq_remove_failure" || scenario=="irq_clear_failure") {
        assert(io.configure("input",false,18,ManagedGpioMode::Input,false));
        assert(io.attach("input",18,handler,nullptr,GPIO_INTR_POSEDGE));
        failure=scenario=="irq_disable_failure"?Op::Disable:scenario=="irq_remove_failure"?Op::Remove:Op::Type;
        assert(!io.detach("input",18));assert(gpio(io).leases[0].fault && gpio(io).leases[0].cleanupPending);
        const auto removes=count(Op::Remove);failure=Op::Count;
        assert(io.detach("input",18) && !handlers[18]);
        if(scenario=="irq_clear_failure")assert(count(Op::Remove)==removes);
        assert(io.release("input",false,18) && gpio(io).count==0);
    } else if(scenario=="dac_supported") {
        assert(dac(io).supported==(CHIP_HAS_DAC!=0));
#if CHIP_HAS_DAC
        assert(!io.writeDac("d",false,24,3));assert(io.writeDac("d",false,25,123));
        assert(io.writeDac("e",false,26,42));assert(dac(io).count==2 && count(Op::DacNew)==2);
        assert(!io.configure("d",false,25,ManagedGpioMode::Input,false));
        assert(!io.releaseDac("e",false,25));
        for(unsigned value=0;value<256;++value)assert(io.writeDac("d",false,25,value));
        assert(count(Op::DacNew)==2);const auto before=allCalls();
        for(int i=0;i<1000;++i) { dac(io); }
        assert(before==allCalls());
        assert(io.releaseDac("d",false,25) && io.releaseDac("e",false,26));
        assert(dac(io).count==0 && !dacHandles[0].alive && !dacHandles[1].alive);
#else
        assert(!io.writeDac("d",false,25,123));assert(dac(io).count==0 && allCalls()==0);
#endif
    } else if(scenario=="dac_new_failure" || scenario=="dac_partial_new_failure" || scenario=="dac_write_failure" ||
              scenario=="dac_zero_failure" || scenario=="dac_delete_failure") {
#if CHIP_HAS_DAC
        if(scenario=="dac_zero_failure" || scenario=="dac_delete_failure") {
            assert(io.writeDac("d",false,25,123));
            failure=scenario=="dac_zero_failure"?Op::DacWrite:Op::DacDelete;
            assert(!io.releaseDac("d",false,25));assert(dac(io).leases[0].cleanupPending);
        } else {
            partialAllocation=scenario=="dac_partial_new_failure";
            failure=scenario=="dac_write_failure"?Op::DacWrite:Op::DacNew;
            assert(!io.writeDac("d",false,25,123));
        }
        assert(dac(io).count==1 && dac(io).leases[0].fault);
        failure=Op::Count;assert(!io.writeDac("d",false,25,12));
        assert(!io.configure("new",false,25,ManagedGpioMode::Output,true));
        assert(io.releaseDac("d",false,25) && dac(io).count==0 && !dacHandles[0].alive);
#else
        assert(!io.writeDac("d",false,25,123) && !dac(io).supported && allCalls()==0);
#endif
    } else return 2;
}
