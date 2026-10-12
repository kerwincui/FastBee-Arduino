// Actual HandlerUtils::sendJsonStream and JSON resource serializers. Transport
// and heap probes are stubs; every JSON allocation failure remains real.
#include "network/RuntimePinResourceJson.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
using String = std::string;
static unsigned failures=0, successPaths=0;
static int responseCode=0;
class AsyncWebServerResponse {};
class AsyncWebServerRequest {
public:
    AsyncWebServerResponse* beginResponse(int code,const char*,const String&) {
        static AsyncWebServerResponse response;responseCode=code;return &response;
    }
};
struct Esp { size_t getMaxAllocHeap() const { return 100000; } } ESP;
namespace FastBee {
class JsonDocumentResponse : public AsyncWebServerResponse {
public: explicit JsonDocumentResponse(JsonDocument&&) {}
};
}
namespace HandlerUtils {
void sendJsonError(AsyncWebServerRequest*,int code,const char*,int) { ++failures;responseCode=code; }
bool sendJsonFromPsramBuffer(AsyncWebServerRequest*,JsonDocument&,size_t) { ++successPaths;responseCode=200;return true; }
bool checkLowMemory(AsyncWebServerRequest*,size_t) { return false; }
bool checkResponseMemory(AsyncWebServerRequest*,size_t) { return false; }
void sendWithClose(AsyncWebServerRequest*,AsyncWebServerResponse*) { ++successPaths; }
#include "resource_send_under_test.inc"
}
class FailingAllocator : public ArduinoJson::Allocator {
public:
    explicit FailingAllocator(unsigned limit) : limit(limit) {}
    unsigned calls=0,limit;
    void* allocate(size_t bytes) override { return calls++>=limit?nullptr:std::malloc(bytes); }
    void deallocate(void* p) override { std::free(p); }
    void* reallocate(void* p,size_t bytes) override { return calls++>=limit?nullptr:std::realloc(p,bytes); }
};
int main() {
    bool earlyFailure=false,lateFailure=false,complete=false;
    for (unsigned limit=0;limit<256;++limit) {
        FailingAllocator allocator(limit);
        JsonDocument doc(&allocator);
        doc["success"]=true;
        JsonObject data=doc["data"]["runtime"].to<JsonObject>();
        FastBee::writeRuntimeResourceHeader(data,"active-board");
        FastBee::I2CResourceSnapshot i2c;i2c.count=16;
        for (uint8_t i=0;i<16;++i) {
            i2c.leases[i]={i,static_cast<uint8_t>(32+i),true};
            std::memset(i2c.leases[i].consumer.id,'A'+i,40);
            i2c.leases[i].consumer.role=FastBee::ResourceConsumerRole::Sensor;
        }
        FastBee::writeI2CResourceJson(data["i2c"].to<JsonObject>(),i2c);
        FastBee::SpiResourceSnapshot spi;spi.count=16;
        for (uint8_t i=0;i<16;++i) {
            std::memset(spi.leases[i].name,'a'+i,40);spi.leases[i].name[40]='\0';
        }
        FastBee::writeSpiResourceJson(data["spi"].to<JsonObject>(),spi);
        FastBee::ControlOutputResourceSnapshot controls;controls.count=4;
        for (auto& lease:controls.leases) std::strcpy(lease.id,"retained-control-resource");
        FastBee::writeControlOutputResourceJson(data["controlOutput"].to<JsonObject>(),controls);
        FastBee::EncoderResourceSnapshot encoders;encoders.count=4;
        for (auto& lease:encoders.leases) std::strcpy(lease.id,"encoder-resource");
        FastBee::writeEncoderResourceJson(data["encoder"].to<JsonObject>(),encoders);
        FastBee::DhtRmtResourceSnapshot dht;dht.supported=true;dht.retainedPin=15;
        FastBee::writeDhtRmtResourceJson(data["dhtRmt"].to<JsonObject>(),dht);
        FastBee::LedcResourceSnapshot ledc;ledc.count=16;
        for (auto& lease:ledc.leases) std::strcpy(lease.id,"native-pwm-resource");
        FastBee::writeLedcResourceJson(data["ledc"].to<JsonObject>(),ledc);
        FastBee::BacklightResourceSnapshot backlight;backlight.available=true;backlight.retained=true;
        FastBee::writeBacklightResourceJson(data["backlightLedc"].to<JsonObject>(),backlight,42);
        FastBee::UartResourceSnapshot uart;uart.portCount=3;uart.generations[1]=5;uart.generations[2]=6;
        uart.consumers[1].assign(FastBee::ResourceConsumerRole::Peripheral,"uart-configured-consumer");
        FastBee::writeUartResourceJson(data["uart"].to<JsonObject>(),uart);
        FastBee::GpioResourceSnapshot gpio;gpio.count=FastBee::GpioResourceSnapshot::CAPACITY;
        for(auto& lease:gpio.leases) lease.consumer.assign(FastBee::ResourceConsumerRole::Peripheral,"gpio-resource-owner");
        FastBee::writeGpioResourceJson(data["gpio"].to<JsonObject>(),gpio);
        FastBee::DacResourceSnapshot dac;dac.supported=true;dac.count=2;
        FastBee::writeDacResourceJson(data["dac"].to<JsonObject>(),dac);
        FastBee::AdcResourceSnapshot adc;adc.observed=true;adc.initialized=true;
        adc.consumerCount=FastBee::AdcResourceSnapshot::CONSUMER_CAPACITY;
        adc.unitCount=FastBee::AdcResourceSnapshot::UNIT_CAPACITY;
        adc.channelCount=FastBee::AdcResourceSnapshot::CHANNEL_CAPACITY;
        for(auto& lease:adc.consumers) lease.identity.assign(FastBee::ResourceConsumerRole::Sensor,"analog-resource-owner");
        FastBee::writeAdcResourceJson(data["adc"].to<JsonObject>(),adc);
        FastBee::NeoPixelResourceSnapshot neo; neo.supported=true; neo.objectAllocated=true;
        neo.consumer.assign(FastBee::ResourceConsumerRole::Peripheral,"neopixel-owner");
        FastBee::writeNeoPixelResourceJson(data["neopixel"].to<JsonObject>(),neo);
        FastBee::StepperResourceSnapshot stepper; stepper.count=FastBee::StepperResourceSnapshot::CAPACITY;
        for(auto& lease:stepper.leases) lease.consumer.assign(FastBee::ResourceConsumerRole::Peripheral,"stepper-owner");
        FastBee::writeStepperResourceJson(data["stepper"].to<JsonObject>(),stepper);
        const bool overflowed=doc.overflowed();
        const bool successFlag=doc["success"]==true;
        failures=0;successPaths=0;responseCode=0;
        AsyncWebServerRequest request;
        HandlerUtils::sendJsonStream(&request,doc);
        if (overflowed) {
            earlyFailure=earlyFailure||!successFlag;lateFailure=lateFailure||successFlag;
            assert(failures==1 && responseCode==503 && successPaths==0);
        } else {
            assert(failures==0 && successPaths==1 && responseCode==200);
            complete=true;break;
        }
    }
    assert(earlyFailure && lateFailure && complete);
}
