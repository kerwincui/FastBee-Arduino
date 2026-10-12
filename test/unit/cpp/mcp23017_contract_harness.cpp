#include "peripherals/drivers/MCP23017Contract.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(86);}}while(false)
using namespace FastBee;
int main(int argc,char** argv) {
    CHECK(argc==2); const std::string mode=argv[1];
    if(mode=="registers") {
        // Independent tables transcribed from DS20001952D Table 3-1.
        const uint8_t expected[2][2][11]={
            {{0,2,4,6,8,10,12,14,16,18,20},{1,3,5,7,9,11,13,15,17,19,21}},
            {{0,1,2,3,4,5,6,7,8,9,10},{16,17,18,19,20,21,22,23,24,25,26}}};
        for(unsigned bank=0;bank<2;++bank) for(unsigned port=0;port<2;++port)
            for(unsigned reg=0;reg<11;++reg) {
                uint8_t address=255; CHECK(mcp23017RegisterAddress(static_cast<MCP23017Register>(reg),port,bank,address));
                CHECK(address==expected[bank][port][reg]);
            }
        uint8_t address=255;
        CHECK(!mcp23017RegisterAddress(static_cast<MCP23017Register>(255),0,0,address) && address==0);
        CHECK(!mcp23017RegisterAddress(MCP23017Register::Latch,2,0,address));
        CHECK(!mcp23017RegisterAddress(MCP23017Register::Latch,0,2,address));
    } else if(mode=="directions") {
        for(uint32_t outputs=0;outputs<=65535;++outputs) {
            MCP23017PortConfig c; c.valid=true;
            const uint32_t inputs=(~outputs)&0xffff;
            const bool valid=makeMCP23017PortConfig(outputs,inputs,outputs,inputs,inputs,c);
            if(!(outputs&0x80) || !(outputs&0x8000)) {CHECK(!valid && !c.valid);continue;}
            CHECK(valid && c.valid);
            CHECK((c.direction[0] | (uint32_t(c.direction[1])<<8))==inputs);
            CHECK((c.latch[0] | (uint32_t(c.latch[1])<<8))==outputs);
            CHECK((c.pullup[0] | (uint32_t(c.pullup[1])<<8))==inputs);
            CHECK((c.polarity[0] | (uint32_t(c.polarity[1])<<8))==inputs);
            CHECK(!(c.direction[0]&0x80) && !(c.direction[1]&0x80));
        }
    } else if(mode=="reject") {
        MCP23017PortConfig c;
        for(const auto invalid : {0x10000U,0xffffffffU}) {
            CHECK(!makeMCP23017PortConfig(invalid,0,0,0,0,c));
            CHECK(!makeMCP23017PortConfig(0x8080,invalid,0,0,0,c));
            CHECK(!makeMCP23017PortConfig(0x8080,0,invalid,0,0,c));
            CHECK(!makeMCP23017PortConfig(0x8080,0,0,invalid,0,c));
            CHECK(!makeMCP23017PortConfig(0x8080,0,0,0,invalid,c));
        }
        for(unsigned bit=0;bit<16;++bit) {
            const uint32_t mask=1U<<bit;
            CHECK(!makeMCP23017PortConfig(0xffff,mask,0,0,0,c));
            CHECK(!makeMCP23017PortConfig(0x8080,0,0,mask,0,c));
            CHECK(!makeMCP23017PortConfig(0x8080,0,0,0,mask,c));
            if(bit!=7 && bit!=15) CHECK(!makeMCP23017PortConfig(0x8080,0,mask,0,0,c));
        }
        CHECK(makeMCP23017PortConfig(0x8080,0,0,0,0,c));
        CHECK(c.direction[0]==0x7f && c.direction[1]==0x7f);
        CHECK(!makeMCP23017PortConfig(0,0,0,0,0,c) && !c.valid);
        CHECK(c.direction[0]==0 && c.direction[1]==0 && c.latch[0]==0 && c.latch[1]==0);
    } else if(mode=="latch") {
        for(uint32_t prior=0;prior<=65535;++prior) for(unsigned bit=0;bit<16;++bit) {
            const uint32_t mask=1U<<bit;uint16_t value;
            CHECK(updateMCP23017Latch(prior,mask|0x8080,mask,mask,value));
            CHECK(value==(prior|mask));
            CHECK(updateMCP23017Latch(prior,mask|0x8080,mask,0,value));
            CHECK(value==(prior&~mask));
        }
        uint16_t value=123;
        CHECK(!updateMCP23017Latch(0,0x8080,1,0,value) && value==0);
        CHECK(!updateMCP23017Latch(0,0x8080,0,1,value));
        CHECK(!updateMCP23017Latch(0,0,0,0,value));
        CHECK(!updateMCP23017Latch(0x10000,0xffff,1,1,value));
    } else CHECK(false);
    std::printf("%s PASS\n",mode.c_str());
}
