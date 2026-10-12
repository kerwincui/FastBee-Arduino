#include "peripherals/drivers/PCA9685OutputSession.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
using Session=FastBee::PCA9685OutputSession;
struct Bus {
    std::array<uint8_t,256> regs{};
    uint32_t now=0;
    unsigned transactions=0, failAt=0, disabledCalls=0;
    bool owns=true, gated=true, gateWorks=true, partial=false;
    Bus() { regs[0]=0x11;regs[1]=4;regs[2]=0xe2;regs[3]=0xe4;regs[4]=0xe8;regs[5]=0xe0;regs[0xfe]=0x1e; }
    bool exclusive() const {return owns;}
    uint32_t nowMicros() const {return now;}
    bool outputsDisabled() const {return gated;}
    bool disableOutputs() {++disabledCalls;if(gateWorks)gated=true;return gateWorks;}
    bool read(uint8_t reg,uint8_t* data,size_t n) {
        ++transactions;if(transactions==failAt)return false;
        assert(size_t(reg)+n<=regs.size());
        for(size_t i=0;i<n;++i)data[i]=regs[reg+i];
        return true;
    }
    bool write(uint8_t reg,const uint8_t* data,size_t n) {
        ++transactions;
        const bool fails=transactions==failAt;
        if(fails && !partial)return false;
        assert(size_t(reg)+n<=regs.size());
        if(reg==0xfa) {
            assert(n==4);
            for(unsigned channel=0;channel<16;++channel)
                for(size_t i=0;i<n;++i)regs[6+4*channel+i]=data[i];
        } else for(size_t i=0;i<n;++i)regs[reg+i]=data[i];
        return !fails;
    }
};
static bool begin(Session& s,Bus& b) {return s.begin(b,Session::ResetEvidence::ConfirmedPowerOn,50,25000000,3,false);}
static void ready(Session& s,Bus& b) {assert(begin(s,b));b.now+=500;assert(s.service(b)&&s.ready()&&b.gated);}
static void rejected(Session& s,Bus& b) {
    assert(s.state()==Session::State::Failed && b.gated);
    FastBee::PCA9685ChannelRegisters value;
    assert(!s.knownChannel(0,value));
    const auto count=b.transactions;
    assert(!s.setChannel(b,0,0,100));assert(b.transactions==count);
}
int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];
    if(mode=="preconditions") {
        Session s;Bus b;
        assert(!s.begin(b,Session::ResetEvidence::Unknown,50,25000000,3,false));
        assert(!s.begin(b,Session::ResetEvidence::ConfirmedPowerOn,50,25000000,0,false));
        assert(b.transactions==0);
        b.owns=false;assert(!begin(s,b));assert(b.transactions==0);rejected(s,b);
    } else if(mode=="wake") {
        Session s;Bus b;b.now=UINT32_MAX-100;
        assert(begin(s,b));assert(!s.ready());
        b.now+=499;const auto count=b.transactions;
        assert(s.service(b)&&!s.ready()&&b.transactions==count);
        ++b.now;assert(s.service(b)&&s.ready()&&b.gated);
        assert(s.confirmForEnable(b));
        FastBee::PCA9685ChannelRegisters value;
        for(unsigned c=0;c<16;++c){assert(s.knownChannel(c,value));assert(value.bytes[3]==0x10);}
        const auto before=b.transactions;assert(!begin(s,b));assert(before==b.transactions);
    } else if(mode=="timeout") {
        Session s;Bus b;assert(begin(s,b));b.now=1000000;assert(!s.service(b));rejected(s,b);
    } else if(mode=="reset") {
        for(unsigned reg:{0U,1U,2U,3U,4U,5U,254U}) {
            Session s;Bus b;b.regs[reg]^=1;assert(!begin(s,b));rejected(s,b);
        }
    } else if(mode=="init_faults") {
        Session normal;Bus baseline;assert(begin(normal,baseline));
        for(bool partial:{false,true})for(unsigned i=1;i<=baseline.transactions;++i) {
            Session s;Bus b;b.failAt=i;b.partial=partial;assert(!begin(s,b));rejected(s,b);
        }
    } else if(mode=="update_faults") {
        Session normal;Bus baseline;ready(normal,baseline);const auto start=baseline.transactions;
        assert(normal.setChannel(baseline,0,4000,200));
        const auto count=baseline.transactions-start;
        for(bool partial:{false,true})for(unsigned i=1;i<=count;++i) {
            Session s;Bus b;ready(s,b);b.gated=false;b.failAt=b.transactions+i;b.partial=partial;
            assert(!s.setChannel(b,0,4000,200));rejected(s,b);
        }
    } else if(mode=="channel") {
        Session s;Bus b;ready(s,b);const auto before=b.transactions;
        assert(!s.setChannel(b,2,0,100));assert(!s.setChannel(b,16,0,100));assert(b.transactions==before);
        assert(s.setChannel(b,0,4000,200));FastBee::PCA9685ChannelRegisters value;
        assert(s.knownChannel(0,value));assert(value.bytes[0]==160 && value.bytes[1]==15);
        assert(value.bytes[2]==104 && value.bytes[3]==0);
        assert(s.setChannel(b,1,0,4096));assert(s.knownChannel(1,value)&&value.bytes[1]==0x10);
    } else if(mode=="drift") {
        for(unsigned reg:{0U,1U,254U,6U}) {
            Session s;Bus b;ready(s,b);b.gated=false;b.regs[reg]^=1;
            assert(!s.setChannel(b,0,0,100));rejected(s,b);
        }
    } else if(mode=="enable_and_stop") {
        Session s;Bus b;ready(s,b);b.regs[10]^=1;
        assert(!s.confirmForEnable(b));rejected(s,b);
        Session other;Bus c;ready(other,c);c.gated=false;
        assert(other.stop(c)&&c.gated&&!other.ready());
    } else if(mode=="gate") {
        Session s;Bus b;b.gateWorks=false;b.gated=false;
        assert(!begin(s,b)&&b.transactions==0&&!s.ready());
    } else assert(false);
    std::printf("%s PASS\n",argv[1]);
}
