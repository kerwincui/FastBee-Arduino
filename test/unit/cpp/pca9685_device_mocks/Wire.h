#pragma once
#include <Arduino.h>
#include <array>

// Register/transport seam only. The shared bus manager and device are real.
class TwoWire {
public:
    std::array<uint8_t,256> registers{};
    unsigned transmissions=0,requests=0,reads=0,starts=0,ends=0,writeLimit=255;
    int nextByte=-2,availableBytes=0;
    bool ack=true,shortRead=false,emptyRead=false,extraRead=false,infinite=false;
    bool applyFailedWrite=false;
    int nackDataRegister=-1; // Fail this data write, after optional register effects.
    uint8_t pointer=0;
    std::vector<uint8_t> buffer,addresses;
    std::vector<bool> stops;
    TwoWire() {
        registers[0]=0x11; registers[1]=4; registers[2]=0xe2;
        registers[3]=0xe4; registers[4]=0xe8; registers[5]=0xe0; registers[0xfe]=0x1e;
    }
    bool begin(int,int) {++starts;return true;}
    void end() {++ends;}
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t address) {++transmissions;addresses.push_back(address);buffer.clear();}
    size_t write(uint8_t value) {return write(&value,1);}
    size_t write(const uint8_t* bytes,size_t size) {
        const size_t count=std::min<size_t>(size,writeLimit);
        buffer.insert(buffer.end(),bytes,bytes+count);return count;
    }
    uint8_t endTransmission(bool stop=true) {
        stops.push_back(stop);
        const bool accepted=ack && !(buffer.size()>1 && buffer[0]==nackDataRegister);
        if ((!accepted && !applyFailedWrite) || buffer.empty()) return accepted ? 0 : 4;
        pointer=buffer[0];
        for(size_t i=1;i<buffer.size();++i) {
            const uint8_t reg=static_cast<uint8_t>(pointer+i-1);
            if(reg>=0xfa && reg<=0xfd) {
                for(unsigned channel=0;channel<16;++channel) registers[6+channel*4+reg-0xfa]=buffer[i];
            } else registers[reg]=buffer[i];
        }
        return accepted ? 0 : 4;
    }
    size_t requestFrom(uint8_t address,uint8_t count) {
        addresses.push_back(address);++requests;
        availableBytes=emptyRead ? 0 : static_cast<int>(count)+(extraRead ? 1 : 0);
        return shortRead ? count-1 : count;
    }
    int available() {return infinite ? 1 : availableBytes;}
    int read() {
        ++reads;if(!infinite && availableBytes>0)--availableBytes;
        if(nextByte!=-2)return nextByte;
        return registers[pointer++];
    }
};
inline TwoWire Wire;
