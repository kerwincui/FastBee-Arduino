#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/JsonFragmentsResponse.h"
#include <cassert>
#include <cstdio>
#include <iostream>

struct HealthMonitor {bool isMemoryCritical() const {return false;}};
struct FastBeeFramework {
    static FastBeeFramework* getInstance() {static FastBeeFramework f;return &f;}
    HealthMonitor* getHealthMonitor() {static HealthMonitor h;return &h;}
};
namespace FastBee {
bool psramAvailableForJson(size_t) {return false;}
namespace MemoryBudget {size_t dramFree(){return 99999;}size_t dramLargest(){return 99999;}}
}
struct AsyncWebServerRequest {
    std::unique_ptr<AsyncAbstractResponse> response;
    int status=0;
    void send(AsyncAbstractResponse* value) {response.reset(value);status=200;}
};
void noteWebRequestActivity() {}
bool canAllocateResponse(size_t) {return true;}
void sendServiceUnavailable(AsyncWebServerRequest* request) {request->status=503;}
struct WebHandlerContext {void sendSuccess(AsyncWebServerRequest*,const JsonDocument&);};
#include "success_under_test.inc"

// Legacy serialization-buffer ownership, independent of the new response.
size_t legacyPeak(const JsonDocument& data) {
    stringPeak=stringBytes;
    String dataStr;dataStr.reserve(measureJson(data)+1);serializeJson(data,dataStr);
    String envelope;envelope.reserve(dataStr.length()+64);
    envelope="{\"success\":true,\"timestamp\":";
    envelope+=String(millis());envelope+=",\"data\":";envelope+=dataStr;envelope+="}";
    String basicResponseCopy(envelope);
    return stringPeak;
}
std::string drain(AsyncAbstractResponse& response) {
    std::string output;uint8_t buffer[37];
    while(output.size()<response._contentLength) {
        size_t count=response._fillBuffer(buffer,sizeof(buffer));assert(count>0);
        output.append(reinterpret_cast<char*>(buffer),count);response._sentLength+=count;
    }
    return output;
}
int main() {
    WebHandlerContext ctx;
    for(bool nullData:{false,true}) {
        JsonDocument doc;
        if(!nullData){doc["label"]="中文 / \"test\"";doc["values"][0]=std::string(8000,'x');doc["values"][1]=nullptr;}
        std::string expected="{\"success\":true,\"timestamp\":4294967295,\"data\":";
        std::string payload;serializeJson(doc,payload);expected+=payload;expected+="}";
        const auto before=stringBytes;
        const auto oldPeak=legacyPeak(doc);
        assert(stringBytes==before);
        stringPeak=stringBytes;
        AsyncWebServerRequest request;ctx.sendSuccess(&request,doc);
        assert(request.status==200&&request.response&&request.response->_sourceValid());
        size_t peak=stringPeak;
        doc.clear(); // Caller may destroy/change its DOM before response reads.
        assert(drain(*request.response)==expected);
        request.response.reset();assert(stringBytes==before);
        if(!nullData) {assert(peak*2<oldPeak);std::cout<<"serialized String capacity peak "<<peak<<" vs legacy "<<oldPeak<<" bytes\n";}
    }
    for(int fault=0;fault<4;++fault) {
        JsonDocument doc;doc["x"]=std::string(8000,'x');
        failReserve=fault==0;failArray=fault==1;failResponse=fault==2;failConcatAfter=fault==3?2:-1;
        AsyncWebServerRequest request;ctx.sendSuccess(&request,doc);
        assert(request.status==503&&!request.response&&stringBytes==0);
        failReserve=failArray=failResponse=false;failConcatAfter=-1;
    }
    {
        struct FailingAllocator:ArduinoJson::Allocator {
            void* allocate(size_t) override {return nullptr;}
            void deallocate(void*) override {}
            void* reallocate(void*,size_t) override {return nullptr;}
        } allocator;
        JsonDocument doc(&allocator);doc["value"]="cannot allocate";assert(doc.overflowed());
        AsyncWebServerRequest request;ctx.sendSuccess(&request,doc);
        assert(request.status==503&&!request.response&&stringBytes==0);
    }
    {
        JsonDocument doc;doc["value"]="cancel before first read";
        AsyncWebServerRequest request;ctx.sendSuccess(&request,doc);
        assert(request.status==200);request.response.reset();assert(stringBytes==0);
    }
    std::cout<<"success envelope: content/null/escaping/ownership/OOM/cancel passed\n";
}
