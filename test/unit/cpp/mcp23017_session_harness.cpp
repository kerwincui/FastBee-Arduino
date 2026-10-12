#include "peripherals/drivers/MCP23017PortSession.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using S = FastBee::MCP23017PortSession;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(86); } } while (0)

struct Bus {
    struct Event { bool write; uint8_t reg, value; };
    std::array<uint8_t, 22> reg{};
    std::vector<Event> events;
    int calls = 0, failAt = 0, ownershipAt = 0, checks = 0;
    bool leased = true, applyFailedWrite = false, ignoreWrite = false;
    int ignoredRegister = -1;
    Bus() { reg[0] = reg[1] = 255; }
    bool exclusive() { ++checks; return leased && (!ownershipAt || checks != ownershipAt); }
    bool read8(uint8_t addr, uint8_t& value) {
        CHECK(addr < reg.size());
        ++calls; events.push_back({false, addr, reg[addr]});
        if (failAt == calls) return false;
        value = reg[addr]; return true;
    }
    bool write8(uint8_t addr, uint8_t value) {
        CHECK(addr < reg.size());
        ++calls; events.push_back({true, addr, value});
        if (!ignoreWrite && addr != ignoredRegister && (failAt != calls || applyFailedWrite)) reg[addr] = value;
        return failAt != calls;
    }
    int writes() const { int n = 0; for (const auto& e : events) n += e.write; return n; }
    void clear() { events.clear(); calls = checks = failAt = ownershipAt = 0; }
};
static bool begin(S& s, Bus& b, S::ResetEvidence r = S::ResetEvidence::ConfirmedBank0Reset) {
    return s.begin(b, r, 0x8181, 0x7e7e, 0x8080, 0x0202, 0x0404);
}
static void unknown(S& s) {
    CHECK(!s.ready()); CHECK(!s.sample().valid);
    uint16_t value = 0xbeef; CHECK(!s.knownLatch(value)); CHECK(value == 0xbeef);
}
static void preconditions() {
    S s; Bus b;
    CHECK(!begin(s, b, S::ResetEvidence::Unknown)); CHECK(b.calls == 0);
    CHECK(s.error() == S::Error::ResetRequired);
    CHECK(!s.begin(b, S::ResetEvidence::ConfirmedBank0Reset, 0, 0xffff, 0, 0, 0));
    CHECK(b.calls == 0); CHECK(s.error() == S::Error::InvalidConfig);
    b.leased = false; CHECK(!begin(s, b)); CHECK(b.calls == 0); unknown(s);
    CHECK(s.error() == S::Error::OwnershipLost);
    CHECK(!s.sampleInputs(b, 9)); CHECK(!s.setOutputs(b, 1, 1)); CHECK(b.calls == 0);
}
static void reset_checks() {
    for (uint8_t addr : {0x0a,0x0b,0,1,4,5,0x14,0x15,2,3,0x0c,0x0d}) {
        S s; Bus b; b.reg[addr] ^= 1;
        CHECK(!begin(s, b)); CHECK(b.writes() == 0); unknown(s);
        CHECK(s.error() == S::Error::ResetMismatch);
    }
}
static void ordering() {
    S s; Bus b; CHECK(begin(s, b)); CHECK(s.ready()); CHECK(b.writes() == 8);
    bool latchA = false, latchB = false;
    for (const auto& e : b.events) if (e.write) {
        if (e.reg == 0x14) { CHECK(e.value == 0x80); latchA = true; }
        if (e.reg == 0x15) { CHECK(e.value == 0x80); latchB = true; }
        if (e.reg <= 1) { CHECK(latchA && latchB); CHECK(e.value == 0x7e); }
        CHECK(e.reg != 0x12 && e.reg != 0x13); // GPIO never written.
    }
    CHECK(b.calls == 34);
    Bus ignored; S t; ignored.ignoreWrite = true;
    CHECK(!begin(t, ignored)); unknown(t); CHECK(t.error() == S::Error::LatchLost);
}
static void init_failures() {
    for (int n = 1; n <= 34; ++n) for (bool applied : {false, true}) {
        S s; Bus b; b.failAt = n; b.applyFailedWrite = applied;
        CHECK(!begin(s,b)); CHECK(b.calls == n); unknown(s);
        CHECK(s.error() == S::Error::Transport);
        const int count = b.calls; CHECK(!s.setOutputs(b,1,1)); CHECK(b.calls == count);
    }
}
static void preload_readback() {
    for (int address : {0x14, 0x15}) {
        S s; Bus b; b.ignoredRegister = address;
        CHECK(!begin(s, b)); unknown(s);
        CHECK(b.reg[0] == 255 && b.reg[1] == 255);
        for (const auto& event : b.events) CHECK(!event.write || event.reg > 1);
        CHECK(s.error() == S::Error::LatchLost);
    }
}
static void latch_not_gpio() {
    S s; Bus b; CHECK(begin(s,b)); b.clear();
    b.reg[0x12] = 0xff; b.reg[0x13] = 0x55;
    CHECK(s.setOutputs(b, 0x0001, 0x0001)); CHECK(b.writes() == 1);
    CHECK(b.reg[0x14] == 0x81 && b.reg[0x15] == 0x80);
    for (const auto& e : b.events) CHECK(e.reg != 0x12 && e.reg != 0x13);
    uint16_t value = 0; CHECK(s.knownLatch(value)); CHECK(value == 0x8081);
    b.clear(); CHECK(!s.setOutputs(b,2,2)); CHECK(b.calls == 0); CHECK(s.ready());
    CHECK(!s.setOutputs(b,1,2)); CHECK(b.calls == 0);
    CHECK(!s.setOutputs(b,0x10000,0)); CHECK(b.calls == 0);
    CHECK(s.setOutputs(b,0,0)); CHECK(b.writes() == 0);
}
static void update_failures() {
    for (int n = 1; n <= 26; ++n) for (bool applied : {false,true}) {
        S s; Bus b; CHECK(begin(s,b)); b.clear(); b.failAt = n; b.applyFailedWrite = applied;
        CHECK(!s.setOutputs(b,0x0101,0x0101)); CHECK(b.calls == n); unknown(s);
        CHECK(b.writes() <= 2); // No rollback/retry after partial port writes.
        const int count = b.calls; CHECK(!s.sampleInputs(b,9)); CHECK(b.calls == count);
    }
    S s; Bus b; CHECK(begin(s,b)); b.clear(); b.ignoreWrite = true;
    CHECK(!s.setOutputs(b,1,1)); unknown(s); CHECK(s.error() == S::Error::LatchLost);
}
static void sample_failures() {
    for (int n = 1; n <= 14; ++n) {
        S s; Bus b; CHECK(begin(s,b)); b.reg[0x12]=0x55; b.reg[0x13]=0xaa;
        CHECK(s.sampleInputs(b, 0xffffffff)); CHECK(s.sample().valid);
        CHECK(s.sample().inputs == (0xaa55 & 0x7e7e)); CHECK(s.sample().capturedMs == 0xffffffff);
        b.clear(); b.reg[0x12]=0xff; b.failAt = n;
        CHECK(!s.sampleInputs(b,0)); CHECK(b.calls == n); CHECK(b.writes() == 0); unknown(s);
    }
}
static void drift() {
    for (uint8_t addr : {0x0a,0x0b,4,5,0,1,2,3,0x0c,0x0d,0x14,0x15}) {
        S s; Bus b; CHECK(begin(s,b)); b.clear(); b.reg[addr] ^= 1;
        CHECK(!s.setOutputs(b,1,1)); CHECK(b.writes() == 0); unknown(s);
        CHECK(s.error() == (addr >= 0x14 ? S::Error::LatchLost : S::Error::ConfigLost));
    }
}
static void ownership() {
    for (int n = 1; n <= 26; ++n) {
        S s; Bus b; CHECK(begin(s,b)); b.clear(); b.ownershipAt = n;
        CHECK(!s.setOutputs(b,0x0101,0x0101)); CHECK(b.calls == n-1); unknown(s);
        CHECK(s.error() == S::Error::OwnershipLost);
    }
    for (int n = 1; n <= 35; ++n) {
        S s; Bus b; b.ownershipAt = n;
        CHECK(!begin(s,b)); CHECK(b.calls == (n <= 2 ? 0 : n-2)); unknown(s);
    }
}
static void restart() {
    S s; Bus b; CHECK(begin(s,b)); b.clear(); s.forget(); unknown(s); CHECK(b.calls == 0);
    CHECK(!begin(s,b)); CHECK(b.writes() == 0); // A software restart is not a chip reset.
    Bus reset; CHECK(begin(s,reset)); CHECK(s.ready());
    CHECK(s.sampleInputs(reset,17)); CHECK(s.sample().valid);
    CHECK(s.setOutputs(reset,1,1)); CHECK(!s.sample().valid);
}
static bool reconfigure(S& s, Bus& b) {
    return s.configure(b, 0x8282, 0x7d7d, 0x8282, 0x0101, 0x0808);
}
static void prepare_reconfigure(S& s, Bus& b) {
    CHECK(begin(s,b));
    CHECK(s.setOutputs(b,0x8101,0x0101)); // A7 stays high, B7 is deliberately low.
    CHECK(s.sampleInputs(b,19));
    b.clear();
}
static void config_ordering() {
    S s; Bus b; prepare_reconfigure(s,b); CHECK(reconfigure(s,b));
    CHECK(s.ready() && !s.sample().valid);
    uint16_t latch=0; CHECK(s.knownLatch(latch) && latch==0x0282);
    CHECK(b.reg[0]==0x7d && b.reg[1]==0x7d);
    bool retiredA=false,retiredB=false,latchedA=false,latchedB=false;
    bool checkedA=false,checkedB=false;
    for(const auto& e:b.events) {
        if(e.write && e.reg<=1 && e.value==0x7f) {
            if(e.reg==0)retiredA=true;else retiredB=true;
        }
        if(e.write && (e.reg==0x14 || e.reg==0x15)) {
            CHECK(retiredA && retiredB);
            if(e.reg==0x14)latchedA=true;else latchedB=true;
        }
        if(!e.write && e.reg==0x14 && latchedA)checkedA=true;
        if(!e.write && e.reg==0x15 && latchedB)checkedB=true;
        if(e.write && e.reg<=1 && e.value==0x7d)CHECK(checkedA && checkedB);
        CHECK(e.reg!=0x12 && e.reg!=0x13);
        // Mandatory A7/B7 direction bits are never changed to inputs.
        if(e.write && e.reg<=1)CHECK((e.value&0x80)==0);
    }
    CHECK(retiredA && retiredB && checkedA && checkedB);
}
static void config_same() {
    S s; Bus b; prepare_reconfigure(s,b);
    // Changing startup values must NOT replay them to existing outputs.
    CHECK(s.configure(b,0x8181,0x7e7e,0,0x0202,0x0404));
    CHECK(b.writes()==0 && s.sample().valid);
    uint16_t latch=0;CHECK(s.knownLatch(latch) && latch==0x0181);
}
static void config_invalid() {
    S s; Bus b; prepare_reconfigure(s,b);
    for(unsigned outputs:{0U,0x8101U,0x18181U}) {
        CHECK(!s.configure(b,outputs,0x7e7e,0,0,0));
        CHECK(b.calls==0 && s.ready() && s.sample().valid);
    }
    CHECK(!s.configure(b,0x8181,0x0101,0,0,0));CHECK(b.calls==0 && s.ready());
    CHECK(!s.configure(b,0x8181,0x7e7e,2,0,0));CHECK(b.calls==0 && s.ready());
    CHECK(!s.configure(b,0x8181,0x7e7e,0,1,0));CHECK(b.calls==0 && s.ready());
}
static void config_failures(bool ownershipLost) {
    S reference;Bus healthy;prepare_reconfigure(reference,healthy);
    CHECK(reconfigure(reference,healthy));const int total=healthy.calls;
    for(int n=1;n<=total;++n) for(bool applied:{false,true}) {
        S s;Bus b;prepare_reconfigure(s,b);b.applyFailedWrite=applied;
        if(ownershipLost)b.ownershipAt=n;else b.failAt=n;
        CHECK(!reconfigure(s,b));unknown(s);
        CHECK(b.calls==(ownershipLost?n-1:n));
        const int count=b.calls;
        CHECK(!reconfigure(s,b) && !s.setOutputs(b,0x8080,0));
        CHECK(!s.sampleInputs(b,20) && b.calls==count); // No blind rollback/retry/reset.
    }
}
static void config_ignored() {
    for(int reg:{0,1,0x14,0x15,2,3,0x0c,0x0d}) {
        S s;Bus b;prepare_reconfigure(s,b);b.ignoredRegister=reg;
        CHECK(!reconfigure(s,b));unknown(s);
        if(reg==0 || reg==1) {
            for(const auto& e:b.events)CHECK(!e.write || e.reg<=1);
        } else if(reg==0x14 || reg==0x15) {
            CHECK(b.reg[0]==0x7f && b.reg[1]==0x7f); // New outputs never enabled.
        }
    }
}
static void config_drift() {
    for(uint8_t reg:{0x0a,0x0b,4,5,0,1,2,3,0x0c,0x0d,0x14,0x15}) {
        S s;Bus b;prepare_reconfigure(s,b);b.reg[reg]^=1;
        CHECK(!reconfigure(s,b) && b.writes()==0);unknown(s);
    }
}
static void config_logical_inputs() {
    S s;Bus b;prepare_reconfigure(s,b);
    CHECK(s.configure(b,0x8181,0x0606,0,0x0202,0x0404));
    CHECK(b.writes()==0 && !s.sample().valid);
    b.reg[0x12]=b.reg[0x13]=255;CHECK(s.sampleInputs(b,23));
    CHECK(s.sample().inputs==0x0606);
}
int main(int argc, char** argv) {
    CHECK(argc == 2); const std::string mode = argv[1];
    if(mode=="preconditions") preconditions(); else if(mode=="reset_checks") reset_checks();
    else if(mode=="ordering") ordering(); else if(mode=="init_failures") init_failures();
    else if(mode=="latch_not_gpio") latch_not_gpio(); else if(mode=="update_failures") update_failures();
    else if(mode=="sample_failures") sample_failures(); else if(mode=="drift") drift();
    else if(mode=="ownership") ownership(); else if(mode=="restart") restart();
    else if(mode=="preload_readback") preload_readback();
    else if(mode=="config_ordering")config_ordering();
    else if(mode=="config_same")config_same();
    else if(mode=="config_invalid")config_invalid();
    else if(mode=="config_failures")config_failures(false);
    else if(mode=="config_ownership")config_failures(true);
    else if(mode=="config_ignored")config_ignored();
    else if(mode=="config_drift")config_drift();
    else if(mode=="config_logical_inputs")config_logical_inputs();
    else CHECK(false);
    std::printf("%s PASS\n", mode.c_str());
}
