#pragma once
#include "../../../mocks/Arduino.h"
#include <cassert>
constexpr int INPUT=0,OUTPUT=1;
inline int rtcWrites=0;
inline std::vector<int> rtcBits;
inline size_t rtcBitIndex=0;
inline bool rtcIoOutput=false;
inline int rtcDataLevel=0;
inline std::vector<int> rtcOutputBits;
inline void (*rtcIoObserver)() = nullptr;
inline void pinMode(uint8_t pin,int mode) {
    if(rtcIoObserver)rtcIoObserver();
    if(pin==6)rtcIoOutput=mode==OUTPUT;
}
inline void digitalWrite(uint8_t pin,int value) {
    if(rtcIoObserver)rtcIoObserver();
    ++rtcWrites;
    if(pin==6)rtcDataLevel=value;
    if(pin==7 && value==HIGH && rtcIoOutput)rtcOutputBits.push_back(rtcDataLevel);
}
inline int digitalRead(uint8_t) {
    if(rtcIoObserver)rtcIoObserver();
    assert(rtcBitIndex<rtcBits.size()); return rtcBits[rtcBitIndex++];
}
inline void rtcDelayMicroseconds(unsigned int) {}
#define delayMicroseconds rtcDelayMicroseconds
inline void rtcLoad(std::initializer_list<uint8_t> bytes) {
    rtcBits.clear(); rtcOutputBits.clear(); rtcBitIndex=0;
    for(auto byte:bytes) for(int bit=0;bit<8;++bit) rtcBits.push_back((byte>>bit)&1);
}
inline std::vector<uint8_t> rtcOutputBytes() {
    assert(rtcOutputBits.size()%8==0);std::vector<uint8_t> bytes;
    for(size_t i=0;i<rtcOutputBits.size();i+=8){
        uint8_t value=0;for(int bit=0;bit<8;++bit)value|=rtcOutputBits[i+bit]<<bit;
        bytes.push_back(value);
    }
    return bytes;
}
