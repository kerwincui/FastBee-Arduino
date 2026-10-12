#include "peripherals/MCP23017Provider.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <string>
#include <thread>

using namespace FastBee;
using P = MCP23017Provider;
using R = IoExpanderResult;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#c); std::exit(86); } } while(0)

struct ControlTrace {
    bool owned = true, resetOk = true, releaseOk = true;
    int pin = 4;
    unsigned pulses = 0, releases = 0, destroyed = 0;
};
struct Reset final : MCP23017ResetLease {
    ControlTrace& trace;
    explicit Reset(ControlTrace& t) : trace(t) {}
    ~Reset() override { ++trace.destroyed; }
    int pin() const override { return trace.pin; }
    bool owned() const override { return trace.owned; }
    bool pulse() override {
        ++trace.pulses;
        CHECK(I2CBusManager::instance().clientCount() == 1);
        CHECK(Wire.transmissions == 0); // no register writes before explicit reset
        if (!trace.resetOk) return false;
        Wire.registers.fill(0); Wire.registers[0] = Wire.registers[1] = 255;
        return true;
    }
    bool release() override {
        ++trace.releases;
        CHECK(I2CBusManager::instance().clientCount() == 0); // bus before RESET
        if (!trace.releaseOk) return false;
        trace.owned = false; return true;
    }
};
static std::unique_ptr<P> make(ControlTrace& trace) {
    return std::make_unique<P>(21, 22, 100000, 0x20, std::make_unique<Reset>(trace));
}
static bool begin(P& p, uint32_t now = 100) { return p.begin(0x8181, 0xfffe, 20, now, 0x7e7e); }
static void arguments() {
    ControlTrace t; auto p = make(t);
    CHECK(!p->begin(0x0080,0xffff,20,0)); // B7 not explicitly owned
    CHECK(!p->begin(0x8080,0xffff,20,0,0x8000)); // input/output overlap
    CHECK(!p->begin(0x8080,0xffff,19,0));
    CHECK(!p->begin(0x8080,0xffff,1001,0));
    CHECK(Wire.transmissions == 0 && t.pulses == 0);
    CHECK(begin(*p));
    const auto before = Wire.transmissions;
    CHECK(!begin(*p) && p->error() == P::Error::AlreadyAttempted && t.pulses == 1);
    CHECK(Wire.transmissions == before && p->healthy(100));
    for (unsigned channel = 16; channel < 256; ++channel) {
        bool high = true;
        CHECK(!p->writeChannel(static_cast<uint8_t>(channel),false,100));
        CHECK(!p->readChannel(static_cast<uint8_t>(channel),true,100,high) && high);
    }
    CHECK(Wire.transmissions == before && p->failures() == 0);
    CHECK(p->configure(0x8080,0xffff,100,0) && p->poll(100));
    bool high = true;
    CHECK(!p->readChannel(0,false,100,high) && high); // unclaimed input is not a sample
    CHECK(p->shutdown(0xffff) == R::Ok);
}
static void normal() {
    ControlTrace t; auto p = make(t); CHECK(begin(*p));
    CHECK(Wire.registers[0] == 0x7e && Wire.registers[1] == 0x7e);
    CHECK(Wire.registers[0x14] == 0x80 && Wire.registers[0x15] == 0x81);
    bool high = false;
    CHECK(p->readChannel(15,true,100,high) && high);
    CHECK(!p->readChannel(15,false,100,high));
    const auto before = Wire.transmissions;
    for (int i=0; i<1000; ++i) CHECK(p->initialized() && p->healthy(100));
    CHECK(Wire.transmissions == before);
    CHECK(p->writeChannel(15,false,100));
    CHECK(Wire.registers[0x15] == 1 && !p->healthy(100));
    CHECK(!p->readChannel(15,true,100,high)); // no stale combined snapshot
    CHECK(p->poll(100) && p->generation() == 2);
    CHECK(p->readChannel(15,true,100,high) && !high);
    CHECK(p->readChannel(7,true,100,high) && high);
    CHECK(p->shutdown(0x8080) == R::Ok);
    CHECK(Wire.registers[0x14] == 0x80 && Wire.registers[0x15] == 0x80);
    CHECK(t.releases == 1 && !t.owned && p->closing());
    const auto closed = Wire.transmissions;
    CHECK(p->shutdown(0) == R::Ok && t.releases == 1);
    CHECK(!p->poll(1000) && !p->configure(0x8080,0,1000));
    CHECK(!p->writeChannel(7,true,1000) && !begin(*p));
    CHECK(Wire.transmissions == closed && t.pulses == 1);
}
static void freshness(uint32_t start) {
    ControlTrace t; auto p = make(t); CHECK(begin(*p,start));
    const auto before = Wire.transmissions;
    CHECK(p->poll(start+19) && Wire.transmissions == before);
    CHECK(p->healthy(start+60) && !p->healthy(start+61));
    bool high = true;
    CHECK(!p->readChannel(1,false,start+61,high) && high);
    CHECK(!p->writeChannel(7,false,start+61) && Wire.transmissions == before);
    CHECK(p->poll(start+61) && p->generation() == 2 && t.pulses == 1);
    CHECK(p->shutdown(0xffff) == R::Ok);
}
static void configure() {
    ControlTrace t; auto p=make(t); CHECK(begin(*p));
    CHECK(p->writeChannel(15,false,100)); CHECK(p->poll(100));
    const auto before=Wire.transmissions;
    CHECK(!p->configure(0x80,0xffff,101));
    CHECK(p->healthy(101) && Wire.transmissions==before && p->failures()==0);
    CHECK(p->configure(0x8282,0xffff,101,0x7d7d));
    CHECK(Wire.registers[0x14]==0x82 && Wire.registers[0x15]==2);
    CHECK(!p->healthy(101) && p->poll(101));
    bool high=false; CHECK(p->readChannel(9,true,101,high) && high);
    CHECK(!p->readChannel(8,true,101,high));
    CHECK(t.pulses==1 && p->shutdown(0xffff)==R::Ok);
}
static void withBusyBus(const std::function<void()>& work) {
    auto& bus=I2CBusManager::instance();
    std::promise<void> entered, release; auto signal=release.get_future();
    std::thread holder([&]{CHECK(bus.lock());entered.set_value();signal.wait();bus.unlock();});
    entered.get_future().wait();
    const auto start=std::chrono::steady_clock::now();
    work();
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<100);
    release.set_value();holder.join();
}
static void busy(const std::string& mode) {
    ControlTrace t; auto p=make(t);
    if(mode!="begin_busy") CHECK(begin(*p));
    const auto before=Wire.transmissions;
    withBusyBus([&] {
        if(mode=="begin_busy") { CHECK(!begin(*p) && t.pulses==0); }
        else if(mode=="shutdown_busy") {
            CHECK(p->shutdown(0x8080)==R::Busy && p->closing());
            CHECK(t.releases==0 && !p->initialized());
        } else {
            CHECK(!p->poll(120) && p->error()==P::Error::Busy);
            CHECK(!p->configure(0x8080,0,120));
            CHECK(!p->writeChannel(7,false,120));
            CHECK(p->initialized() && p->failures()==0);
        }
        CHECK(Wire.transmissions==before);
    });
    if(mode=="begin_busy") CHECK(begin(*p));
    CHECK(p->shutdown(0xffff)==R::Ok);
    if(mode=="shutdown_busy") CHECK(Wire.registers[0x14]==0x80 && Wire.registers[0x15]==0x80);
    CHECK(t.pulses==1 && t.releases==1);
}
static void resetFailure(const std::string& mode) {
    ControlTrace t; auto p=make(t);
    if(mode=="reset_not_owned") {
        t.owned=false;CHECK(!begin(*p) && t.pulses==0);
    } else if(mode=="reset_failed") {
        t.resetOk=false; CHECK(!begin(*p) && t.pulses==1);
    } else {
        CHECK(begin(*p));
        if(mode=="reset_lost") t.owned=false;
        else t.pin=5;
        const auto before=Wire.transmissions;
        CHECK(!p->writeChannel(7,false,100));
        CHECK(Wire.transmissions==before && p->error()==P::Error::ResetOwnership);
    }
    const auto before=Wire.transmissions;
    t.owned=true;t.pin=4;t.resetOk=true;
    CHECK(!p->poll(1000) && !begin(*p) && !p->initialized());
    CHECK(p->shutdown(0xffff)==R::Error && t.pulses==(mode=="reset_not_owned"?0U:1U));
    CHECK(Wire.transmissions==before && p->failures()==1);
}
static void collision() {
    auto& bus=I2CBusManager::instance();String error;int other;
    CHECK(bus.acquire(&other,21,22,100000,error) && bus.claimAddress(&other,0x20,error));
    ControlTrace t;auto p=make(t);
    CHECK(!begin(*p) && t.pulses==0 && Wire.transmissions==0);
    CHECK(bus.ownsAddress(&other,0x20));CHECK(bus.release(&other));
    CHECK(!begin(*p) && p->shutdown(0xffff)==R::Error && t.pulses==0);
}
static void fault(const std::string& mode) {
    ControlTrace t;auto p=make(t);CHECK(begin(*p));
    Wire.writeLimit=1;
    if(mode=="shutdown_fault") CHECK(p->shutdown(0)==R::Error);
    else CHECK(!p->writeChannel(7,false,100) && !p->initialized());
    const auto before=Wire.transmissions;
    Wire.writeLimit=255;
    CHECK(!p->poll(1000) && !begin(*p));
    CHECK(p->shutdown(0xffff)==R::Error);
    CHECK(Wire.transmissions==before && t.pulses==1 && p->failures()==1);
}
static void sampleFault(bool drift) {
    ControlTrace t;auto p=make(t);CHECK(begin(*p));
    if(drift) Wire.registers[0x15]^=1;
    else Wire.ack=false;
    CHECK(!p->poll(120) && !p->initialized());
    CHECK(p->sessionError()==(drift?MCP23017PortSession::Error::LatchLost:MCP23017PortSession::Error::Transport));
    const auto before=Wire.transmissions;Wire.ack=true;
    const auto registers=Wire.registers;
    CHECK(!p->poll(10000) && !p->writeChannel(15,false,120));
    CHECK(p->shutdown(0)==R::Error && Wire.transmissions==before);
    CHECK(Wire.registers==registers && t.pulses==1 && p->failures()==1);
}
static void releaseRetry() {
    ControlTrace t;auto p=make(t);CHECK(begin(*p));
    t.releaseOk=false;
    CHECK(p->shutdown(0x8080)==R::Busy && p->error()==P::Error::ControlRelease);
    CHECK(I2CBusManager::instance().clientCount()==0 && t.owned);
    const auto before=Wire.transmissions;
    CHECK(p->shutdown(0)==R::Busy && Wire.transmissions==before);
    CHECK(!p->poll(1000) && !p->writeChannel(15,true,1000));
    t.releaseOk=true;
    CHECK(p->shutdown(0)==R::Ok && t.releases==3 && t.pulses==1);
    CHECK(Wire.transmissions==before && Wire.registers[0x15]==0x80);
}
static void cachedOwnership(bool changedPin) {
    ControlTrace t; auto p=make(t); CHECK(begin(*p));
    const auto before=Wire.transmissions;
    const auto generation=p->generation();
    if(changedPin) t.pin=5; else t.owned=false;
    bool high=true;
    CHECK(!p->healthy(100));
    CHECK(!p->readChannel(1,false,100,high) && high);
    CHECK(!p->readChannel(7,true,100,high) && high);
    // A cached observation makes no bus transaction or reset attempt, and
    // leaves fault recording to the next mutating/polling boundary.
    CHECK(Wire.transmissions==before && t.pulses==1 && p->generation()==generation);
    CHECK(!p->poll(101) && p->error()==P::Error::ResetOwnership);
    t.pin=4; t.owned=true;
    CHECK(!p->healthy(101) && !p->poll(200));
    CHECK(p->shutdown(0xffff)==R::Error && Wire.transmissions==before);
}
static void destructor() {
    ControlTrace t;std::unique_ptr<DigitalIoProvider> p=make(t);
    CHECK(p->begin(0x8080,0xffff,20,100));
    const auto before=Wire.transmissions;p.reset();
    CHECK(I2CBusManager::instance().clientCount()==0 && t.destroyed==1);
    CHECK(Wire.transmissions==before && t.pulses==1); // no implicit safe write/reset
}
int main(int argc,char** argv) {
    CHECK(argc==2);const std::string mode=argv[1];
    if(mode=="arguments")arguments();else if(mode=="normal")normal();
    else if(mode=="freshness")freshness(100);else if(mode=="wrap")freshness(UINT32_MAX-10);
    else if(mode=="configure")configure();else if(mode=="collision")collision();
    else if(mode=="busy" || mode=="begin_busy" || mode=="shutdown_busy")busy(mode);
    else if(mode=="cached_reset_lost" || mode=="cached_pin_changed")cachedOwnership(mode=="cached_pin_changed");
    else if(mode.find("reset_")==0)resetFailure(mode);
    else if(mode=="transport_fault" || mode=="shutdown_fault")fault(mode);
    else if(mode=="sample_fault" || mode=="drift")sampleFault(mode=="drift");
    else if(mode=="release_retry")releaseRetry();else if(mode=="destructor")destructor();
    else CHECK(false);
    std::printf("%s PASS\n",mode.c_str());
}
