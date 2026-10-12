#include <Arduino.h>
static uint32_t pcaClock=0;
static uint32_t pcaTestMicros() {return pcaClock;}
#define micros pcaTestMicros
#include "peripherals/drivers/PCA9685Device.h"
#undef micros
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
using namespace FastBee;
using D=PCA9685Device;
using S=PCA9685OutputSession;
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(86);}}while(0)
struct Gate final:PCA9685OutputGate {
    bool gated=true,disableWorks=true,enableWorks=true;
    bool owns=true;
    int gpio=4;
    int pin() const override {return gpio;}
    bool owned() const override {return owns;}
    unsigned disables=0,enables=0;
    bool disable() override {++disables;if(disableWorks&&owns)gated=true;return disableWorks&&owns;}
    bool enable() override {++enables;if(enableWorks&&owns)gated=false;return enableWorks&&owns;}
    bool disabled() const override {return owns&&gated;}
};
static bool begin(D& d,unsigned address=0x40) {
    return d.begin(21,22,100000,address,S::ResetEvidence::ConfirmedPowerOn,50,25000000,3,false);
}
static void ready(D& d) {
    CHECK(begin(d));CHECK(!d.session().ready());pcaClock+=499;CHECK(d.service());CHECK(!d.session().ready());
    ++pcaClock;CHECK(d.service());CHECK(d.session().ready()&&d.outputsDisabled());
}
static void failed(D& d,Gate& gate) {
    CHECK(!d.session().ready()&&gate.gated);
    PCA9685ChannelRegisters value;CHECK(!d.session().knownChannel(0,value));
}
static void admission() {
    Gate gate;D d(gate);
    for(unsigned addr:{0U,0x3fU,0x70U,0x78U,0x140U})CHECK(!begin(d,addr));
    CHECK(!d.begin(21,22,100000,0x40,S::ResetEvidence::Unknown,50,25000000,3,false));
    CHECK(!d.begin(21,21,100000,0x40,S::ResetEvidence::ConfirmedPowerOn,50,25000000,3,false));
    CHECK(Wire.starts==0&&Wire.transmissions==0&&gate.disables==0);
}
static void collision(unsigned address) {
    auto& manager=I2CBusManager::instance();String error;int other;
    CHECK(manager.acquire(&other,21,22,100000,error));CHECK(manager.claimAddress(&other,address,error));
    Gate gate;D d(gate);CHECK(!begin(d));CHECK(d.error()==D::Error::Admission);
    CHECK(Wire.transmissions==0&&gate.disables==0&&manager.clientCount()==1);
    CHECK(manager.ownsAddress(&other,address));CHECK(manager.release(&other));
}
static void normal() {
    Gate gate;D d(gate);ready(d);auto& manager=I2CBusManager::instance();
    CHECK(manager.clientCount()==2&&manager.ownsAddress(&d,0x40));
    CHECK(d.setChannel(0,4000,200));CHECK(Wire.registers[6]==0xa0&&Wire.registers[8]==0x68);
    CHECK(d.enableOutputs()&&!gate.gated&&gate.enables==1);
    String error;int other;CHECK(manager.acquire(&other,21,22,100000,error));
    CHECK(!manager.claimAddress(&other,0x70,error));CHECK(manager.release(&other));
    const auto count=Wire.transmissions;CHECK(!begin(d));CHECK(d.error()==D::Error::AlreadyOpen);
    CHECK(Wire.transmissions==count);CHECK(d.end());CHECK(gate.gated&&manager.clientCount()==0);
    for(const auto address:Wire.addresses)CHECK(address==0x40); // No broadcast/alias traffic.
}
static void transport(const std::string& mode) {
    Gate gate;D d(gate);ready(d);CHECK(d.enableOutputs());const auto reads=Wire.reads,requests=Wire.requests;
    if(mode=="pointer_short")Wire.writeLimit=0;
    else if(mode=="write_short")Wire.writeLimit=1;
    else if(mode=="nack")Wire.ack=false;
    else if(mode=="short_read")Wire.shortRead=true;
    else if(mode=="negative")Wire.nextByte=-1;
    else if(mode=="wide")Wire.nextByte=256;
    else if(mode=="empty")Wire.emptyRead=true;
    else if(mode=="extra")Wire.extraRead=true;
    else if(mode=="infinite")Wire.infinite=true;
    else CHECK(false);
    CHECK(!d.setChannel(0,0,100));failed(d,gate);
    if(mode=="pointer_short"){CHECK(Wire.requests==requests);CHECK(Wire.stops.back());}
    if(mode=="nack")CHECK(Wire.requests==requests);
    if(mode=="infinite")CHECK(Wire.reads-reads<=260);
    CHECK(d.end());CHECK(I2CBusManager::instance().clientCount()==0);
}
static void lost() {
    Gate gate;D d(gate);ready(d);CHECK(d.enableOutputs());CHECK(I2CBusManager::instance().release(&d));
    const auto count=Wire.transmissions;CHECK(!d.setChannel(0,0,100));failed(d,gate);
    CHECK(d.session().error()==S::Error::OwnershipLost&&Wire.transmissions==count);CHECK(d.end());
    CHECK(I2CBusManager::instance().clientCount()==0);
}
static void busy(bool destroy) {
    Gate gate;auto* d=new D(gate);ready(*d);CHECK(d->enableOutputs());auto& manager=I2CBusManager::instance();
    std::promise<void> locked,release;auto signal=release.get_future();
    std::thread holder([&]{CHECK(manager.lock());locked.set_value();signal.wait();manager.unlock();});
    locked.get_future().wait();const auto count=Wire.transmissions;
    if(destroy){delete d;d=nullptr;}
    else {CHECK(!d->setChannel(0,0,100));CHECK(d->error()==D::Error::Busy);CHECK(!d->end());CHECK(d->session().state()==S::State::Stopped);}
    CHECK(gate.gated&&Wire.transmissions==count);release.set_value();holder.join();
    if(d){CHECK(d->end());delete d;}CHECK(manager.clientCount()==0);
}
static void capacity() {
    auto& manager=I2CBusManager::instance();String error;int owners[15];
    for(auto& owner:owners)CHECK(manager.acquire(&owner,21,22,100000,error));
    Gate gate;D d(gate);CHECK(!begin(d));CHECK(Wire.transmissions==0&&manager.clientCount()==15);
    for(auto& owner:owners)CHECK(manager.release(&owner));
}
static void gateFailure() {
    Gate gate;D d(gate);ready(d);gate.enableWorks=false;CHECK(!d.enableOutputs());CHECK(gate.gated);
    CHECK(d.session().state()==S::State::Stopped);CHECK(d.end());
}
static void initFailure() {
    Gate gate;D d(gate);Wire.shortRead=true;CHECK(!begin(d));failed(d,gate);
    CHECK(I2CBusManager::instance().clientCount()==0);CHECK(d.end());
}
static void gateAdmission(const std::string& mode) {
    Gate gate;D d(gate);
    if(mode=="gate_unowned")gate.owns=false;
    else gate.gpio=mode=="gate_sda"?21:mode=="gate_scl"?22:49;
    CHECK(!begin(d)&&d.error()==D::Error::OutputGate);
    CHECK(Wire.starts==0&&Wire.transmissions==0&&gate.disables==0);
}
static void gateLost() {
    Gate gate;D d(gate);ready(d);CHECK(d.enableOutputs());gate.owns=false;
    const auto count=Wire.transmissions;
    CHECK(!d.setChannel(0,0,100)&&!d.session().ready()&&Wire.transmissions==count);
    CHECK(!d.end()&&I2CBusManager::instance().clientCount()==2);
    gate.owns=true;CHECK(d.end()&&gate.gated&&I2CBusManager::instance().clientCount()==0);
}
static void gateChanged() {
    Gate gate;D d(gate);ready(d);CHECK(d.enableOutputs());gate.gpio=5;
    const auto count=Wire.transmissions,disables=gate.disables;
    CHECK(!d.setChannel(0,0,100)&&!d.session().ready());
    CHECK(!d.outputsDisabled()&&!d.end());
    CHECK(Wire.transmissions==count&&gate.disables==disables);
    CHECK(I2CBusManager::instance().clientCount()==2);
    gate.gpio=4;CHECK(d.end()&&gate.gated&&I2CBusManager::instance().clientCount()==0);
}
static void endedDestructor() {
    Gate gate;auto* d=new D(gate);ready(*d);CHECK(d->enableOutputs());CHECK(d->end());
    const auto count=Wire.transmissions,disables=gate.disables;
    delete d;
    CHECK(gate.disables==disables&&Wire.transmissions==count&&I2CBusManager::instance().clientCount()==0);
}
int main(int argc,char** argv) {
    CHECK(argc==2);const std::string mode=argv[1];
    if(mode=="admission")admission();else if(mode=="main_collision")collision(0x40);
    else if(mode=="alias_collision")collision(0x70);else if(mode=="normal")normal();
    else if(mode=="lease_lost")lost();else if(mode=="busy")busy(false);else if(mode=="deferred")busy(true);
    else if(mode=="capacity")capacity();else if(mode=="gate_failure")gateFailure();else if(mode=="init_failure")initFailure();
    else if(mode=="gate_unowned"||mode=="gate_sda"||mode=="gate_scl"||mode=="gate_invalid")gateAdmission(mode);
    else if(mode=="gate_lost")gateLost();
    else if(mode=="gate_changed")gateChanged();else if(mode=="ended_destructor")endedDestructor();
    else transport(mode);
    std::printf("%s PASS\n",mode.c_str());
}
