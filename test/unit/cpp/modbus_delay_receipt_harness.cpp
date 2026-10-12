#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <array>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
constexpr int pdTRUE = 1, MODBUS_MASTER = 1;
struct FakeSemaphore { bool held = false; unsigned attempts = 0, releases = 0; };
using SemaphoreHandle_t = FakeSemaphore*;
static int xSemaphoreTake(SemaphoreHandle_t lock, unsigned wait) {
    assert(wait == 0); ++lock->attempts;
    if (lock->held) return 0;
    lock->held = true; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) { assert(lock->held); lock->held=false; ++lock->releases; }
static int64_t clockMicros = 1000000;
static int64_t esp_timer_get_time() { return clockMicros; }
namespace Protocols { constexpr size_t MODBUS_ONESHOT_BUFFER_SIZE = 128, MODBUS_BUFFER_SIZE = 256, MODBUS_MAX_COIL_DELAY_TASKS = 8; }
// @instance@
// @types@
// Managed producers have separate actual-PM integration coverage.
struct LegacyManagedDelayRecord : CoilDelayTask { bool running=false,hardwareOnly=false; };
struct ModbusHandler {
    struct Device { uint8_t slaveAddress=0; uint16_t batchRegister=0; };
    struct Configuration { int mode=MODBUS_MASTER; struct Master { uint8_t deviceCount=0; Device devices[8]; } master; } config;
    FakeSemaphore semaphore;
    SemaphoreHandle_t _oneShotSemaphore = &semaphore;
    bool isInitialized=true, isOneShotReading=false;
    const char* _lastTravelError=nullptr;
    CoilDelayTask coilDelayTasks[Protocols::MODBUS_MAX_COIL_DELAY_TASKS];
    LegacyManagedDelayRecord ordinaryCoilDelayTasks[Protocols::MODBUS_MAX_COIL_DELAY_TASKS];
    uint64_t _ordinaryDelayAckOperation=0;
    uint32_t _ordinaryDelayAckWindowMs=0;
    void appendOrdinaryDelayStatusLocked(JsonArray) {}
    uint32_t _delayInstance=nextModbusDelayInstance(), _delayConfiguration=1;
    uint64_t _delayWriteSequence=0, _delayNextOperation=0, _delayLastTimeMs=0, _delayArmedSequence=0;
    ModbusDelayReceipt _delayLastWrite;
    uint32_t _delayFailures=0;
    uint64_t _delayFirstFailureOperation=0;
    OneShotError _delayFirstFailure=ONESHOT_SUCCESS;
    enum Fault { None, Timeout, Crc, Exception, ShortEcho, WrongEcho, Travel } fault=None;
    unsigned transportCalls=0;
    std::vector<std::array<uint8_t,8>> frames;
    uint16_t calculateCRC(const uint8_t*, size_t) { return 0xABCD; }
    void invalidateCoilDelayTasksLocked(const char*);
    void trackCoilDelayWriteLocked(const uint8_t*, size_t);
    bool coilDelayTimeLocked(uint64_t&);
    bool addCoilDelayTask(uint8_t,uint16_t,unsigned long,bool);
    bool addCoilDelayTask(uint8_t,uint16_t,unsigned long,bool,bool);
    bool addCoilDelayTask(uint8_t,uint16_t,unsigned long,bool,bool,const ModbusDelayReceipt&,uint64_t* = nullptr);
    void processCoilDelayTasks();
    bool getCoilDelayStatus(JsonObject);
    OneShotResult _executeModbusTransaction(const uint8_t*,size_t,uint8_t,uint16_t,uint8_t,bool);
    OneShotError _executeModbusTransactionRaw(const uint8_t* request,size_t length,uint8_t expected,
        uint8_t* raw,size_t cap,size_t& returned,uint8_t& exception,uint16_t timeout,uint8_t retries,bool bypass) {
        assert(semaphore.held && length == 8 && expected == request[0] && cap >= length);
        assert(timeout == 0 && retries == 0 && bypass); // Real delayed code cannot retry/reacquire.
        if (fault == Travel) { _lastTravelError="TRAVEL_NOT_CONFIRMED"; return ONESHOT_TRAVEL_BLOCKED; }
        trackCoilDelayWriteLocked(request,length);
        ++transportCalls;
        std::array<uint8_t,8> frame{}; std::memcpy(frame.data(),request,8); frames.push_back(frame);
        if (fault == Timeout) return ONESHOT_TIMEOUT;
        if (fault == Crc) return ONESHOT_CRC_ERROR;
        if (fault == Exception) { exception=2; return ONESHOT_EXCEPTION; }
        std::memcpy(raw,request,length); returned=length;
        if (fault == ShortEcho) returned=6;
        if (fault == WrongEcho) raw[3]^=1;
        return ONESHOT_SUCCESS;
    }
    ModbusDelayReceipt initial(uint8_t function=5,uint16_t address=10,uint8_t slave=2) {
        uint8_t frame[8]={slave,function,static_cast<uint8_t>(address>>8),static_cast<uint8_t>(address),0,0,0xCD,0xAB};
        assert(xSemaphoreTake(_oneShotSemaphore,0));
        OneShotResult result=_executeModbusTransaction(frame,8,slave,0,0,true);
        xSemaphoreGive(_oneShotSemaphore);
        return result.delayReceipt;
    }
    void raw(const std::vector<uint8_t>& bytes) {
        assert(xSemaphoreTake(_oneShotSemaphore,0));
        trackCoilDelayWriteLocked(bytes.data(),bytes.size());
        xSemaphoreGive(_oneShotSemaphore);
    }
};
// @methods@
static bool arm(ModbusHandler& handler,const ModbusDelayReceipt& receipt,bool value=false,uint64_t* op=nullptr) {
    return handler.addCoilDelayTask(receipt.slave,receipt.address,100,value,receipt.function==6,receipt,op);
}
static void due(ModbusHandler& handler) { clockMicros+=200000; handler.processCoilDelayTasks(); }
int main(int argc,char**argv) {
    assert(argc==2); const std::string mode=argv[1]; ModbusHandler handler;
    const auto pending=CoilDelayTask::Phase::Pending, retired=CoilDelayTask::Phase::Retired;
    if(mode=="clock64") clockMicros=(static_cast<int64_t>(UINT32_MAX)+100)*1000;
    const auto receipt=handler.initial(mode=="register"?6:5);
    assert(receipt.instance==1 && receipt.writeSequence==1);
    if(mode=="no_receipt") {
        assert(!handler.addCoilDelayTask(2,10,100,false));
        assert(!handler.addCoilDelayTask(2,10,100,false,false));
        assert(!arm(handler,ModbusDelayReceipt{})); return 0;
    }
    if(mode=="wrong_instance" || mode=="wrong_configuration" || mode=="wrong_address" || mode=="wrong_function") {
        auto wrong=receipt;
        if(mode=="wrong_instance") ++wrong.instance;
        if(mode=="wrong_configuration") ++wrong.configuration;
        if(mode=="wrong_address") ++wrong.address;
        if(mode=="wrong_function") wrong.function=6;
        assert(!arm(handler,wrong)); return 0;
    }
    if(mode=="late_arm") { handler.initial(5,11); assert(!arm(handler,receipt)); return 0; }
    if(mode=="cross_handler") { ModbusHandler other; assert(other._delayInstance!=handler._delayInstance); other.initial(); assert(!arm(other,receipt)); return 0; }
    if(mode=="exhaustion") { handler._delayNextOperation=UINT64_MAX; assert(!arm(handler,receipt)); return 0; }
    uint64_t operation=0; assert(arm(handler,receipt,mode=="register",&operation) && operation==1);
    auto& task=handler.coilDelayTasks[0]; assert(task.phase==pending);
    if(mode=="replay") { assert(!arm(handler,receipt)); due(handler); assert(!arm(handler,receipt)); return 0; }
    if(mode=="configuration") {
        handler.semaphore.held=true; handler.invalidateCoilDelayTasksLocked("HANDLER_STOPPED"); handler.semaphore.held=false;
        assert(task.phase==retired && !arm(handler,receipt)); due(handler); assert(handler.transportCalls==1); return 0;
    }
    if(mode=="clock_reverse") { clockMicros-=1; handler.processCoilDelayTasks(); assert(task.phase==retired && handler.transportCalls==1); return 0; }
    if(mode=="busy") {
        handler.semaphore.held=true; clockMicros+=200000; handler.processCoilDelayTasks();
        JsonDocument document; assert(!handler.getCoilDelayStatus(document.to<JsonObject>()));
        assert(document["available"]==false && task.phase==pending && handler.transportCalls==1);
        assert(!arm(handler,receipt)); handler.semaphore.held=false; return 0;
    }
    if(mode=="retire_same_target") handler.initial();
    if(mode=="range_overlap") handler.raw({2,15,0,9,0,2,1,0,0,0});
    if(mode=="range_disjoint") handler.raw({2,15,0,12,0,2,1,0,0,0});
    if(mode=="other_space") handler.initial(6);
    if(mode=="proprietary") handler.raw({2,5,2,0,3,0,0,0});
    if(mode=="broadcast") handler.raw({0,6,0,99,0,1,0,0});
    if(mode=="batch_alias") {
        handler.config.master.deviceCount=1; handler.config.master.devices[0]={2,500};
        handler.initial(6,500);
    }
    if(mode=="retire_same_target" || mode=="range_overlap" || mode=="proprietary" || mode=="broadcast" || mode=="batch_alias") {
        assert(task.phase==retired && !task.attempted); unsigned calls=handler.transportCalls; due(handler);
        assert(handler.transportCalls==calls); return 0;
    }
    if(mode=="one_per_tick" || mode=="queue_full") {
        const unsigned count=mode=="queue_full"?8:2;
        for(unsigned i=1;i<count;++i) assert(arm(handler,handler.initial(5,10+i)));
        if(mode=="queue_full") { auto last=handler.initial(5,100); assert(!arm(handler,last)); assert(handler.transportCalls==9); return 0; }
        const unsigned calls=handler.transportCalls; due(handler); assert(handler.transportCalls==calls+1);
        handler.processCoilDelayTasks(); assert(handler.transportCalls==calls+2); return 0;
    }
    if(mode=="timeout" || mode=="failure_retained") handler.fault=ModbusHandler::Timeout;
    if(mode=="crc") handler.fault=ModbusHandler::Crc;
    if(mode=="exception") handler.fault=ModbusHandler::Exception;
    if(mode=="short_echo") handler.fault=ModbusHandler::ShortEcho;
    if(mode=="wrong_echo") handler.fault=ModbusHandler::WrongEcho;
    if(mode=="travel_blocked") handler.fault=ModbusHandler::Travel;
    const unsigned before=handler.transportCalls; due(handler);
    assert(handler.transportCalls==before+(mode=="travel_blocked"?0:1));
    if(handler.fault!=ModbusHandler::None) {
        assert(task.phase==CoilDelayTask::Phase::ResultUnknown && handler._delayFailures==1);
        assert(task.attempted==(mode!="travel_blocked") && handler._delayFirstFailureOperation==1);
        const unsigned calls=handler.transportCalls; due(handler); assert(handler.transportCalls==calls);
        if(mode=="failure_retained") {
            handler.fault=ModbusHandler::None; assert(arm(handler,handler.initial(5,11))); due(handler);
            assert(task.phase==CoilDelayTask::Phase::ResultUnknown && handler._delayFailures==1);
            JsonDocument document; assert(handler.getCoilDelayStatus(document.to<JsonObject>()));
            assert(document["failureCount"]==1 && document["firstFailureOperation"]=="1");
            assert(document["records"][0]["phase"]=="RESULT_UNKNOWN" && document["records"][1]["phase"]=="ACKNOWLEDGED");
        }
    } else {
        assert(task.phase==CoilDelayTask::Phase::Acknowledged && handler._delayFailures==0);
        assert(handler.frames.back()[1]==receipt.function && handler.frames.back()[5]==(mode=="register"?1:0));
        JsonDocument document; assert(handler.getCoilDelayStatus(document.to<JsonObject>()));
        assert(document["records"][0]["operationId"]=="1" && document["physicalOutputVerified"]==false);
    }
    assert(!handler.semaphore.held && !handler.isOneShotReading);
}
