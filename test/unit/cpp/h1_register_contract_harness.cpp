#include "peripherals/drivers/ADS1115Contract.h"
#include "peripherals/drivers/PCA9685Contract.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

using namespace FastBee;

int main(int argc, char** argv) {
    assert(argc == 2);
    const char* name = argv[1];
    if (!std::strcmp(name, "ads_config")) {
        const uint32_t ranges[] = {6144,4096,2048,1024,512,256};
        const uint32_t rates[] = {8,16,32,64,128,250,475,860};
        for (uint32_t mux=0;mux<8;++mux) for (uint32_t gain=0;gain<6;++gain)
            for (uint32_t rate=0;rate<8;++rate) {
                ADS1115ConversionConfig c;
                assert(makeADS1115ConversionConfig(mux,ranges[gain],rates[rate],c));
                assert((c.registerValue & 0x8103) == 0x8103);
                assert(((c.registerValue >> 12) & 7) == mux);
                assert(((c.registerValue >> 9) & 7) == gain);
                assert(((c.registerValue >> 5) & 7) == rate);
                assert((c.registerValue & 0x001c) == 0);
                assert(c.nominalPeriodUs == (1000000+rates[rate]-1)/rates[rate]);
            }
    } else if (!std::strcmp(name, "ads_reject")) {
        ADS1115ConversionConfig c;
        for (const auto mux : {8U,256U,65536U,UINT32_MAX}) {
            c={65535,123}; assert(!makeADS1115ConversionConfig(mux,2048,128,c));
            assert(c.registerValue==0 && c.nominalPeriodUs==0);
        }
        for (const auto range : {0U,255U,257U,2000U,65536U,UINT32_MAX})
            assert(!makeADS1115ConversionConfig(4,range,128,c));
        for (const auto rate : {0U,7U,9U,129U,1000U,65536U,UINT32_MAX})
            assert(!makeADS1115ConversionConfig(4,2048,rate,c));
    } else if (!std::strcmp(name, "ads_decode")) {
        for (const auto range : {6144U,4096U,2048U,1024U,512U,256U}) {
            double previous = -100.0;
            for (int32_t code=-32768;code<=32767;++code) {
                ADS1115DecodedSample s;
                assert(decodeADS1115Sample(static_cast<uint16_t>(code),range,s));
                assert(s.valid && s.signedCode==code && s.volts>previous);
                assert(std::fabs(s.volts-(static_cast<double>(code)*range/32768000))<1e-12);
                assert(s.atCodeRail==(code==-32768 || code==32767));
                previous=s.volts;
            }
        }
        ADS1115DecodedSample s;
        assert(decodeADS1115Sample(0xffff,2048,s) && s.signedCode==-1 && s.volts<0);
        assert(!decodeADS1115Sample(0,0,s) && !s.valid && std::isnan(s.volts));
    } else if (!std::strcmp(name, "pca_frequency")) {
        PCA9685FrequencyConfig c;
        assert(makePCA9685FrequencyConfig(200,25000000,c));
        assert(c.prescale==30 && std::fabs(c.actualHz-25000000.0/(4096*31))<1e-10);
        assert(makePCA9685FrequencyConfig(50,25000000,c) && c.prescale==121);
        for (uint32_t prescale=3;prescale<=255;++prescale) {
            const double hz=25000000.0/(4096*(prescale+1));
            assert(makePCA9685FrequencyConfig(hz,25000000,c) && c.prescale==prescale);
        }
    } else if (!std::strcmp(name, "pca_reject")) {
        PCA9685FrequencyConfig c;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infinity = std::numeric_limits<double>::infinity();
        for (double hz : {0.0,-1.0,nan,infinity,1e-300,1e300,10.0,2000.0}) {
            c={30,200}; assert(!makePCA9685FrequencyConfig(hz,25000000,c));
            assert(c.prescale==0 && c.actualHz==0);
        }
        for (double hz : {0.0,-1.0,nan,infinity,50000001.0})
            assert(!makePCA9685FrequencyConfig(200,hz,c));
        PCA9685ChannelRegisters r;
        for (uint32_t bad : {16U,256U,UINT32_MAX})
            assert(!makePCA9685ChannelRegisters(bad,0,0,r) && !r.valid);
        assert(!makePCA9685ChannelRegisters(0,4096,1,r));
        assert(!makePCA9685ChannelRegisters(0,0,4097,r));
    } else if (!std::strcmp(name, "pca_endpoints")) {
        for (uint32_t channel=0;channel<16;++channel) for(uint32_t phase=0;phase<4096;phase+=17) {
            PCA9685ChannelRegisters r;
            assert(makePCA9685ChannelRegisters(channel,phase,0,r));
            assert(r.valid && r.startRegister==6+channel*4);
            assert(r.bytes[0]==0 && r.bytes[1]==0 && r.bytes[2]==0 && r.bytes[3]==16);
            assert(makePCA9685ChannelRegisters(channel,phase,4096,r));
            assert(r.bytes[0]==0 && r.bytes[1]==16 && r.bytes[2]==0 && r.bytes[3]==0);
        }
    } else if (!std::strcmp(name, "pca_pwm")) {
        for (uint32_t phase=0;phase<4096;++phase) for (const uint32_t width : {1U,2U,2048U,4094U,4095U}) {
            PCA9685ChannelRegisters r;
            assert(makePCA9685ChannelRegisters(15,phase,width,r));
            const uint32_t on=r.bytes[0]+256U*r.bytes[1],off=r.bytes[2]+256U*r.bytes[3];
            assert(r.startRegister==0x42 && on==phase && off<4096 && on!=off);
            assert(((off+4096-on)%4096)==width);
        }
    } else { assert(false); }
    std::cout << name << " PASS (pure contract; no hardware)\n";
}
