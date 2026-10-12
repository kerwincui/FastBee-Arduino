#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include "config.inc"
#include "core/I2CBusManager.h"
#include "screen/ScreenBackend.h"
#include "core/FeatureFlags.h"
#include "systems/LoggerSystem.h"
#include <new>
// Test-only visibility to revoke the second lease through the real manager.
#define private public
#include "Gt911TouchBackend.cpp"
#undef private
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#x);std::exit(86);}} while(false)
void initConfig() {
    std::memcpy(Wire.registers+0x8140,"911",4);
    for(unsigned i=0;i<184;++i)Wire.registers[0x8047+i]=i*7+3;
    Wire.registers[0x8048]=0xe0;Wire.registers[0x8049]=1;
    Wire.registers[0x804a]=0xe0;Wire.registers[0x804b]=1;
    Wire.registers[0x804c]=0xa5;
    uint8_t sum=0;for(unsigned i=0;i<184;++i)sum+=Wire.registers[0x8047+i];
    Wire.registers[0x80ff]=0-sum;Wire.registers[0x8100]=0;
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    const std::string mode=argv[1];
    using namespace FastBee; using namespace FastBee::Screen;
    auto& bus=I2CBusManager::instance(); String error; int other=0;
    DisplayConfigParams c{}; std::strcpy(c.touchBackend,"gt911");
    c.touchSda=15;c.touchScl=7;c.touchRst=8;c.touchInt=9;
    c.width=c.height=480;c.i2cFrequency=100000;
    initConfig();
    auto touch=createTouchBackend(c,error); CHECK(touch);
    if(mode=="unused_destructor" || mode=="invalid") {
        if(mode=="invalid") {c.width=0;CHECK(!touch->begin(c,error));}
        touch.reset();
        CHECK(Wire.transmissions==0 && mockPinModes==0 && mockPinWrites==0);
    } else if(mode=="begin_busy") {
        std::atomic<bool> locked{false}, release{false};
        std::thread holder([&]{CHECK(bus.lock());locked=true;while(!release.load())std::this_thread::yield();bus.unlock();});
        while(!locked.load())std::this_thread::yield();
        CHECK(!touch->begin(c,error));
        CHECK(error=="SCREEN_TOUCH_I2C_LOCK_FAILED");
        release=true;holder.join();
        touch.reset();
        CHECK(Wire.transmissions==0 && mockPinModes==0 && mockPinWrites==0);
    } else if(mode.find("conflict_")==0) {
        const uint8_t addr=mode=="conflict_primary"?0x5d:0x14;
        CHECK(bus.acquire(&other,15,7,100000,error) && bus.claimAddress(&other,addr,error));
        CHECK(!touch->begin(c,error));
        CHECK(error=="I2C_ADDRESS_CONFLICT");
        CHECK(Wire.transmissions==0 && mockPinModes==0 && mockPinWrites==0);
        CHECK(bus.clientCount()==1 && bus.ownsAddress(&other,addr));
        touch.reset();
        CHECK(mockPinModes==0 && mockPinWrites==0);
        CHECK(bus.release(&other));
    } else if(mode=="capacity") {
        int owners[I2CBusManager::MAX_CLIENTS-1]{};
        for(auto& owner:owners) CHECK(bus.acquire(&owner,15,7,100000,error));
        CHECK(!touch->begin(c,error));
        CHECK(bus.clientCount()==I2CBusManager::MAX_CLIENTS-1);
        CHECK(Wire.transmissions==0 && mockPinModes==0 && mockPinWrites==0);
        for(auto& owner:owners) CHECK(bus.release(&owner));
    } else if(mode=="not_found") {
        Wire.present=0x20; CHECK(!touch->begin(c,error));
        CHECK(error=="SCREEN_TOUCH_GT911_NOT_FOUND" && bus.clientCount()==0);
    } else {
        if(mode=="reserve_primary" || mode.find("lost_primary")==0) Wire.present=0x5d;
        CHECK(touch->begin(c,error) && touch->ready());
        if(mode=="destructor") {
            touch.reset(); CHECK(bus.clientCount()==0);
        } else if(mode=="deferred_destructor") {
            std::atomic<bool> locked{false}, release{false};
            std::thread holder([&]{CHECK(bus.lock());locked=true;while(!release.load())std::this_thread::yield();bus.unlock();});
            while(!locked.load())std::this_thread::yield();
            touch.reset(); release=true; holder.join();
            CHECK(bus.acquire(&other,15,7,100000,error));
            CHECK(bus.clientCount()==1); CHECK(bus.release(&other));
        } else if(mode.find("lost_")==0) {
            const bool primary=mode.find("lost_primary")==0;
            const void* owner=primary ? static_cast<const void*>(touch.get()) :
                &static_cast<Gt911TouchBackend*>(touch.get())->_alternateOwner;
            const uint8_t address=primary?0x5d:0x14;
            CHECK(bus.release(owner));
            if(mode.find("_address")!=std::string::npos) {
                CHECK(bus.acquire(owner,15,7,100000,error));
                CHECK(bus.claimAddress(owner,0x20,error));
            }
            CHECK(bus.acquire(&other,15,7,100000,error) && bus.claimAddress(&other,address,error));
            const unsigned before=Wire.transmissions;
            TouchPoint point; mockNow+=20;
            CHECK(!touch->poll(point) && Wire.transmissions==before);
            const uint8_t value=0;
            CHECK(!static_cast<Gt911TouchBackend*>(touch.get())->writeRegister(0x8040,&value,1));
            CHECK(Wire.transmissions==before);
            touch->end(); CHECK(bus.ownsAddress(&other,address)); CHECK(bus.release(&other));
        } else if(mode=="failed_rebegin") {
            c.width=0;CHECK(!touch->begin(c,error) && !touch->ready());
            CHECK(bus.clientCount()==0);
        } else {
            CHECK(bus.clientCount()==2);
            CHECK(bus.acquire(&other,15,7,100000,error));
            CHECK(!bus.claimAddress(&other,0x14,error));
            CHECK(!bus.claimAddress(&other,0x5d,error));
            CHECK(bus.release(&other));
            if(mode=="reinitialize") {
                CHECK(touch->begin(c,error)); CHECK(bus.clientCount()==2);
            }
            touch->end(); CHECK(bus.clientCount()==0);
        }
    }
    CHECK(bus.clientCount()==0);
}
