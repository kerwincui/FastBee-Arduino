#include "network/JsonDocumentResponse.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#undef assert
#define assert(condition) do { if (!(condition)) { std::cerr << "assertion failed at " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (0)

class TrackingAllocator : public ArduinoJson::Allocator {
public:
    std::unordered_map<void*,size_t> blocks;
    size_t calls=0;
    void* allocate(size_t size) override {
        ++calls;void* p=std::malloc(size);if(p) blocks[p]=size;return p;
    }
    void deallocate(void* p) override {blocks.erase(p);std::free(p);}
    void* reallocate(void* p,size_t size) override {
        ++calls;blocks.erase(p);void* next=std::realloc(p,size);if(next) blocks[next]=size;return next;
    }
};
int main() {
    TrackingAllocator allocator;
    {
        JsonDocument doc(&allocator);
        doc["success"]=true;
        auto types=doc["types"].to<JsonArray>();
        for(int index=0;index<250;++index) {
            auto type=types.add<JsonObject>();
            type["name"]=std::string("气象数据 \"\\\n")+std::to_string(index);
            type["value"]=index;type["enabled"]=true;
        }
        std::string expected;serializeJson(doc,expected);
        assert(expected.size()>9213);
        const size_t before=allocator.calls;
        FastBee::JsonDocumentResponse response(std::move(doc));
        assert(allocator.calls==before); // ownership transfer has no duplicate pool
        doc.clear(); // caller can leave its scope without invalidating response
        assert(response._sourceValid());
        assert(response._contentLength==expected.size());
        std::string actual;
        uint8_t buffer[1436];
        const size_t capacities[]={1,7,1436,256,13,768};
        size_t iteration=0;
        while(response._sentLength<response._contentLength) {
            const size_t capacity=capacities[iteration++%6];
            const size_t count=response._fillBuffer(buffer,capacity);
            assert(count>0&&count<=capacity);
            actual.append(reinterpret_cast<char*>(buffer),count);
            response._sentLength+=count;
        }
        assert(response._fillBuffer(buffer,sizeof(buffer))==0);
        assert(actual==expected);
        assert(allocator.calls==before); // repeated serialization creates no heap pool
        assert(allocator.blocks.empty()); // last staged bytes do not await TCP ACK/FIN
    }
    assert(allocator.blocks.empty()); // response destruction releases owned document
    std::cout<<"JSON ownership, range, UTF8 and cleanup passed\n";
}
