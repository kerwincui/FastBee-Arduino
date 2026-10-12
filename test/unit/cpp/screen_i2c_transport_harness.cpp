#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #x); std::exit(86); } } while(false)
struct TwoWire {
    unsigned transactions=0, requests=0, readCalls=0, writeCalls=0, shortAt=0;
    unsigned advertised=4, actual=4, cursor=0;
    int negativeAt=-1, invalidAt=-1, txStatus=0;
    bool stuck=false, negativeAvailable=false;
    std::vector<uint8_t> sent;
    std::vector<bool> stops;
    void beginTransmission(uint8_t) { ++transactions; sent.clear(); }
    size_t write(uint8_t byte) { return write(&byte,1); }
    size_t write(const uint8_t* data, size_t length) {
        const size_t count=++writeCalls==shortAt ? length-1 : length;
        sent.insert(sent.end(),data,data+count); return count;
    }
    int endTransmission(bool stop=true) { stops.push_back(stop); return txStatus; }
    size_t requestFrom(uint8_t, uint8_t) { ++requests; cursor=0; return advertised; }
    int available() {
        if(negativeAvailable) return -1;
        return stuck ? 1 : static_cast<int>(actual-cursor);
    }
    int read() {
        CHECK(++readCalls<=96); // Old drain loop fails deterministically, never hangs.
        if(static_cast<int>(cursor)==negativeAt) return -1;
        if(static_cast<int>(cursor)==invalidAt) { ++cursor; return 256; }
        if(stuck) return 0;
        return cursor<actual ? static_cast<int>(0x30+cursor++) : -1;
    }
} wire;
bool availableLock=true, availableLease=true, availableAddress=true;
unsigned heldLocks=0;
class I2CBusLock {
    bool _locked;
public:
    explicit I2CBusLock(uint32_t=1000):_locked(availableLock) { if(_locked) ++heldLocks; }
    ~I2CBusLock() { if(_locked) --heldLocks; }
    bool locked() const { return _locked; }
};
class I2CBusManager {
public:
    static I2CBusManager& instance() { static I2CBusManager m; return m; }
    TwoWire* wire(const void*) { return availableLease ? &::wire : nullptr; }
    bool ownsAddress(const void*,uint8_t) const { return availableLease && availableAddress; }
};
struct Gt {
    static constexpr uint8_t GT911_ADDRESS_PRIMARY=0x5d, GT911_ADDRESS_SECONDARY=0x14;
    bool _leased=true;
    bool _alternateLeased=true;
    uint8_t _alternateOwner=0;
    uint8_t _address=0x14;
    GT_METHODS
};
struct Rgb {
    struct { uint8_t controlAddress=0x24; } _config;
    bool _ch32=true;
    uint8_t _ch32Direction=0xa5, _ch32Output=0x5a;
    RGB_METHODS
};
enum class IoExpanderResult { Ok, Busy, Error };
struct IoExpanderProvider {
    uint8_t _address=0x20;
    IoExpanderResult writeByte(uint8_t value);
    IoExpanderResult readByte(uint8_t& value);
};
int main(int argc, char** argv) {
    CHECK(argc==2);
    std::string mode=argv[1];
    Gt gt; Rgb rgb; IoExpanderProvider provider;
    uint8_t data[32]; std::memset(data,0xa5,sizeof(data));
    uint8_t value=0xa5;
    if(mode.find("gt_")==0) {
        if(mode=="gt_success") {
            for(unsigned size : {1U,4U,32U}) {
                wire.actual=wire.advertised=size;
                CHECK(gt.readRegister(0x8140,data,size));
                for(unsigned i=0;i<size;++i) CHECK(data[i]==0x30+i);
                CHECK(!wire.stops.back() && wire.sent==std::vector<uint8_t>({0x81,0x40}));
            }
            CHECK(gt.writeRegister(0x8047,data,29));
            CHECK(wire.sent.size()==31 && wire.stops.back());
        } else if(mode=="gt_invalid") {
            CHECK(!gt.readRegister(0x8140,nullptr,4));
            CHECK(!gt.readRegister(0x8140,data,0));
            CHECK(!gt.readRegister(0x8140,data,33));
            CHECK(!gt.writeRegister(0x8140,nullptr,4));
            CHECK(!gt.writeRegister(0x8140,data,0));
            CHECK(!gt.writeRegister(0x8140,data,30));
            gt._leased=false; CHECK(!gt.readRegister(0x8140,data,4));
            CHECK(!gt.writeRegister(0x8140,data,4));
            gt._leased=true; gt._address=0; CHECK(!gt.readRegister(0x8140,data,4));
            CHECK(wire.transactions==0 && wire.requests==0);
        } else if(mode.find("gt_write_")==0) {
            if(mode=="gt_write_short_first") wire.shortAt=1;
            if(mode=="gt_write_short_second") wire.shortAt=2;
            if(mode=="gt_write_short_payload") wire.shortAt=3;
            if(mode=="gt_write_nack") wire.txStatus=2;
            CHECK(!gt.writeRegister(0x8047,data,4));
            CHECK(wire.requests==0);
        } else {
            if(mode=="gt_pointer_short_first") wire.shortAt=1;
            if(mode=="gt_pointer_short_second") wire.shortAt=2;
            if(mode=="gt_pointer_nack") wire.txStatus=2;
            if(mode=="gt_count_short") wire.advertised=2;
            if(mode=="gt_count_excess") wire.advertised=5;
            if(mode=="gt_missing") wire.actual=2;
            if(mode=="gt_negative_first") wire.negativeAt=0;
            if(mode=="gt_negative_middle") wire.negativeAt=2;
            if(mode=="gt_negative_last") wire.negativeAt=3;
            if(mode=="gt_invalid_byte") wire.invalidAt=2;
            if(mode=="gt_negative_available") wire.negativeAvailable=true;
            if(mode=="gt_bounded_drain") { wire.advertised=0; wire.stuck=true; }
            if(mode=="gt_drain_negative") { wire.advertised=0; wire.negativeAt=0; }
            if(mode=="gt_busy") availableLock=false;
            if(mode=="gt_lost_lease") availableLease=false;
            if(mode=="gt_lost_address") availableAddress=false;
            CHECK(!gt.readRegister(0x8140,data,4));
            for(uint8_t byte : data) CHECK(byte==0xa5);
            if(mode.find("gt_pointer_")==0) CHECK(wire.requests==0);
            if(mode=="gt_busy" || mode=="gt_lost_lease" || mode=="gt_lost_address") {
                CHECK(!gt.writeRegister(0x8047,data,4));
                CHECK(wire.transactions==0);
            }
            CHECK(wire.readCalls<=32);
        }
    } else if(mode.find("rgb_")==0) {
        wire.actual=wire.advertised=1;
        if(mode=="rgb_success") {
            CHECK(rgb.readLocked(&wire,4,value) && value==0x30);
            CHECK(rgb.writeLocked(&wire,3,0x20) && wire.stops.back());
            CHECK(wire.sent==std::vector<uint8_t>({3,0x20}));
            CHECK(rgb.updateRegister(3,0x0f,3) && rgb._ch32Output==0x53);
        } else if(mode.find("rgb_write_")==0) {
            if(mode=="rgb_write_short_first") wire.shortAt=1;
            if(mode=="rgb_write_short_second") wire.shortAt=2;
            if(mode=="rgb_write_nack") wire.txStatus=2;
            CHECK(!rgb.writeLocked(&wire,3,0x20));
        } else if(mode=="rgb_shadow_short") {
            wire.shortAt=2;
            CHECK(!rgb.updateRegister(3,0x0f,3) && rgb._ch32Output==0x5a);
        } else if(mode=="rgb_rmw_missing") {
            rgb._ch32=false; wire.actual=0;
            CHECK(!rgb.updateRegister(1,0x0f,3));
            CHECK(wire.writeCalls==1); // Failed read must never feed a masked write.
        } else if(mode=="rgb_busy" || mode=="rgb_lost_lease" || mode=="rgb_lost_address") {
            if(mode=="rgb_busy") availableLock=false;
            else if(mode=="rgb_lost_address") availableAddress=false;
            else availableLease=false;
            CHECK(!rgb.writeRegister(5,128));
            CHECK(!rgb.updateRegister(3,0x0f,3) && rgb._ch32Output==0x5a);
            CHECK(wire.transactions==0);
        } else {
            if(mode=="rgb_pointer_short") wire.shortAt=1;
            if(mode=="rgb_pointer_nack") wire.txStatus=2;
            if(mode=="rgb_count_short") wire.advertised=0;
            if(mode=="rgb_count_excess") wire.advertised=2;
            if(mode=="rgb_missing") wire.actual=0;
            if(mode=="rgb_negative") wire.negativeAt=0;
            if(mode=="rgb_invalid_byte") wire.invalidAt=0;
            if(mode=="rgb_negative_available") wire.negativeAvailable=true;
            if(mode=="rgb_bounded_drain") { wire.advertised=0; wire.stuck=true; }
            CHECK(!rgb.readLocked(&wire,4,value) && value==0xa5);
            if(mode.find("rgb_pointer_")==0) CHECK(wire.requests==0);
            CHECK(wire.readCalls<=32);
        }
    } else {
        wire.actual=wire.advertised=1;
        if(mode=="provider_success") {
            CHECK(provider.readByte(value)==IoExpanderResult::Ok && value==0x30);
            CHECK(provider.writeByte(0x80)==IoExpanderResult::Ok);
        } else if(mode=="provider_short_write") {
            wire.shortAt=1;
            CHECK(provider.writeByte(0x80)==IoExpanderResult::Error);
        } else if(mode=="provider_busy" || mode=="provider_lost_lease") {
            if(mode=="provider_busy") availableLock=false; else availableLease=false;
            CHECK(provider.readByte(value)!=IoExpanderResult::Ok && value==0xa5);
            CHECK(provider.writeByte(0x80)!=IoExpanderResult::Ok);
            CHECK(wire.transactions==0 && wire.requests==0);
        } else {
            if(mode=="provider_negative") wire.negativeAt=0;
            if(mode=="provider_invalid_byte") wire.invalidAt=0;
            if(mode=="provider_missing") wire.actual=0;
            CHECK(provider.readByte(value)==IoExpanderResult::Error && value==0xa5);
        }
    }
    CHECK(heldLocks==0);
    std::printf("transport %s passed\n",argv[1]);
}
