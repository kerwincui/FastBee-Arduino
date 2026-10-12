#pragma once
#include <Arduino.h>
struct TwoWire {
    uint8_t registers[65536]{};
    uint8_t address=0,present=0x14;
    uint16_t reg=0,readAt=0;
    size_t remaining=0;
    unsigned transmissions=0,requests=0,writes=0,begins=0,ends=0;
    std::vector<uint8_t> sent;
    bool begin(int8_t,int8_t) {++begins;return true;}
    void end() {++ends;}
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t addr) {address=addr;sent.clear();++transmissions;}
    size_t write(uint8_t byte) {sent.push_back(byte);return 1;}
    size_t write(const uint8_t* data,size_t n) {sent.insert(sent.end(),data,data+n);return n;}
    int endTransmission(bool=true) {
        if(address!=present || sent.size()<2) return 2;
        reg=(sent[0]<<8)|sent[1];
        for(size_t i=2;i<sent.size();++i) {registers[reg+i-2]=sent[i];++writes;}
        return 0;
    }
    size_t requestFrom(uint8_t addr,uint8_t count) {
        ++requests;remaining=addr==present?count:0;readAt=reg;return remaining;
    }
    int available() {return static_cast<int>(remaining);}
    int read() {if(!remaining)return -1;--remaining;return registers[readAt++];}
};
inline TwoWire Wire;
