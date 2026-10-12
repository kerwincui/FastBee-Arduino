#include "peripherals/drivers/DS1302Driver.h"
#include "core/FeatureInstanceRegistry.h"
#include <cassert>
#include <cstring>
static DS1302DateTime value() { return {24,2,29,12,34,56,4}; }
static void calendar() {
    auto dt=value(); assert(dt.isValid());
    dt.year=23; assert(!dt.isValid()); dt.year=0; assert(dt.isValid());
    dt.month=4; dt.day=31; assert(!dt.isValid());
    for(auto bad:{0,13,255}) {dt=value();dt.month=bad;assert(!dt.isValid());}
    for(auto bad:{0,32,255}) {dt=value();dt.day=bad;assert(!dt.isValid());}
    for(auto bad:{0,8,255}) {dt=value();dt.dayOfWeek=bad;assert(!dt.isValid());}
    dt=value();dt.year=100;assert(!dt.isValid());
    dt=value();dt.hour=24;assert(!dt.isValid());
    dt=value();dt.minute=60;assert(!dt.isValid());
    dt=value();dt.second=60;assert(!dt.isValid());
}
static void formatting() {
    auto dt=value();
    assert(dt.toString()=="2024-02-29 12:34:56");
    assert(dt.toString("YYYY/MM/DD HH:nn:SS")=="2024/02/29 12:34:56");
    assert(dt.toString("HH:MM:SS")=="12:34:56");
    assert(dt.toString("YYYY-MM-DD")=="2024-02-29");
    assert(dt.toString(nullptr).isEmpty());
    dt.month=0; assert(dt.toString().isEmpty());
}
static void timestamps() {
    DS1302DateTime dt{0,1,1,0,0,0,6};
    assert(dt.toUnixTimestamp()==946684800UL);
    dt.month=3;assert(dt.toUnixTimestamp()==946684800UL+60UL*86400UL);
    dt={99,12,31,23,59,59,4};assert(dt.toUnixTimestamp()==4102444799UL);
    for(int month=0;month<256;++month){
        dt=value();dt.month=static_cast<uint8_t>(month);
        if(month<1||month>12)assert(dt.toUnixTimestamp()==0);
    }
    dt=value();dt.day=0;assert(dt.toUnixTimestamp()==0);
    dt=value();dt.year=255;assert(dt.toUnixTimestamp()==0);
}
static void readHours() {
    auto& rtc=DS1302Driver::getInstance();assert(rtc.begin(5,6,7));
    const uint8_t registers[]={0x92,0xb2,0x81,0xa1,0x91,0xb1,0x00,0x23};
    const uint8_t expected[]={0,12,1,13,11,23,0,23};
    for(size_t i=0;i<sizeof(registers);++i){
        rtcLoad({0x56,0x34,registers[i],0x29,0x02,0x04,0x24,0x00});
        auto dt=rtc.readDateTime(); assert(dt.isValid() && dt.hour==expected[i]);
        assert(dt.minute==34 && dt.second==56 && rtcBitIndex==64);
    }
    rtcLoad({0,0,0x80,0x29,0x02,0x04,0x24,0});
    assert(!rtc.readDateTime().isValid() && rtcBitIndex==64);rtc.end();
}
static void invalidRead() {
    auto& rtc=DS1302Driver::getInstance();assert(!rtc.readDateTime().isValid());
    assert(rtc.begin(5,6,7));
    rtcLoad({0,0,0x12,0x29,0x02,0x03,0x23,0});
    auto dt=rtc.readDateTime();assert(!dt.isValid() && dt.month==0);
    assert(dt.toString().isEmpty() && dt.toUnixTimestamp()==0);
    rtcLoad({0,0,0x12,0x31,0x04,0x01,0x24,0});assert(!rtc.readDateTime().isValid());
    rtcLoad({0,0,0x12,0x29,0x02,0,0x24,0});assert(!rtc.readDateTime().isValid());
    rtcLoad({255,255,255,255,255,255,255,255});assert(!rtc.readDateTime().isValid());
    rtcLoad({0x1a,0,0x12,0x29,0x02,0x01,0x24,0});assert(!rtc.readDateTime().isValid());
    rtcLoad({0,0,0x12,0x29,0x0a,0x01,0x24,0});assert(!rtc.readDateTime().isValid());
    rtc.end();
}
static void modeConversion() {
    auto& rtc=DS1302Driver::getInstance();assert(rtc.begin(5,6,7));
    auto bcd=[](int value){return static_cast<uint8_t>(((value/10)<<4)|(value%10));};
    for(int hour=0;hour<24;++hour){
        const auto encoded24=bcd(hour);
        const auto encoded12=static_cast<uint8_t>(0x80|(hour>=12?0x20:0)|bcd(hour%12==0?12:hour%12));
        rtcLoad({encoded24});rtc.set24HourMode(false);
        assert((rtcOutputBytes()==std::vector<uint8_t>{0x85,0x84,encoded12}));
        rtcLoad({encoded12});rtc.set24HourMode(true);
        assert((rtcOutputBytes()==std::vector<uint8_t>{0x85,0x84,encoded24}));
        rtcLoad({encoded24});rtc.set24HourMode(true);
        assert((rtcOutputBytes()==std::vector<uint8_t>{0x85}));
        rtcLoad({encoded12});rtc.set24HourMode(false);
        assert((rtcOutputBytes()==std::vector<uint8_t>{0x85}));
    }
    for(auto bad:{0x24,0x1a,0x80,0x93,0xff}) {
        rtcLoad({static_cast<uint8_t>(bad)});rtc.set24HourMode(true);
        assert((rtcOutputBytes()==std::vector<uint8_t>{0x85}));
    }
    rtc.end();
}
static void writeGuard() {
    auto& rtc=DS1302Driver::getInstance();auto dt=value();
    int before=rtcWrites;assert(!rtc.setDateTime(dt) && rtcWrites==before);
    assert(rtc.begin(5,6,7));before=rtcWrites;
    dt.year=23;assert(!rtc.setDateTime(dt));
    dt=value();dt.dayOfWeek=0;assert(!rtc.setDateTime(dt));
    dt=value();dt.year=100;assert(!rtc.setDateTime(dt));
    assert(rtcWrites==before);dt=value();assert(rtc.setDateTime(dt) && rtcWrites>before);
    rtc.end();
}
static void observation() {
    using namespace FastBee;
    auto& registry = FeatureInstanceRegistry::instance();
    assert(registry.observe("ds1302").state == FeatureInstanceState::Unobserved);
    auto& rtc = DS1302Driver::getInstance();
    int ioCalls = 0; static int* counter = nullptr; counter = &ioCalls;
    rtcIoObserver = [] { ++*counter; };
    auto check = [&](FeatureInstanceState state, unsigned count) {
        const int before = ioCalls;
        for (int i = 0; i < 1000; ++i) {
            auto snapshot = registry.observe("ds1302");
            assert(snapshot.state == state && snapshot.initializedCount == count);
        }
        assert(ioCalls == before);
    };
    check(FeatureInstanceState::NotInitialized, 0);
    assert(!rtc.begin(255, 6, 7)); check(FeatureInstanceState::NotInitialized, 0);
    assert(rtc.begin(5, 6, 7)); check(FeatureInstanceState::Initialized, 1);
    rtc.end(); check(FeatureInstanceState::NotInitialized, 0);
    assert(rtc.begin(5, 6, 7)); check(FeatureInstanceState::Initialized, 1);
    rtc.end(); rtcIoObserver = nullptr; counter = nullptr;
}
int main(int argc,char** argv){
    assert(argc==2);
    if(!std::strcmp(argv[1],"calendar"))calendar();
    else if(!std::strcmp(argv[1],"format"))formatting();
    else if(!std::strcmp(argv[1],"timestamp"))timestamps();
    else if(!std::strcmp(argv[1],"read-hours"))readHours();
    else if(!std::strcmp(argv[1],"read-invalid"))invalidRead();
    else if(!std::strcmp(argv[1],"write-guard"))writeGuard();
    else if(!std::strcmp(argv[1],"mode"))modeConversion();
    else if(!std::strcmp(argv[1],"observation"))observation();
    else assert(false);
}
