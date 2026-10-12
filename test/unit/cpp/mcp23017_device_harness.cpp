#include "peripherals/drivers/MCP23017Device.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
using namespace FastBee;
using D = MCP23017Device;
using S = MCP23017PortSession;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#c); std::exit(86); } } while(0)
static bool begin(D& d, unsigned addr = 0x20) {
    return d.begin(21,22,100000,addr,S::ResetEvidence::ConfirmedBank0Reset,
                   0x8181,0x7e7e,0x8080,0x0202,0x0404);
}
static void admission() {
    D d;
    for(unsigned addr : {0U,0x1fU,0x28U,0x120U}) CHECK(!begin(d,addr));
    CHECK(Wire.transmissions == 0 && Wire.starts == 0);
    CHECK(!d.begin(21,22,100000,0x20,S::ResetEvidence::Unknown,0x8080,0x7f7f,0,0,0));
    CHECK(Wire.starts == 0); CHECK(d.error() == D::Error::ResetRequired);
    CHECK(!d.begin(21,21,100000,0x20,S::ResetEvidence::ConfirmedBank0Reset,0x8080,0,0,0,0));
    CHECK(Wire.starts == 0);
}
static void collision() {
    auto& m = I2CBusManager::instance(); String error; int other;
    CHECK(m.acquire(&other,21,22,100000,error)); CHECK(m.claimAddress(&other,0x20,error));
    D d; CHECK(!begin(d)); CHECK(Wire.transmissions == 0);
    CHECK(m.clientCount()==1 && m.ownsAddress(&other,0x20)); CHECK(m.release(&other));
    CHECK(begin(d)); CHECK(m.clientCount()==1); CHECK(m.ownsAddress(&d,0x20));
    unsigned before = Wire.transmissions; CHECK(!begin(d)); CHECK(d.error()==D::Error::AlreadyOpen);
    CHECK(Wire.transmissions==before); CHECK(d.end()); CHECK(m.clientCount()==0);
}
static void normal() {
    D d; CHECK(begin(d)); CHECK(d.session().ready());
    CHECK(d.setOutputs(0x0101,0x0101)); CHECK(Wire.registers[0x14]==0x81);
    Wire.registers[0x12]=0x55; Wire.registers[0x13]=0xaa;
    CHECK(d.sampleInputs(123)); CHECK(d.session().sample().valid);
    CHECK(d.session().sample().inputs==(0xaa55&0x7e7e));
    auto before = Wire.transmissions; CHECK(d.end()); CHECK(Wire.transmissions==before);
    CHECK(Wire.registers[0x14]==0x81); // Release must not silently drive outputs.
    CHECK(!d.session().ready()); CHECK(I2CBusManager::instance().clientCount()==0);
}
static void read_fault(const std::string& mode) {
    D d; CHECK(begin(d)); CHECK(d.sampleInputs(10)); unsigned requests = Wire.requests;
    if(mode=="pointer_short") Wire.writeLimit=0;
    if(mode=="nack") Wire.ack=false;
    if(mode=="short_read") Wire.shortRead=true;
    if(mode=="negative") Wire.nextByte=-1;
    if(mode=="wide") Wire.nextByte=256;
    if(mode=="empty") Wire.emptyRead=true;
    if(mode=="extra") Wire.extraRead=true;
    if(mode=="infinite") Wire.infinite=true;
    CHECK(!d.sampleInputs(20)); CHECK(!d.session().ready()); CHECK(!d.session().sample().valid);
    if(mode=="pointer_short") {CHECK(Wire.requests==requests); CHECK(Wire.stops.back());}
    if(mode=="nack") CHECK(Wire.requests==requests);
    if(mode=="infinite") CHECK(Wire.reads<=400); // bounded drain rather than infinite loop
    CHECK(d.end()); CHECK(I2CBusManager::instance().clientCount()==0);
}
static void write_short() {
    D d; CHECK(begin(d)); Wire.writeLimit=1;
    CHECK(!d.setOutputs(1,1)); CHECK(!d.session().ready()); CHECK(Wire.stops.back());
    CHECK(Wire.registers[0x14]==0x80); CHECK(d.end());
}
static void lease_lost() {
    D d; CHECK(begin(d)); CHECK(I2CBusManager::instance().release(&d));
    auto before = Wire.transmissions; CHECK(!d.setOutputs(1,1)); CHECK(Wire.transmissions==before);
    CHECK(d.session().error()==S::Error::OwnershipLost); CHECK(d.end());
}
static void init_failure() {
    D d; Wire.shortRead=true; CHECK(!begin(d)); CHECK(I2CBusManager::instance().clientCount()==0);
    CHECK(d.session().error()==S::Error::Transport); CHECK(!d.session().ready());
}
static void busy(bool destroy) {
    auto* d = new D; CHECK(begin(*d)); auto& m=I2CBusManager::instance();
    std::promise<void> locked, release; auto signal = release.get_future();
    std::thread holder([&] {CHECK(m.lock()); locked.set_value(); signal.wait(); m.unlock();});
    locked.get_future().wait(); const unsigned before = Wire.transmissions;
    if(destroy) { delete d; d=nullptr; }
    else {
        CHECK(!d->setOutputs(1,1)); CHECK(d->error()==D::Error::Busy);
        CHECK(!d->configure(0x8282,0x7d7d,0x8282,0x0101,0x0808));
        CHECK(d->error()==D::Error::Busy && d->session().ready());
        CHECK(!d->end()); CHECK(d->session().ready());
    }
    CHECK(Wire.transmissions==before); release.set_value(); holder.join();
    if(d) {CHECK(d->end()); delete d;}
    CHECK(m.clientCount()==0);
}
static void capacity() {
    auto& m = I2CBusManager::instance(); String error; int owners[16];
    for(auto& o: owners) CHECK(m.acquire(&o,21,22,100000,error));
    D d; CHECK(!begin(d)); CHECK(Wire.transmissions==0); CHECK(m.clientCount()==16);
    for(auto& o: owners) CHECK(m.release(&o));
}
static void controlled_reset(const std::string& mode) {
    D d;
    struct Reset : MCP23017ResetControl {
        D* device = nullptr;
        int gpio = 4;
        bool owner = true, succeeds = true, loseOwner = false;
        unsigned calls = 0;
        int pin() const override { return gpio; }
        bool owned() const override { return owner; }
        bool pulse() override {
            ++calls;
            CHECK(Wire.transmissions == 0);
            CHECK(I2CBusManager::instance().ownsAddress(device, 0x20));
            Wire.registers.fill(0); Wire.registers[0] = Wire.registers[1] = 255;
            if (loseOwner) owner = false;
            return succeeds;
        }
    } reset;
    reset.device = &d;
    auto start = [&] { return d.beginWithReset(21,22,100000,0x20,reset,0x8080,0x7f7f,0,0,0); };
    if(mode=="reset_not_owned") reset.owner=false;
    if(mode=="reset_bus_pin") reset.gpio=21;
    if(mode=="reset_failed") reset.succeeds=false;
    if(mode=="reset_owner_lost") reset.loseOwner=true;
    auto& manager=I2CBusManager::instance();int other;String error;
    if(mode=="reset_collision") {
        CHECK(manager.acquire(&other,21,22,100000,error));CHECK(manager.claimAddress(&other,0x20,error));
    }
    Wire.registers[0x0a]=0x80; // inherited bank must only be cleared by the reset capability
    if(mode=="reset_normal") {
        CHECK(start());CHECK(reset.calls==1 && d.session().ready());
        CHECK(!start());CHECK(reset.calls==1);CHECK(d.end());
    } else {
        CHECK(!start());CHECK(!d.session().ready() && Wire.transmissions==0);
        CHECK(reset.calls==((mode=="reset_failed" || mode=="reset_owner_lost")?1U:0U));
        if(mode=="reset_collision") CHECK(manager.release(&other));
    }
    CHECK(manager.clientCount()==0);
}
static void configure(const std::string& mode) {
    D d;CHECK(begin(d));CHECK(d.setOutputs(0x8101,0x0101));
    const unsigned before=Wire.transmissions;
    if(mode=="configure_lost")CHECK(I2CBusManager::instance().release(&d));
    if(mode=="configure_short")Wire.writeLimit=1;
    const bool ok=d.configure(mode=="configure_invalid"?0:0x8282,0x7d7d,0x8282,0x0101,0x0808);
    if(mode=="configure_normal") {
        CHECK(ok && d.session().ready());
        uint16_t latch=0;CHECK(d.session().knownLatch(latch) && latch==0x0282);
        CHECK(Wire.registers[0]==0x7d && Wire.registers[1]==0x7d);
    } else {
        CHECK(!ok && d.error()==D::Error::Session);
        if(mode=="configure_invalid")CHECK(d.session().ready() && Wire.transmissions==before);
        else {
            CHECK(!d.session().ready());
            if(mode=="configure_lost")CHECK(Wire.transmissions==before);
            const unsigned failed=Wire.transmissions;
            CHECK(!d.configure(0x8282,0x7d7d,0x8282,0x0101,0x0808));
            CHECK(Wire.transmissions==failed);
        }
    }
    CHECK(d.end());CHECK(I2CBusManager::instance().clientCount()==0);
}
int main(int argc,char** argv) {
    CHECK(argc==2); const std::string mode=argv[1];
    if(mode.find("reset_")==0) controlled_reset(mode);
    else if(mode.find("configure_")==0) configure(mode);
    else if(mode=="admission") admission(); else if(mode=="collision") collision();
    else if(mode=="normal") normal(); else if(mode=="write_short") write_short();
    else if(mode=="lease_lost") lease_lost(); else if(mode=="init_failure") init_failure();
    else if(mode=="busy") busy(false); else if(mode=="deferred") busy(true);
    else if(mode=="capacity") capacity(); else read_fault(mode);
    std::printf("%s PASS\n",mode.c_str());
}
