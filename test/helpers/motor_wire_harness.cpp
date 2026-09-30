#include <Arduino.h>
#include <cassert>
#include <vector>
#include <cstring>
#include "protocols/ModbusReadDiagnostics.h"
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_DEBUGF(...) ((void)0)
namespace Protocols { constexpr unsigned MODBUS_BUFFER_SIZE = 256, MODBUS_ONESHOT_BUFFER_SIZE = 32, MODBUS_MAX_REGISTERS_PER_READ = 125; }
enum OneShotError { ONESHOT_SUCCESS, ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR, ONESHOT_EXCEPTION };
constexpr uint8_t MODBUS_EX_ILLEGAL_FUNCTION = 1, MODBUS_EX_ILLEGAL_DATA_VALUE = 3;
struct OneShotResult { OneShotError error = ONESHOT_SUCCESS; uint16_t count = 0, data[32]{}; uint8_t exceptionCode = 0; };
struct ModbusSubDevice { bool enabled = true; uint8_t slaveAddress = 1; char deviceType[8] = "motor"; uint16_t motorRegs[5] = {0,1,2,5,7}; };
struct ModbusHandler {
    ModbusSubDevice dev;
    std::vector<uint8_t> request;
    bool _controlPending = false, shortReply = false, wrongEcho = false;
    int readLengthDelta = 0;
    uint8_t retries = 255;
    uint16_t timeout = 0;
    uint8_t getSubDeviceCount() { return 1; }
    const ModbusSubDevice& getSubDevice(uint8_t) { return dev; }
    uint16_t calculateCRC(const uint8_t*, size_t);
    OneShotResult readRegistersOnce(uint8_t,uint8_t,uint16_t,uint16_t,bool,uint16_t,uint8_t,String* = nullptr,ModbusReadDiagnostics* = nullptr);
    OneShotResult writeRegisterOnce(uint8_t,uint16_t,uint16_t,bool,uint16_t,uint8_t);
    OneShotResult writeMultipleRegistersOnce(uint8_t,uint16_t,uint16_t,const uint16_t*,bool,uint16_t,uint8_t);
    OneShotResult sendOneShotRequest(const uint8_t* data,size_t len,uint8_t,uint16_t timeoutMs,uint8_t retry) {
        request.assign(data, data + len); retries = retry; timeout = timeoutMs;
        OneShotResult result; result.count = shortReply ? 4 : 8;
        for (unsigned i = 0; i < 6; ++i) result.data[i] = data[i];
        if (wrongEcho) result.data[3] ^= 1;
        return result;
    }
    OneShotResult sendControlRequest(uint8_t* data,size_t len,uint8_t slave,uint16_t timeoutMs,uint8_t retry) {
        return sendOneShotRequest(data,len,slave,timeoutMs,retry);
    }
    OneShotError _sendOneShotRequestRaw(const uint8_t* data,size_t len,uint8_t slave,uint8_t* out,size_t,
                                      size_t& outLen,uint8_t&,uint16_t timeoutMs,uint8_t retry,bool,ModbusReadDiagnostics*) {
        request.assign(data,data+len); retries=retry; timeout=timeoutMs;
        const uint8_t response[] = {slave,data[1],2,0,255,0,0};
        memcpy(out,response,sizeof(response));outLen=sizeof(response)+readLengthDelta;return ONESHOT_SUCCESS;
    }
};
#include "motor_wire_production.inc"
int main() {
    ModbusHandler bus;
    struct Vector { uint16_t reg, value; uint8_t crcLow, crcHigh; };
    // Manufacturer spreadsheet Sheet1 B11:B18 and D17, checked against its
    // complete frames rather than the contradictory introductory examples.
    const Vector writes[] = {{0,1,0x48,0x0A},{1,1,0x19,0xCA},{2,1,0xE9,0xCA},
        {5,255,0xD9,0x8B},{7,255,0x78,0x4B},{4,255,0x88,0x4B},
        {6,2,0xE8,0x0A},{6,8,0x68,0x0D},{10,1,0x68,0x08}};
    for (const auto& v : writes) {
        const auto result = bus.writeRegisterOnce(1,v.reg,v.value,true,777,2);
        assert(result.error==ONESHOT_SUCCESS);
        const std::vector<uint8_t> expected={1,6,0,static_cast<uint8_t>(v.reg),0,static_cast<uint8_t>(v.value),v.crcLow,v.crcHigh};
        assert(bus.request==expected && bus.timeout==777);
        assert(bus.retries==(v.reg<2?0:2));
    }
    const Vector reads[]={{5,0,0x94,0x0B},{7,0,0x35,0xCB},{4,0,0xC5,0xCB},{10,0,0xA4,0x08}};
    for (const auto& v : reads) {
        const auto result=bus.readRegistersOnce(1,3,v.reg,1,true,800,0);
        assert(result.error==ONESHOT_SUCCESS && result.count==1 && result.data[0]==255);
        const std::vector<uint8_t> expected={1,3,0,static_cast<uint8_t>(v.reg),0,1,v.crcLow,v.crcHigh};
        assert(bus.request==expected && bus.timeout==800 && bus.retries==0);
    }
    // A non-YF53 layout with one shared action word and FC16 stop=0.
    for (int delta : {-2, -1, 1}) {
        bus.readLengthDelta = delta;
        ModbusReadDiagnostics trace;
        const auto malformed = bus.readRegistersOnce(8,3,5,1,true,800,0,nullptr,&trace);
        assert(malformed.error == ONESHOT_CRC_ERROR && malformed.count == 0);
        assert(strcmp(trace.reason, "RESPONSE_LENGTH_MISMATCH") == 0);
    }
    bus.readLengthDelta = 0;
    bus.dev.motorRegs[0]=bus.dev.motorRegs[1]=100;
    const uint16_t stop=0;
    auto result=bus.writeMultipleRegistersOnce(1,100,1,&stop,true,900,2);
    assert(result.error==ONESHOT_SUCCESS && bus.request.size()==11 && bus.request[1]==16);
    assert(bus.request[3]==100 && bus.request[5]==1 && bus.request[6]==2 && bus.request[8]==0);
    assert(bus.timeout==900 && bus.retries==0);
    uint16_t data[3]={1,2,3};
    bus.writeMultipleRegistersOnce(1,99,3,data,false,901,2);assert(bus.retries==0);
    bus.writeMultipleRegistersOnce(1,101,1,data,false,901,2);assert(bus.retries==2);
    assert(bus.writeMultipleRegistersOnce(1,65535,2,data,true,900,0).error==ONESHOT_EXCEPTION);
    assert(bus.writeMultipleRegistersOnce(1,0,1,nullptr,true,900,0).error==ONESHOT_EXCEPTION);
    for (unsigned kind=0;kind<2;++kind) {
        bus.shortReply=kind==0;bus.wrongEcho=kind==1;
        assert(bus.writeRegisterOnce(1,5,20,true,900,0).error!=ONESHOT_SUCCESS);
        assert(bus.writeMultipleRegistersOnce(1,100,1,&stop,true,900,0).error!=ONESHOT_SUCCESS);
    }
}
