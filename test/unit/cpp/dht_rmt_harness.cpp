#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using std::isnan;
using std::isinf;
#define SOC_RMT_SUPPORTED 1
static constexpr int LOW=0,HIGH=1,INPUT_PULLUP=2,OUTPUT=3;
static constexpr int RMT_RX_MODE=0,RMT_MEM_NUM_BLOCKS_1=1,GPIO_MODE_INPUT_OUTPUT_OD=7,ESP_OK=0;
using gpio_num_t=int;
enum class SensorDriverType { DHT11,DHT22 };
struct rmt_data_t { unsigned duration0=0,level0=0,duration1=0,level1=0; };
static std::string scenario;
static uint64_t nowUs=0,releasedAt=0,armedAt=0;
static bool active=false,armed=false,delivered=false,cleanupBlocked=false,interrupted=false;
static unsigned line=1,initCalls=0,armCalls=0;
static rmt_data_t* output=nullptr;
static size_t* countOutput=nullptr;
static std::vector<rmt_data_t> frame;
static uint32_t micros(){return static_cast<uint32_t>(nowUs++);}
static uint32_t millis(){return static_cast<uint32_t>(nowUs/1000);}
static void delay(unsigned ms){nowUs+=ms*1000ULL;}
static void pinMode(int,int){if(active)throw std::string("pinMode detached live RMT");}
static int digitalRead(int){return scenario=="stuck-low" ? 0 : 1;}
static int gpio_get_level(int){return static_cast<int>(line);}
static int gpio_set_direction(int,int){return 0;}
static int gpio_pullup_en(int){return 0;}
static void expireHostLow(){
    // Model the RX idle threshold independently of polling by the CPU. The
    // peripheral can finish while the task is descheduled and still drives low.
    if(armed&&!delivered&&line==0&&nowUs-armedAt>=10000){
        output[0]={10000,0,0,0};*countOutput=1;delivered=true;
    }
}
static int gpio_set_level(int,unsigned v){
    // A task can also be preempted after the final completion check. Keep this
    // remaining race visible as a failed frame, never fabricated sensor data.
    if(scenario=="arm-expiry-after-check"&&v&&line==0&&armed&&!delivered)nowUs+=12000;
    expireHostLow();
    if(v&&line==0&&armed)releasedAt=nowUs;
    line=v;return 0;
}
static bool rmtInit(int,int,int,unsigned frequency){++initCalls;if(frequency!=1000000)throw std::string("wrong tick");active=scenario!="init-fail";return active;}
static bool rmtDeinit(int){if(cleanupBlocked)return false;active=false;armed=false;return true;}
static bool rmtSetRxMaxThreshold(int,unsigned value){return value==10000&&scenario!="config-fail";}
static bool rmtSetRxMinThreshold(int,unsigned value){return value==1;}
static bool rmtReadAsync(int,rmt_data_t* data,size_t* count){
    if(line!=0||*count!=48)throw std::string("receiver not armed before open-drain release");
    ++armCalls;
    if(scenario=="arm-retry-fail"&&armCalls==2)return false;
    output=data;countOutput=count;armed=true;delivered=false;
    armedAt=nowUs;
    if((scenario=="arm-preempted"||scenario=="arm-preempted-dht22"||scenario=="arm-retry-fail")&&armCalls==1)nowUs+=12000;
    if(scenario=="arm-preempted-repeat")nowUs+=12000;
    if(scenario=="arm-delay-short")nowUs+=9000;
    expireHostLow();
    return scenario!="arm-fail";
}
static bool rmtReceiveCompleted(int){
    expireHostLow();
    if(delivered)return true;
    if(line==0)return false;
    if(scenario=="cpu-preempted"&&!interrupted){nowUs+=2000;interrupted=true;}
    if(scenario=="timeout")return false;
    if(nowUs-releasedAt<15000)return false;
    if(!delivered){for(size_t n=0;n<frame.size();++n)output[n]=frame[n];*countOutput=frame.size();delivered=true;}
    return true;
}
#include "dht_rmt_reader.inc"
static void makeFrame(std::array<uint8_t,5> bytes){
    std::vector<std::array<unsigned,2>> pulses;
    if(scenario=="release-prefix")pulses.push_back({1,20});
    else if(scenario!="no-prefix"){pulses.push_back({0,10});pulses.push_back({1,20});}
    pulses.push_back({0,80});pulses.push_back({1,80});
    for(unsigned bit=0;bit<40;++bit){
        if(scenario=="truncated"&&bit==23)break;
        pulses.push_back({0,scenario=="long-pulse"&&bit==9?1028U:50U});
        pulses.push_back({1,(bytes[bit/8]>>(7-bit%8))&1?70U:26U});
    }
    pulses.push_back({0,50});pulses.push_back({1,10000});
    if(scenario=="no-response")pulses={{1,10000}};
    for(size_t n=0;n<pulses.size();n+=2){
        rmt_data_t symbol;symbol.level0=pulses[n][0];symbol.duration0=pulses[n][1];
        if(n+1<pulses.size()){symbol.level1=pulses[n+1][0];symbol.duration1=pulses[n+1][1];}
        frame.push_back(symbol);
    }
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    scenario=argv[1];
    auto type=SensorDriverType::DHT11;
    std::array<uint8_t,5> bytes={55,0,24,0,79};
    float expectedHumidity=55,expectedTemperature=24;
    if(scenario=="dht22"||scenario=="negative"||scenario=="arm-preempted-dht22"){
        type=SensorDriverType::DHT22;bytes={2,43,0,243,32};expectedHumidity=55.5;expectedTemperature=24.3;
        if(scenario=="negative"){bytes={2,43,128,123,40};expectedTemperature=-12.3;}
    }
    if(scenario=="range")bytes={110,0,24,0,134};
    if(scenario=="checksum")bytes[4]^=1;
    if(scenario=="wrap")nowUs=static_cast<uint64_t>(UINT32_MAX)*1000ULL-22000;
    cleanupBlocked=scenario=="cleanup-recovery";
    makeFrame(bytes);
    float humidity=NAN,temperature=NAN;
    DhtReadDiagnostic diagnostic;
    const bool valid=scenario=="valid"||scenario=="dht22"||scenario=="negative"||scenario=="no-prefix"||scenario=="release-prefix"||scenario=="cpu-preempted"||scenario=="wrap"||scenario=="arm-preempted"||scenario=="arm-preempted-dht22"||scenario=="arm-delay-short";
    const auto before=nowUs;
    try{
        bool result=readDhtBounded(10,type,humidity,temperature,&diagnostic);
        std::string phase="ok";
        if(scenario=="stuck-low")phase="idle_low";
        if(scenario=="init-fail")phase="rmt_init";
        if(scenario=="config-fail")phase="rmt_config";
        if(scenario=="arm-fail")phase="rmt_arm";
        if(scenario=="arm-retry-fail")phase="rmt_arm";
        if(scenario=="arm-preempted-repeat")phase="rmt_arm_expired";
        if(scenario=="timeout")phase="rmt_timeout";
        if(scenario=="no-response")phase="rmt_preamble";
        if(scenario=="arm-expiry-after-check")phase="rmt_preamble";
        if(scenario=="truncated")phase="rmt_short_frame";
        if(scenario=="long-pulse")phase="rmt_bit_pulse";
        if(scenario=="checksum")phase="checksum";
        if(scenario=="range")phase="range";
        if(scenario=="cleanup-recovery")phase="rmt_cleanup";
        if(result!=valid||diagnostic.phase!=phase)throw std::string("result/phase mismatch: ")+diagnostic.phase;
        if(valid&&(std::fabs(humidity-expectedHumidity)>.05||std::fabs(temperature-expectedTemperature)>.05))throw std::string("decoded values incorrect");
        if(nowUs-before>62000)throw std::string("read exceeded bounded timeout");
        if((scenario=="arm-preempted"||scenario=="arm-preempted-dht22"||scenario=="arm-preempted-repeat"||scenario=="arm-retry-fail")&&armCalls!=2)throw std::string("expired RX must be retried exactly once");
        if(scenario=="arm-delay-short"&&armCalls!=1)throw std::string("unexpired RX was retried");
        if(scenario=="arm-expiry-after-check"&&armCalls!=1)throw std::string("late expiry was hidden by an extra retry");
        if(scenario=="cleanup-recovery"){
            const auto calls=initCalls;
            if(readDhtBounded(10,type,humidity,temperature,&diagnostic)||initCalls!=calls)throw std::string("failed cleanup allowed new RX");
            cleanupBlocked=false;scenario="valid";
            if(!readDhtBounded(10,type,humidity,temperature,&diagnostic))throw std::string("cleanup recovery failed");
        }
        if(active)throw std::string("channel leaked");
    }catch(const std::string& error){std::cerr<<scenario<<": "<<error<<'\n';return 1;}
    std::cout<<scenario<<" PASS\n";
}
