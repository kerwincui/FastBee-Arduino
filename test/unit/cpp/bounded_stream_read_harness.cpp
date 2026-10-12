#include "peripherals/BoundedStreamRead.h"
#include <cassert>
#include <vector>
struct Stream {
    std::vector<int> bytes;
    size_t index=0;
    int forced=-2;
    unsigned reads=0;
    int available() const { return forced==-2 ? static_cast<int>(bytes.size()-index) : forced; }
    int read() {++reads;return index<bytes.size()?bytes[index++]:-1;}
};
int main() {
    uint8_t buffer[8]={99,99,99,99,99,99,99,99};size_t length=55;
    Stream good{{0,255,128,5}};
    assert(FastBee::readBufferedStream<3>(good,buffer,8,length)&&length==3&&good.reads==3);
    assert(buffer[0]==0&&buffer[1]==255&&buffer[2]==128&&buffer[3]==99);
    Stream empty;length=55;
    assert(FastBee::readBufferedStream<3>(empty,buffer,8,length)&&length==0&&empty.reads==0);
    Stream negative;negative.forced=-1;
    assert(!FastBee::readBufferedStream<3>(negative,buffer,8,length)&&length==0);
    Stream missing{{10}};missing.forced=3;
    buffer[0]=99;
    assert(!FastBee::readBufferedStream<3>(missing,buffer,8,length)&&length==0&&buffer[0]==99);
    for(int value:{-1,256,1000}) {
        Stream bad{{10,value,30}};
        assert(!FastBee::readBufferedStream<3>(bad,buffer,8,length)&&length==0&&buffer[0]==99);
    }
    Stream exact{{1,2,3}};
    assert(FastBee::readExactBufferedStream<4>(exact,buffer,3)&&buffer[0]==1&&buffer[2]==3);
    buffer[0]=99;
    Stream shortRead{{1,2}},extra{{1,2,3,4}};
    assert(!FastBee::readExactBufferedStream<4>(shortRead,buffer,3)&&buffer[0]==99);
    assert(!FastBee::readExactBufferedStream<4>(extra,buffer,3)&&buffer[0]==99);
    Stream invalid{{1,-1,3}};
    assert(!FastBee::readExactBufferedStream<4>(invalid,buffer,3)&&buffer[0]==99);
    Stream stuck;stuck.forced=100000;stuck.bytes.assign(1000,1);
    FastBee::discardBufferedStream(stuck);assert(stuck.reads==255);
    Stream null{{1}};
    assert(!FastBee::readBufferedStream<3>(null,nullptr,8,length)&&null.reads==0);
    assert(!FastBee::readBufferedStream<3>(null,buffer,0,length)&&null.reads==0);
}
