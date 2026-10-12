#define millis unused_ordinary_millis
#define delay unused_ordinary_delay
#define yield unused_ordinary_yield
#include <Arduino.h>
#undef millis
#undef delay
#undef yield
#include "core/ModbusOrdinaryControl.h"
#include "core/UartReceiveGeneration.h"
#include "core/UartReceiveFault.h"
#include "protocols/ModbusReadDiagnostics.h"
#include "protocols/MotorProtocol.h"
#include "protocols/ModbusMotorReference.h"
#include <ArduinoJson.h>
#include <array>
#include <cassert>
#include <functional>
#include <new>
#include <thread>
#include <mutex>
#include <future>
#include <iostream>
#include <chrono>
static bool failOrdinaryStorage = false;
static size_t ordinaryStorageAttempts = 0;
void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    if (bytes == sizeof(FastBee::ModbusOrdinarySlot) * FastBee::ModbusOrdinarySlots::CAPACITY) {
        ++ordinaryStorageAttempts;
        if (failOrdinaryStorage) return nullptr;
    }
    return (std::malloc)(bytes);
}
#define FASTBEE_ENABLE_MODBUS 1
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
constexpr int pdTRUE = 1, MODBUS_MASTER = 1;
constexpr unsigned portMAX_DELAY=UINT32_MAX;
namespace Protocols { constexpr size_t MODBUS_ONESHOT_BUFFER_SIZE=128, MODBUS_BUFFER_SIZE=256, MODBUS_MAX_COIL_DELAY_TASKS=8; }
namespace FastBee { struct UartPortLease { static constexpr uint8_t PORT_COUNT=3; }; }
namespace FastBee { struct MemoryBudget { static uint32_t dramFree() { return 0; } }; }
static std::atomic<int64_t> clockUs{1000000};
static uint32_t operationClockOffset=0;
static thread_local unsigned pmDepth=0, busDepth=0;
static bool failPmLock=false, invalidClock=false;
static int64_t esp_timer_get_time() { return invalidClock?-1:clockUs.load(); }
static unsigned long millis() { return static_cast<uint32_t>((clockUs+=1000)/1000)+operationClockOffset; }
static void delay(unsigned ms) { clockUs+=static_cast<int64_t>(ms)*1000; }
static void yield() {}
static void vTaskDelay(unsigned ticks) { delay(ticks); }
static std::function<void()> busWaitHook, responseHook, travelHook;
struct Semaphore { bool busy=false; std::recursive_timed_mutex native; };
using TickType_t=unsigned;
static std::atomic<bool> pmWaiting{false};
static thread_local unsigned lastPmWait=0;
using SemaphoreHandle_t=Semaphore*;
static int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) {
    assert(!pmDepth && !busDepth && (timeout==2000 || timeout==0 || timeout==portMAX_DELAY));
    if(busWaitHook) { auto hook=std::move(busWaitHook); busWaitHook={}; hook(); }
    if(lock->busy) return 0;
    ++busDepth; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t) { assert(busDepth==1 && !pmDepth); --busDepth; }
static int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, unsigned timeout) {
    assert(!busDepth);
    lastPmWait=timeout;
    if(failPmLock) return 0;
    bool held=lock->native.try_lock();
    if(!held) {
        pmWaiting=true;
        if(timeout==portMAX_DELAY) {lock->native.lock();held=true;}
        else if(timeout) held=lock->native.try_lock_for(std::chrono::milliseconds(timeout));
    }
    if(held)++pmDepth;
    return held?pdTRUE:0;
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t lock) {
    assert(pmDepth && !busDepth);--pmDepth;lock->native.unlock();
}
// @recursive_guard@
// @types@
static uint16_t crcOf(const uint8_t* bytes,size_t size) {
    uint16_t crc=0xffff;
    for(size_t i=0;i<size;++i) { crc^=bytes[i]; for(unsigned bit=0;bit<8;++bit) crc=crc&1?(crc>>1)^0xa001:crc>>1; }
    return crc;
}
struct HardwareSerial {
    enum Fault { None,Timeout,Crc,Exception,ShortWrite,ShortEcho,WrongEcho,WrongFc } fault=None;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> reply;
    size_t cursor=0;
    bool low=true;
    explicit operator bool() const { return true; }
    unsigned baudRate() const { return 9600; }
    int available() { return static_cast<int>(reply.size()-cursor); }
    int read() { return available()?reply[cursor++]:-1; }
    void flush() {}
    size_t write(const uint8_t* bytes,size_t size) {
        assert(busDepth==1 && !pmDepth && size>=8 && size<=256);
        frames.emplace_back(bytes,bytes+size);
        reply.clear(); cursor=0;
        if(responseHook) { auto hook=std::move(responseHook); responseHook={}; hook(); }
        if(fault==ShortWrite) return size-1;
        if(fault==Timeout) return size;
        if(fault==Exception) reply={bytes[0],static_cast<uint8_t>(bytes[1]|0x80),2};
        else if(bytes[1]==1) reply={bytes[0],1,1,static_cast<uint8_t>(low?0:1)};
        else if(bytes[1]==3) reply={bytes[0],3,2,0,static_cast<uint8_t>(low?0:1)};
        else reply.assign(bytes,bytes+6);
        if (!reply.empty() && !reply[0]) reply[0]=7; // Broadcast maintenance responder.
        if(fault==ShortEcho && reply.size()==6) reply.resize(4);
        if(fault==WrongEcho && reply.size()==6) reply[3]^=1;
        if(fault==WrongFc) reply[1]=4;
        const uint16_t crc=crcOf(reply.data(),reply.size());
        reply.push_back(static_cast<uint8_t>(crc)); reply.push_back(static_cast<uint8_t>(crc>>8));
        if(fault==Crc) reply.back()^=1;
        return size;
    }
};
struct ModbusSubDevice {
    bool enabled=true,ncMode=false;
    char deviceType[16]="relay", sensorId[32]="motor";
    uint16_t motorRegs[5]{10,11,12,13,14};
    uint8_t motorDecimals=0;
    MotorProtocol motorProtocol;
    uint8_t slaveAddress=2,controlProtocol=0,channelCount=4,batchRegType=0;
    uint16_t coilBase=10,batchRegister=0,pwmRegBase=20,pidAddrs[6]{30,31,32,33,34,35};
    uint8_t pwmResolution=16,delayMode=0,baudRateMode=0;
    uint16_t baudRateReg=0,addressReg=0;
};
class ModbusHandler {
public:
    struct SensorIdMatch { uint8_t deviceIndex=0,channel=0; bool matched=false; String action; };
    struct Config {
        int mode=MODBUS_MASTER; uint32_t baudRate=9600;
        struct Master { uint8_t deviceCount=1,maxRetries=3; uint16_t responseTimeout=10; ModbusSubDevice devices[16]; } master;
    } config;
    HardwareSerial serial; HardwareSerial* modbusSerial=&serial;
    Semaphore semaphore; SemaphoreHandle_t _oneShotSemaphore=&semaphore;
    bool isInitialized=true,_controlPending=false;
    volatile bool isOneShotReading=false;
    uint32_t _delayInstance=1,_delayConfiguration=1;
    uint64_t _delayWriteSequence=0,_delayNextOperation=0,_delayLastTimeMs=0,_delayArmedSequence=0;
    uint32_t _delayFailures=0;
    uint64_t _delayFirstFailureOperation=0;
    OneShotError _delayFirstFailure=ONESHOT_SUCCESS;
    CoilDelayTask coilDelayTasks[Protocols::MODBUS_MAX_COIL_DELAY_TASKS];
    OrdinaryCoilDelayTask ordinaryCoilDelayTasks[Protocols::MODBUS_MAX_COIL_DELAY_TASKS];
    FastBee::ModbusControlTarget _ordinaryDelayAckTarget;
    FastBee::OrdinaryControlContext _ordinaryDelayAckContext;
    FastBee::OrdinaryControl::Actor _ordinaryDelayAckActor;
    uint64_t _ordinaryDelayAckOperation=0;
    uint32_t _ordinaryDelayAckWindowMs=0;
    ModbusDelayReceipt _delayLastWrite;
    const char* _lastTravelError=nullptr;
    String _lastTxHex,_lastRxHex;
    const FastBee::ModbusWriteTicket* _ordinaryWriteTicket=nullptr;
    bool _ordinaryWriteAttempted=false;
    bool blockTravel=false;
    struct Travel {
        ModbusHandler* owner;
        const char* before(const uint8_t*,size_t,unsigned long) {
            if(travelHook) { auto hook=std::move(travelHook); travelHook={}; hook(); }
            return owner->blockTravel?"TRAVEL_BLOCKED":nullptr;
        }
        bool protects(uint8_t) const { return false; }
        void after(const uint8_t*,size_t,const uint8_t*,size_t,bool,unsigned long) {}
    } _motorTravel{this};
    void _syncMotorTravel() {}
    bool _shouldDeferSlaveRequest(uint8_t,unsigned long) { return false; }
    void _recordSlaveOutcome(uint8_t,OneShotError,unsigned long) {}
    void setTransmitMode(bool) { assert(busDepth && !pmDepth); }
    void trackCoilDelayWriteLocked(const uint8_t*,size_t);
    void invalidateCoilDelayTasksLocked(const char*);
    bool coilDelayTimeLocked(uint64_t&);
    bool addCoilDelayTask(uint8_t,uint16_t,unsigned long,bool,bool,const ModbusDelayReceipt&,uint64_t* =nullptr);
    void processCoilDelayTasks();
    bool getCoilDelayStatus(JsonObject);
    bool addOrdinaryCoilDelayTask(uint8_t,uint16_t,unsigned long,bool,bool,const ModbusDelayReceipt&,
        const FastBee::OrdinaryControlContext&,uint64_t* =nullptr);
    bool retainOrdinaryHardwareDelay(uint8_t,unsigned long,const ModbusDelayReceipt&,
        const FastBee::OrdinaryControlContext&,uint64_t* =nullptr);
    bool scheduleOrdinaryDelay(uint8_t,uint16_t,unsigned long,bool,bool,const ModbusDelayReceipt&,
        const FastBee::OrdinaryControlContext&,uint64_t*,bool);
    void processOrdinaryCoilDelayTasks();
    void retireOrdinaryDelayWork();
    void appendOrdinaryDelayStatusLocked(JsonArray);
    bool findBySensorIdEx(const String&,SensorIdMatch& match) { match={}; match.matched=true; return true; }
    uint16_t calculateCRC(const uint8_t*,size_t);
    bool validateFrame(const uint8_t*,size_t);
    bool ordinaryFrameTargetLocked(const FastBee::ModbusOutputFrame&,FastBee::ModbusControlTarget&) const;
    bool snapshotOrdinaryFrame(const FastBee::ModbusOutputFrame&,FastBee::ModbusControlTarget&);
    OneShotResult writeOrdinaryFrameOnce(const FastBee::ModbusControlTarget&,const FastBee::ModbusOutputFrame&,
        const FastBee::ModbusWriteTicket&,uint16_t=0,uint8_t* =nullptr,size_t=0,size_t* =nullptr);
    bool ordinaryRelayTargetLocked(uint8_t,FastBee::ModbusControlTarget&) const;
    bool ordinaryMotorTargetLocked(uint8_t,FastBee::ModbusControlTarget&) const;
    bool snapshotOrdinaryMotorReference(const String&,ModbusSubDevice&,FastBee::ModbusControlTarget&);
    bool snapshotOrdinaryRelay(uint8_t,ModbusSubDevice&,FastBee::ModbusControlTarget&);
    bool snapshotControlDevice(uint8_t,ModbusSubDevice&,FastBee::ModbusControlTarget&);
    bool snapshotOrdinaryRelayEndpoint(uint8_t,uint8_t,uint16_t,ModbusSubDevice&,FastBee::ModbusControlTarget&);
    bool snapshotControlSensor(const String&,SensorIdMatch&,ModbusSubDevice&,FastBee::ModbusControlTarget&);
    OneShotResult writeOrdinaryRelayOnce(const FastBee::ModbusControlTarget&,uint8_t,uint16_t,uint16_t,
        const FastBee::ModbusWriteTicket&,uint16_t=0,bool=false);
    OneShotResult _executeModbusTransaction(const uint8_t*,size_t,uint8_t,uint16_t,uint8_t,bool);
    OneShotError _executeModbusTransactionRaw(const uint8_t*,size_t,uint8_t,uint8_t*,size_t,size_t&,uint8_t&,
        uint16_t,uint8_t,bool,ModbusReadDiagnostics* = nullptr);
};
static bool isReadFunctionCode(uint8_t function) { return function>=1 && function<=4; }
class PeripheralManager;
static PeripheralManager* delayPm=nullptr;
class PeripheralManager {
public:
    static PeripheralManager& getInstance() { assert(delayPm);return *delayPm; }
    Semaphore pmSemaphore; SemaphoreHandle_t _mutex=&pmSemaphore;
    uint64_t _nextOrdinaryControlOperation=0,_nextOrdinaryControlGeneration=0,_ordinaryModbusRetiredFloor=0;
    FastBee::ModbusOrdinarySlots _ordinaryModbus;
    struct Port { bool owned() const { return true; } } uartPortLeases[3];
    std::map<String,uint8_t> uartPortById;
    FastBee::UartReceiveGeneration uartReceiveGenerations;
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin&) const;
    bool beginOrdinaryModbusMaintenance(uint64_t,uint32_t,FastBee::OrdinaryControlContext&);
    bool beginOrdinaryControl(FastBee::OrdinaryControl::Source,uint64_t,uint32_t,bool,uint64_t,
        const String&,uint32_t,const FastBee::UartEventOrigin&,FastBee::OrdinaryControlContext&);
    void finishOrdinaryModbusLocked(const FastBee::OrdinaryControlContext&,bool);
    void serviceOrdinaryModbusLocked();
    OneShotResult submitOrdinaryModbusRelay(ModbusHandler&,const FastBee::ModbusControlTarget&,uint8_t,uint16_t,uint16_t,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,uint16_t=0,bool=false,uint64_t=0);
    FastBee::ModbusOrdinarySlot* acquireOrdinaryModbusLocked(const FastBee::ModbusControlTarget&,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,uint64_t,uint64_t=0);
    OneShotResult submitOrdinaryModbusFrame(ModbusHandler&,const FastBee::ModbusControlTarget&,
        const FastBee::ModbusOutputFrame&,const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,
        uint16_t=0,uint8_t* =nullptr,size_t=0,size_t* =nullptr,uint64_t=0);
    bool reserveOrdinaryModbusWork(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,
        uint64_t&,FastBee::OrdinaryControlResult&,bool=false);
    bool activateOrdinaryModbusWork(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,uint64_t, const char** rejectionReason=nullptr);
    bool ordinaryModbusWorkCurrent(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,uint64_t, const char** rejectionReason=nullptr);
    bool finishOrdinaryModbusWork(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,uint64_t,bool);
};
// @backend@
// @issuer@
// @transport@
// @adapter@
// @delay@
static FastBee::OrdinaryControlContext context(PeripheralManager& pm,OC::Source source=OC::Source::Mqtt,
    uint64_t actor=1,uint32_t hold=0,bool review=false,uint64_t generation=0,const String& target="",
    const FastBee::UartEventOrigin& uart={}) {
    FastBee::OrdinaryControlContext out;
    assert(pm.beginOrdinaryControl(source,actor,hold,review,generation,target,FastBee::SensorConfigEpoch::current(),uart,out));
    return out;
}
static void terminal(PeripheralManager& pm,const FastBee::OrdinaryControlContext& source,bool cancel=false) {
    RecursiveMutexGuard lock(pm._mutex); pm.finishOrdinaryModbusLocked(source,cancel);
}
int main(int argc,char**argv) {
    assert(argc==2); const std::string mode=argv[1];
    PeripheralManager pm; delayPm=&pm; ModbusHandler bus;
    ModbusSubDevice device; FastBee::ModbusControlTarget target;
    if (mode.rfind("lazy_", 0) == 0) {
        assert(!pm._ordinaryModbus.begin() && ordinaryStorageAttempts == 0);
        auto source = context(pm);
        FastBee::OrdinaryControlResult result;
        assert(bus.snapshotOrdinaryRelay(0, device, target));
        if (mode == "lazy_empty") {
            for (unsigned i = 0; i < 10; ++i) {
                RecursiveMutexGuard lock(pm._mutex);
                pm.serviceOrdinaryModbusLocked();
                pm.finishOrdinaryModbusLocked(source, true);
            }
            assert(!pm.activateOrdinaryModbusWork(target, source, 1));
            assert(!pm.ordinaryModbusWorkCurrent(target, source, 1));
            assert(!pm.finishOrdinaryModbusWork(target, source, 1, true));
            assert(!pm._ordinaryModbus.begin() && ordinaryStorageAttempts == 0);
            assert(!pm._nextOrdinaryControlGeneration && !pm._ordinaryModbusRetiredFloor);
            assert(bus.serial.frames.empty()); return 0;
        }
        if (mode == "lazy_stable_capacity") {
            std::array<FastBee::OrdinaryControlContext, 16> sources;
            FastBee::ModbusOrdinarySlot* first = nullptr;
            for (unsigned i = 0; i < 16; ++i) {
                sources[i] = context(pm);
                target.slave = static_cast<uint8_t>(i + 1);
                RecursiveMutexGuard lock(pm._mutex);
                auto* slot = pm.acquireOrdinaryModbusLocked(target, sources[i], result, clockUs / 1000);
                assert(slot);
                if (!i) first = slot;
                assert(slot == first + i && pm._ordinaryModbus.begin() == first);
            }
            assert(ordinaryStorageAttempts == 1 && pm._ordinaryModbus.end() == first + 16);
            auto extra = context(pm); target.slave = 17;
            {
                RecursiveMutexGuard lock(pm._mutex);
                assert(!pm.acquireOrdinaryModbusLocked(target, extra, result, clockUs / 1000));
                assert(!std::strcmp(result.reason, "CONTROL_CAPACITY"));
            }
            terminal(pm, sources[0], true);
            const auto watermark = first->lease.retiredThrough;
            auto newer = context(pm);
            {
                RecursiveMutexGuard lock(pm._mutex);
                assert(pm.acquireOrdinaryModbusLocked(target, newer, result, clockUs / 1000) == first);
            }
            assert(first->lease.retiredThrough >= watermark && ordinaryStorageAttempts == 1);
            assert(pm._ordinaryModbus.begin() == first && bus.serial.frames.empty()); return 0;
        }
        failOrdinaryStorage = true;
        const auto operation = pm._nextOrdinaryControlOperation;
        if (mode == "lazy_oom_relay") {
            const auto output = pm.submitOrdinaryModbusRelay(bus, target, target.function, target.first,
                0xff00, source, result);
            assert(output.error == ONESHOT_CONTROL_REJECTED && !output.outputAttempted);
        } else if (mode == "lazy_oom_frame") {
            FastBee::ModbusOutputFrame frame;
            assert(frame.single(2, 5, 10, 0xff00) && bus.snapshotOrdinaryFrame(frame, target));
            const auto output = pm.submitOrdinaryModbusFrame(bus, target, frame, source, result);
            assert(output.error == ONESHOT_CONTROL_REJECTED && !output.outputAttempted);
        } else {
            assert(mode == "lazy_oom_worker" || mode == "lazy_oom_stop");
            auto& dev = bus.config.master.devices[0];
            std::strcpy(dev.deviceType, "motor"); dev.motorProtocol.writeFunction = 6;
            assert(bus.snapshotControlDevice(0, device, target));
            uint64_t work = 99;
            assert(!pm.reserveOrdinaryModbusWork(target, source, work, result, mode == "lazy_oom_stop"));
            assert(!work);
        }
        assert(!std::strcmp(result.reason, "CONTROL_MEMORY_UNAVAILABLE") && !result.accepted);
        assert(!pm._ordinaryModbus.begin() && ordinaryStorageAttempts == 1);
        assert(pm._nextOrdinaryControlOperation == operation && !pm._nextOrdinaryControlGeneration);
        assert(!pm._ordinaryModbusRetiredFloor && bus.serial.frames.empty() && !bus._delayWriteSequence);
        failOrdinaryStorage = false;
        auto recovery = context(pm);
        {
            RecursiveMutexGuard lock(pm._mutex);
            assert(pm.acquireOrdinaryModbusLocked(target, recovery, result, clockUs / 1000));
        }
        assert(pm._ordinaryModbus.begin() && ordinaryStorageAttempts == 2 && bus.serial.frames.empty());
        return 0;
    }
    if(mode.rfind("mapping_",0)==0) {
        const std::string scenario=mode.substr(8);
        auto& dev=bus.config.master.devices[0];
        auto& alias=bus.config.master.devices[1];
        bus.config.master.deviceCount=2;
        alias=dev;
        auto motorSnapshot=[&](uint8_t index,FastBee::ModbusControlTarget& out) {
            return bus.snapshotOrdinaryMotorReference(FastBee::motorControlReference(
                bus.config.master.devices[index]),device,out);
        };
        FastBee::ModbusOutputFrame frame;
        FastBee::OrdinaryControlResult result;
        if(scenario=="mixed_nonoverlap" || scenario=="mixed_source_conflict" || scenario=="spaces_distinct") {
            std::strcpy(alias.deviceType,"motor");
            if(scenario!="spaces_distinct") {
                dev.controlProtocol=1;
                for(auto& reg:alias.motorRegs)reg+=100;
            }
            FastBee::ModbusControlTarget motor;
            assert(bus.snapshotOrdinaryRelay(0,device,target));
            assert(motorSnapshot(1,motor) && motor.kind==FastBee::ModbusControlTarget::Kind::Motor);
            assert(frame.single(2,target.function,10,target.function==5?0xff00:1));
            FastBee::ModbusControlTarget generic;
            assert(bus.snapshotOrdinaryFrame(frame,generic));
            auto source=context(pm);
            assert(pm.submitOrdinaryModbusRelay(bus,target,target.function,10,target.function==5?0xff00:1,
                source,result,10).error==ONESHOT_SUCCESS);
            if(scenario=="mixed_source_conflict") {
                auto other=context(pm,OC::Source::Http,2,100);
                uint64_t work=0;
                assert(!pm.reserveOrdinaryModbusWork(motor,other,work,result));
                assert(!work && result.decision==OC::Decision::Conflict && !result.attempted);
            }
            assert(bus.serial.frames.size()==1);return 0;
        }
        if(scenario=="motor_shared_command" || scenario=="motor_disjoint") {
            std::strcpy(dev.deviceType,"motor");
            alias.enabled=false;
            if(scenario=="motor_shared_command") {
                dev.motorRegs[0]=dev.motorRegs[1]=dev.motorRegs[2]=10;
                dev.motorProtocol.configured=true;
                dev.motorProtocol.forwardValue=1;dev.motorProtocol.reverseValue=2;dev.motorProtocol.stopValue=3;
            } else {
                alias=dev;for(auto& reg:alias.motorRegs)reg+=100;
            }
            assert(motorSnapshot(0,target));
            if(scenario=="motor_disjoint") {FastBee::ModbusControlTarget second;assert(motorSnapshot(1,second));}
            assert(frame.single(2,6,dev.motorRegs[2],dev.motorProtocol.stopValue));
            FastBee::ModbusControlTarget generic;assert(bus.snapshotOrdinaryFrame(frame,generic));
            auto source=context(pm);
            const auto withoutWorker=pm.submitOrdinaryModbusRelay(bus,target,6,dev.motorRegs[2],
                dev.motorProtocol.stopValue,source,result,10);
            assert(withoutWorker.error==ONESHOT_CONTROL_REJECTED && !withoutWorker.outputAttempted &&
                String(withoutWorker.controlReason)=="CONTROL_PARAMETER_INVALID" && bus.serial.frames.empty());
            uint64_t work=0;
            assert(pm.reserveOrdinaryModbusWork(target,source,work,result));
            assert(pm.activateOrdinaryModbusWork(target,source,work));
            assert(pm.submitOrdinaryModbusRelay(bus,target,6,dev.motorRegs[2],dev.motorProtocol.stopValue,
                source,result,10,false,work).error==ONESHOT_SUCCESS);
            assert(pm.finishOrdinaryModbusWork(target,source,work,false));
            assert(bus.serial.frames.size()==1);return 0;
        }
        if(scenario=="relay_wait_alias" || scenario=="motor_wait_alias" || scenario=="frame_wait_alias") {
            dev.controlProtocol=1;
            if(scenario=="motor_wait_alias")std::strcpy(dev.deviceType,"motor");
            alias=dev;alias.enabled=false;
            assert(frame.single(2,6,10,1));
            if(scenario=="motor_wait_alias")assert(motorSnapshot(0,target));
            else if(scenario=="frame_wait_alias")assert(bus.snapshotOrdinaryFrame(frame,target));
            else assert(bus.snapshotOrdinaryRelay(0,device,target));
            auto source=context(pm);
            uint64_t work=0;
            if(scenario=="motor_wait_alias") {
                assert(pm.reserveOrdinaryModbusWork(target,source,work,result));
                assert(pm.activateOrdinaryModbusWork(target,source,work));
            }
            busWaitHook=[&]{alias.enabled=true;}; // No epoch hint: mapping itself must be rechecked.
            const auto rejected=scenario=="frame_wait_alias"?
                pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10):
                pm.submitOrdinaryModbusRelay(bus,target,6,10,1,source,result,10,false,work);
            assert(rejected.error!=ONESHOT_SUCCESS && !rejected.outputAttempted && bus.serial.frames.empty());
            assert(alias.enabled && !busWaitHook);
            if(work)assert(pm.finishOrdinaryModbusWork(target,source,work,true));
            return 0;
        }
        if(scenario=="disabled_alias" || scenario=="other_slave_alias" || scenario=="unrelated_invalid_profile") {
            dev.controlProtocol=1;alias=dev;
            if(scenario=="disabled_alias")alias.enabled=false;
            else if(scenario=="other_slave_alias")alias.slaveAddress=3;
            else {std::strcpy(alias.deviceType,"motor");alias.motorProtocol.valid=false;
                for(auto& reg:alias.motorRegs)reg+=100;}
            assert(bus.snapshotOrdinaryRelay(0,device,target));
            assert(frame.single(2,6,10,1));assert(bus.snapshotOrdinaryFrame(frame,target));
            auto source=context(pm);
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10).error==ONESHOT_SUCCESS);
            assert(bus.serial.frames.size()==1);return 0;
        }
        // Every remaining case must fail during mapping, before any UART read/write.
        if(scenario=="value_alias_high" || scenario=="value_alias_low") {
            dev.controlProtocol=1;dev.delayMode=1;
            std::strcpy(alias.deviceType,"pwm");alias.pwmRegBase=10;
            assert(frame.single(2,6,10,scenario=="value_alias_high"?255:1));
            assert(!bus.snapshotOrdinaryRelay(0,device,target));
        } else if(scenario=="pwm_resolution_alias") {
            std::strcpy(dev.deviceType,"pwm");dev.pwmRegBase=10;dev.pwmResolution=8;
            alias=dev;alias.pwmResolution=16;
            assert(frame.single(2,6,10,300));
        } else if(scenario=="motor_fc_alias" || scenario=="invalid_motor_alias") {
            std::strcpy(dev.deviceType,"motor");dev.motorProtocol.writeFunction=16;
            if(scenario=="invalid_motor_alias")dev.motorProtocol.valid=false;
            std::strcpy(alias.deviceType,"pwm");alias.pwmRegBase=10;
            assert(frame.single(2,6,10,1));assert(!motorSnapshot(0,target));
        } else if(scenario=="pid_pv_alias" || scenario=="pid_internal_pv") {
            std::strcpy(dev.deviceType,"pid");
            if(scenario=="pid_internal_pv") {alias.enabled=false;dev.pidAddrs[1]=dev.pidAddrs[0];}
            else {std::strcpy(alias.deviceType,"pwm");alias.pwmRegBase=dev.pidAddrs[0];}
            assert(frame.single(2,6,dev.pidAddrs[0],10));
        } else if(scenario=="relay_internal_batch") {
            alias.enabled=false;dev.controlProtocol=1;dev.batchRegister=10;
            assert(frame.single(2,6,10,1));assert(!bus.snapshotOrdinaryRelay(0,device,target));
        } else if(scenario=="relay_internal_timer") {
            alias.enabled=false;dev.coilBase=0x0200;
            assert(frame.single(2,5,0x0200,0xff00));assert(!bus.snapshotOrdinaryRelay(0,device,target));
        } else if(scenario=="timer_other_alias") {
            alias.coilBase=0x0200;alias.delayMode=1;
            assert(frame.single(2,5,0x0200,0xff00));assert(!bus.snapshotOrdinaryRelay(1,device,target));
        } else if(scenario=="batch_toggle_alias") {
            dev.batchRegister=20;std::strcpy(alias.deviceType,"pwm");alias.pwmRegBase=20;
            assert(frame.single(2,6,20,15));frame.transform=FastBee::ModbusFrameTransform::RelayBatchToggle;
            assert(!bus.snapshotOrdinaryRelay(0,device,target));
        } else if(scenario=="vendor_relay_alias") {
            alias.coilBase=9;alias.channelCount=2;
            assert(frame.single(2,5,10,0x5a00,FastBee::ModbusFrameAccess::RelayVendor));
        } else if(scenario=="same_type_relay") {
            assert(frame.single(2,5,10,0xff00));assert(!bus.snapshotOrdinaryRelay(0,device,target));
        } else if(scenario=="same_type_motor") {
            std::strcpy(dev.deviceType,"motor");alias=dev;alias.motorRegs[0]=9;
            assert(frame.single(2,6,11,1));assert(!motorSnapshot(0,target));
        } else if(scenario=="fc16_suffix_alias") {
            std::strcpy(dev.deviceType,"pwm");dev.pwmRegBase=10;dev.channelCount=2;
            alias.controlProtocol=1;alias.delayMode=1;alias.coilBase=11;alias.channelCount=1;
            const uint8_t bytes[]={2,16,0,10,0,2,4,0,1,0,255};
            assert(frame.assign(bytes,sizeof(bytes)));
        } else {assert(false && "unknown mapping scenario");}
        assert(!bus.snapshotOrdinaryFrame(frame,target) && bus.serial.frames.empty());return 0;
    }
    if(mode.rfind("delay_",0)==0) {
        const std::string scenario=mode.substr(6);
        const bool hardware=scenario.rfind("hardware",0)==0;
        auto& dev=bus.config.master.devices[0];
        dev.delayMode=hardware?2:1;dev.controlProtocol=hardware || scenario=="register";
        const uint8_t fc=dev.controlProtocol?6:5;
        auto source=scenario=="software_http"?context(pm,OC::Source::Http,1,10000):context(pm);
        if(scenario=="rule_retire" || scenario=="context_binding") {
            source.ruleBound=true;source.ruleEpoch=FastBee::RuleOutputIdentity::currentEpoch();
        }
        if(scenario=="deadline_retire")source.operationDeadlineAt=3000;
        if(scenario=="uart_retire") {
            pm.uartPortById["uart"]=1;assert(pm.uartReceiveGenerations.begin(1)); FastBee::UartReceiveFault::arm(1, pm.uartReceiveGenerations.current(1));
            const auto origin=FastBee::UartEventOrigin::captured(
                FastBee::SensorConfigEpoch::current(),pm.uartReceiveGenerations.current(1));
            source=context(pm,OC::Source::Rule,1,0,false,0,"",origin);
        }
        FastBee::ModbusOutputFrame frame;
        assert(frame.single(2,fc,10,hardware?101:fc==5?0xff00:1));
        if(scenario=="hardware_fc16") {
            const uint8_t bytes[]={2,16,0,10,0,2,4,0,101,0,201};
            assert(frame.assign(bytes,sizeof(bytes)));
        }
        if(scenario=="hardware_overflow") {
            assert(frame.single(2,6,10,3000));
            assert(!bus.snapshotOrdinaryFrame(frame,target));return 0;
        }
        assert(bus.snapshotOrdinaryFrame(frame,target));
        assert(target.hardwareWindowMs==(scenario=="hardware_fc16"?2000:hardware?1000:0));
        if(scenario=="hardware_window_mismatch") {
            frame.hardwareWindowMs=900;
            FastBee::OrdinaryControlResult result;
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,source,result).error!=ONESHOT_SUCCESS);
            assert(bus.serial.frames.empty());return 0;
        }
        if(scenario=="hardware_timeout")bus.serial.fault=HardwareSerial::Timeout;
        if(scenario=="hardware_unattempted")bus.semaphore.busy=true;
        if(scenario=="hardware_clock_unknown")responseHook=[]{invalidClock=true;};
        FastBee::OrdinaryControlResult result;
        const auto initial=pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10);
        if(scenario=="hardware_unattempted") {
            assert(!initial.outputAttempted && !pm._ordinaryModbus[0].hardwareUntilMs);
            bus.semaphore.busy=false;terminal(pm,source);
            auto newer=context(pm,OC::Source::Mqtt,2);
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,newer,result,10).error==ONESHOT_SUCCESS);return 0;
        }
        if(hardware) {
            assert(initial.outputAttempted && pm._ordinaryModbus[0].hardwareUntilMs);
            if(scenario=="hardware_timeout")assert(initial.error!=ONESHOT_SUCCESS);
            else assert(initial.error==ONESHOT_SUCCESS);
            if(scenario=="hardware_clock_unknown") {
                assert(pm._ordinaryModbus[0].hardwareUntilMs==UINT64_MAX);
                invalidClock=false;
            }
            uint64_t hardwareId=0,finalId=0;
            if(scenario=="hardware" || scenario=="hardware_nc" || scenario=="hardware_admission_failure") {
                assert(bus.retainOrdinaryHardwareDelay(2,1000,initial.delayReceipt,source,&hardwareId));
                assert(hardwareId && bus.ordinaryCoilDelayTasks[0].hardwareOnly);
                if(scenario!="hardware") {
                    if(scenario=="hardware_admission_failure")failPmLock=true;
                    const bool scheduled=bus.addOrdinaryCoilDelayTask(2,10,1500,true,true,initial.delayReceipt,source,&finalId);
                    failPmLock=false;
                    assert(scheduled==(scenario=="hardware_nc"));
                    if(scheduled)assert(finalId==hardwareId && !bus.ordinaryCoilDelayTasks[0].hardwareOnly);
                    else assert(bus.ordinaryCoilDelayTasks[0].hardwareOnly && !finalId);
                }
            }
            terminal(pm,source);
            if(scenario=="hardware_source_retire")FastBee::SensorConfigEpoch::advance();
            if(scenario=="hardware_handler_replace") {
                bus.isInitialized=false;
                bus.invalidateCoilDelayTasksLocked("HANDLER_STOPPED");
            }
            ModbusHandler replacement;replacement._delayInstance=2;
            replacement.config.master.devices[0]=dev;
            auto newer=context(pm,OC::Source::Mqtt,2);
            FastBee::ModbusControlTarget second;
            FastBee::ModbusOutputFrame ordinary;assert(ordinary.single(2,6,10,0));
            if(scenario=="hardware_other_slave") {
                replacement.config.master.devices[0].slaveAddress=3;
                assert(ordinary.single(3,6,10,0));
            }
            if(scenario=="hardware_maintenance") {
                assert(pm.beginOrdinaryModbusMaintenance(2,FastBee::SensorConfigEpoch::current(),newer));
                const uint8_t bytes[]={0,16,0,0,0,1,2,0,4};
                assert(ordinary.assign(bytes,sizeof(bytes),FastBee::ModbusFrameAccess::Maintenance));
            }
            assert(replacement.snapshotOrdinaryFrame(ordinary,second));
            const auto competing=pm.submitOrdinaryModbusFrame(replacement,second,ordinary,newer,result,10);
            if(scenario=="hardware_other_slave") {
                assert(competing.error==ONESHOT_SUCCESS && replacement.serial.frames.size()==1);return 0;
            }
            assert(!competing.outputAttempted && replacement.serial.frames.empty());
            if(scenario=="hardware_window_expiry" || scenario=="hardware_clock_unknown") {
                clockUs+=3000000;
                auto after=context(pm,OC::Source::Mqtt,3);
                const auto expired=pm.submitOrdinaryModbusFrame(replacement,second,ordinary,after,result,10);
                assert((expired.error==ONESHOT_SUCCESS)==(scenario=="hardware_window_expiry"));
                assert(pm._ordinaryModbus[0].failures==0);return 0;
            }
            if(hardwareId) {
                clockUs+=2000000;bus.processOrdinaryCoilDelayTasks();
                auto& task=bus.ordinaryCoilDelayTasks[0];
                assert(!task.workGeneration);
                if(scenario=="hardware_nc") {
                    assert(task.phase==CoilDelayTask::Phase::Acknowledged && task.attempted &&
                        bus.serial.frames.size()==2 && bus.serial.frames.back()[5]==1);
                } else {
                    assert(task.hardwareOnly && !task.attempted && task.phase==CoilDelayTask::Phase::Retired &&
                        bus.serial.frames.size()==1);
                }
                JsonDocument status;assert(bus.getCoilDelayStatus(status.to<JsonObject>()));
                assert(status["records"][0]["physicalOutputVerified"]==false);
                assert(status["records"][0]["finalCommandAcknowledged"]==(scenario=="hardware_nc"));
            }
            if(scenario=="hardware_timeout") {
                const auto failures=pm._ordinaryModbus[0].failures;
                clockUs+=3000000;bus.serial.fault=HardwareSerial::None;
                auto after=context(pm,OC::Source::Mqtt,3);
                assert(pm.submitOrdinaryModbusFrame(bus,target,frame,after,result,10).error==ONESHOT_SUCCESS);
                assert(failures==1 && pm._ordinaryModbus[0].failures==failures);
            }
            return 0;
        }
        assert(initial.error==ONESHOT_SUCCESS && initial.delayReceipt.writeSequence);
        auto receipt=initial.delayReceipt;
        if(scenario=="receipt_mismatch") {
            ++receipt.writeSequence;
            assert(!bus.addOrdinaryCoilDelayTask(2,10,1000,false,fc==6,receipt,source));
            assert(!pm._ordinaryModbus[0].workerReserved);return 0;
        }
        auto supplied=source;
        if(scenario=="context_binding"){supplied.ruleBound=false;supplied.ruleEpoch=0;supplied.operationDeadlineAt=0;}
        uint64_t operation=0;
        assert(bus.addOrdinaryCoilDelayTask(2,10,1000,false,fc==6,receipt,supplied,&operation));
        auto& task=bus.ordinaryCoilDelayTasks[0];
        assert(operation && task.workGeneration && pm._ordinaryModbus[0].workerReserved);
        terminal(pm,source,scenario=="cancel");
        assert(pm._ordinaryModbus[0].workerReserved);
        if(scenario=="config_retire")FastBee::SensorConfigEpoch::advance();
        if(scenario=="rule_retire" || scenario=="context_binding")FastBee::RuleOutputIdentity::invalidate();
        if(scenario=="uart_retire") {pm.uartReceiveGenerations.end(1);assert(pm.uartReceiveGenerations.begin(1)); FastBee::UartReceiveFault::arm(1, pm.uartReceiveGenerations.current(1));}
        if(scenario=="handler_retire") {
            assert(xSemaphoreTake(bus._oneShotSemaphore,0));
            bus.invalidateCoilDelayTasksLocked("HANDLER_STOPPED");bus.isInitialized=false;
            xSemaphoreGive(bus._oneShotSemaphore);bus.retireOrdinaryDelayWork();
        }
        if(scenario=="newer_write") {
            const uint8_t newer[]={2,5,0,10,0,0,0,0};
            assert(xSemaphoreTake(bus._oneShotSemaphore,0));
            bus.trackCoilDelayWriteLocked(newer,sizeof(newer));xSemaphoreGive(bus._oneShotSemaphore);
        }
        if(scenario=="deadline_retire")clockUs+=3000000;
        if(scenario=="timeout")bus.serial.fault=HardwareSerial::Timeout;
        if(scenario=="bus_busy"){bus.semaphore.busy=true;clockUs+=2000000;bus.processOrdinaryCoilDelayTasks();
            assert(task.workGeneration && bus.serial.frames.size()==1);bus.semaphore.busy=false;}
        if(scenario=="cleanup_pending") {
            // Lose PM access at source check/finalization. The record must
            // retain cleanup work, emit no suffix, and release only after recovery.
            failPmLock=true;clockUs+=2000000;bus.processOrdinaryCoilDelayTasks();
            assert(task.workGeneration && task.phase==CoilDelayTask::Phase::Retired &&
                !task.attempted && bus.serial.frames.size()==1);
            failPmLock=false;bus.processOrdinaryCoilDelayTasks();
            assert(!task.workGeneration && bus.serial.frames.size()==1);return 0;
        }
        clockUs+=2000000;bus.processOrdinaryCoilDelayTasks();
        const bool retired=scenario=="config_retire" || scenario=="handler_retire" || scenario=="rule_retire" ||
            scenario=="context_binding" || scenario=="uart_retire" || scenario=="deadline_retire" ||
            scenario=="newer_write" || scenario=="cancel";
        assert(!task.workGeneration && !pm._ordinaryModbus[0].workerReserved);
        assert(bus.serial.frames.size()==(retired?1:2));
        assert(task.attempted==!retired);
        assert(task.phase==(retired?CoilDelayTask::Phase::Retired:scenario=="timeout"?
            CoilDelayTask::Phase::ResultUnknown:CoilDelayTask::Phase::Acknowledged));
        const size_t count=bus.serial.frames.size();
        bus.processOrdinaryCoilDelayTasks();assert(bus.serial.frames.size()==count);
        assert(!bus.addOrdinaryCoilDelayTask(2,10,1000,false,fc==6,receipt,source));
        JsonDocument status;assert(bus.getCoilDelayStatus(status.to<JsonObject>()));
        assert(status["physicalOutputVerified"]==false);
        assert(status["records"][0]["finalCommandAcknowledged"]==(!retired && scenario!="timeout"));
        if(scenario=="timeout")assert(status["failureCount"]==1 && status["firstFailureOperation"]=="1");
        return 0;
    }
    if(mode.rfind("frame_",0)==0) {
        const std::string scenario=mode.substr(6);
        using Access=FastBee::ModbusFrameAccess;
        FastBee::ModbusOutputFrame frame;
        auto& dev=bus.config.master.devices[0];
        assert(frame.single(2,5,10,0xff00));
        if(scenario=="fc06" || scenario=="fc16" || scenario=="fc16_max" || scenario=="pwm" ||
            scenario=="snapshot_pwm" || scenario=="worker_multi_frame") {
            std::strcpy(dev.deviceType,"pwm");dev.pwmRegBase=10;dev.channelCount=123;
            assert(frame.single(2,6,10,31));
        }
        if(scenario=="pid") {
            std::strcpy(dev.deviceType,"pid");assert(frame.single(2,6,31,600));
        }
        if(scenario=="fc15" || scenario=="unused_coil_bits") {
            const uint8_t bytes[]={2,15,0,10,0,4,1,static_cast<uint8_t>(scenario=="fc15"?5:0xf5)};
            const bool valid=frame.assign(bytes,sizeof(bytes));
            if(scenario=="unused_coil_bits"){assert(!valid && !frame.length);return 0;}
            assert(valid);
        }
        if(scenario=="fc16" || scenario=="fc16_max" || scenario=="bad_bytecount" || scenario=="range_overflow") {
            const uint16_t count=scenario=="fc16_max"?123:2;
            uint8_t bytes[253]={2,16,0,10,static_cast<uint8_t>(count>>8),static_cast<uint8_t>(count),
                static_cast<uint8_t>(count*2)};
            for(unsigned i=0;i<count;++i){bytes[7+2*i]=0;bytes[8+2*i]=static_cast<uint8_t>(i);}
            if(scenario=="bad_bytecount")--bytes[6];
            if(scenario=="range_overflow"){bytes[2]=255;bytes[3]=255;}
            const bool valid=frame.assign(bytes,7+count*2);
            if(scenario=="bad_bytecount" || scenario=="range_overflow"){assert(!valid && !frame.length);return 0;}
            assert(valid);
        }
        if(scenario=="unmapped")assert(frame.single(2,6,600,1));
        if(scenario=="ambiguous"){bus.config.master.deviceCount=2;bus.config.master.devices[1]=dev;}
        if(scenario=="vendor_toggle" || scenario=="vendor_delay255" || scenario=="vendor_mixed" || scenario=="vendor_unknown") {
            const uint16_t address=scenario=="vendor_delay255"?0x0200:10;
            const uint16_t value=scenario=="vendor_delay255"?0xff00:scenario=="vendor_unknown"?0x1234:0x5a00;
            const bool valid=frame.single(2,5,address,value,Access::RelayVendor);
            if(scenario=="vendor_unknown"){assert(!valid);return 0;}
            assert(valid);
            if(scenario=="vendor_mixed"){bus.config.master.deviceCount=2;bus.config.master.devices[1]=dev;
                std::strcpy(bus.config.master.devices[1].deviceType,"motor");}
        }
        if(scenario=="toggle" || scenario=="toggle_retired" || scenario=="toggle_timeout") {
            dev.batchRegister=20;assert(frame.single(2,6,20,15));
            frame.transform=FastBee::ModbusFrameTransform::RelayBatchToggle;
        }
        FastBee::ModbusControlTarget ordinaryTarget;
        FastBee::ModbusOutputFrame ordinaryFrame;assert(ordinaryFrame.single(2,5,10,0xff00));
        const bool maintenance=scenario.rfind("maintenance",0)==0 || scenario=="ordinary_maintenance_busy";
        if(maintenance) {
            assert(bus.snapshotOrdinaryFrame(ordinaryFrame,ordinaryTarget));
            const uint8_t changeAddress[]={0,16,0,0,0,1,2,0,3};
            assert(frame.assign(changeAddress,sizeof(changeAddress),Access::Maintenance));
            if(scenario=="maintenance_baud") {
                dev.baudRateMode=1;dev.baudRateReg=51;
                assert(frame.single(2,6,51,7,Access::Maintenance));
            }
        }
        const bool mapped=bus.snapshotOrdinaryFrame(frame,target);
        if(scenario=="unmapped" || scenario=="ambiguous" || scenario=="vendor_mixed") {
            assert(!mapped && bus.serial.frames.empty());return 0;
        }
        assert(mapped && target.valid());
        if(scenario=="snapshot_pwm") {
            FastBee::ModbusControlTarget copied;ModbusSubDevice saved;
            assert(bus.snapshotControlDevice(0,saved,copied));
            assert(copied.kind==FastBee::ModbusControlTarget::Kind::Snapshot &&
                copied.instance==target.instance && copied.configuration==target.configuration &&
                copied.slave==target.slave && !copied.accepts(6,10,1));return 0;
        }
        FastBee::OrdinaryControlContext source;
        if(maintenance && scenario!="maintenance_auth")
            assert(pm.beginOrdinaryModbusMaintenance(1,FastBee::SensorConfigEpoch::current(),source));
        else source=context(pm);
        if(scenario=="maintenance_cannot_write_ordinary") {
            FastBee::OrdinaryControlResult blocked;
            assert(pm.submitOrdinaryModbusFrame(bus,ordinaryTarget,ordinaryFrame,source,blocked,10).error!=ONESHOT_SUCCESS);
            assert(bus.serial.frames.empty());return 0;
        }
        if(scenario=="maintenance_ordinary_http_zero") {
            FastBee::OrdinaryControlContext invalid;
            assert(!pm.beginOrdinaryControl(OC::Source::Http,1,0,false,0,"",
                FastBee::SensorConfigEpoch::current(),{},invalid));
            assert(bus.serial.frames.empty());return 0;
        }
        FastBee::OrdinaryControlResult result;uint64_t work=0;
        if(scenario=="maintenance_ordinary_busy" || scenario=="maintenance_worker_busy") {
            auto owner=context(pm,OC::Source::Mqtt,9);
            if(scenario=="maintenance_worker_busy") {
                assert(pm.reserveOrdinaryModbusWork(ordinaryTarget,owner,work,result));
                terminal(pm,owner);
            } else assert(pm.submitOrdinaryModbusFrame(bus,ordinaryTarget,ordinaryFrame,owner,result,10).error==ONESHOT_SUCCESS);
        }
        if(scenario=="worker_multi_frame") {
            assert(pm.reserveOrdinaryModbusWork(target,source,work,result));
            terminal(pm,source);
        }
        if(scenario=="source_wait")busWaitHook=[]{FastBee::SensorConfigEpoch::advance();};
        if(scenario=="handler_wait")busWaitHook=[&]{++bus._delayConfiguration;};
        if(scenario=="deadline_wait"){source.operationDeadlineAt=1100;busWaitHook=[]{clockUs+=200000;};}
        if(scenario=="mutated_frame")++frame.bytes[3];
        if(scenario=="toggle_retired")responseHook=[]{FastBee::SensorConfigEpoch::advance();};
        if(scenario=="toggle_timeout")bus.serial.fault=HardwareSerial::Timeout;
        if(scenario=="raw_exception")bus.serial.fault=HardwareSerial::Exception;
        if(scenario=="wrong_echo")bus.serial.fault=HardwareSerial::WrongEcho;
        if(scenario=="short_write")bus.serial.fault=HardwareSerial::ShortWrite;
        uint8_t raw[256]{};size_t length=999;
        const size_t before=bus.serial.frames.size();
        const auto first=pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10,raw,
            scenario=="bad_raw_capacity"?255:sizeof(raw),&length,work);
        if(scenario=="maintenance_auth" || scenario=="maintenance_ordinary_busy" || scenario=="maintenance_worker_busy" ||
            scenario=="source_wait" || scenario=="handler_wait" || scenario=="deadline_wait" ||
            scenario=="mutated_frame" || scenario=="bad_raw_capacity") {
            assert(first.error!=ONESHOT_SUCCESS && !first.outputAttempted && length==0 &&
                bus.serial.frames.size()==before);return 0;
        }
        if(scenario=="toggle_retired" || scenario=="toggle_timeout") {
            assert(first.error!=ONESHOT_SUCCESS && !first.outputAttempted && bus.serial.frames.size()==1);return 0;
        }
        if(scenario=="raw_exception") {
            assert(first.error==ONESHOT_EXCEPTION && first.outputAttempted && length==5 && raw[1]==0x85 && raw[2]==2);
            assert(crcOf(raw,length-2)==static_cast<uint16_t>((raw[length-1]<<8)|raw[length-2]));
            assert(bus.serial.frames.size()==1);return 0;
        }
        if(scenario=="wrong_echo" || scenario=="short_write") {
            assert(first.error!=ONESHOT_SUCCESS && first.outputAttempted && bus.serial.frames.size()==1 && !length);return 0;
        }
        assert(first.error==ONESHOT_SUCCESS && first.outputAttempted && result.accepted && length==8);
        if(scenario=="maintenance_terminal") {
            terminal(pm,source);
            auto writer=context(pm,OC::Source::Mqtt,2);
            assert(pm.submitOrdinaryModbusFrame(bus,ordinaryTarget,ordinaryFrame,writer,result,10).error==ONESHOT_SUCCESS);
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10).error!=ONESHOT_SUCCESS);
            assert(bus.serial.frames.size()==2);return 0;
        }
        if(scenario=="ordinary_maintenance_busy") {
            auto writer=context(pm,OC::Source::Mqtt,2);
            assert(pm.submitOrdinaryModbusFrame(bus,ordinaryTarget,ordinaryFrame,writer,result,10).error!=ONESHOT_SUCCESS);
            assert(bus.serial.frames.size()==1);return 0;
        }
        if(scenario=="prefix_failure") {
            bus.serial.fault=HardwareSerial::Timeout;
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10).outputAttempted);
            bus.serial.fault=HardwareSerial::None;
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10).error!=ONESHOT_SUCCESS);
            assert(bus.serial.frames.size()==2 && pm._ordinaryModbus[0].failures==1);return 0;
        }
        if(scenario=="worker_multi_frame") {
            const auto original=target;
            assert(frame.single(2,6,11,42) && bus.snapshotOrdinaryFrame(frame,target));
            assert(pm.submitOrdinaryModbusFrame(bus,target,frame,source,result,10,nullptr,0,nullptr,work).error==ONESHOT_SUCCESS);
            assert(pm.finishOrdinaryModbusWork(original,source,work,false));
            assert(pm._ordinaryModbus[0].lease.phase==OC::Phase::Idle && bus.serial.frames.size()==2);return 0;
        }
        if(scenario=="toggle") {
            assert(bus.serial.frames.size()==2 && bus.serial.frames[0][1]==3 &&
                bus.serial.frames[1][1]==6 && bus.serial.frames[1][5]==15);
        } else assert(bus.serial.frames.size()==1);
        if(frame.bytes[1]==5 || frame.bytes[1]==6)assert(first.delayReceipt.writeSequence!=0);
        terminal(pm,source);return 0;
    }
    const bool motorMode=mode.rfind("worker_",0)==0 || mode.rfind("stop_",0)==0 || mode.rfind("motor_fc16",0)==0;
    if(motorMode) {
        auto& dev=bus.config.master.devices[0]; std::strcpy(dev.deviceType,"motor");
        dev.motorProtocol.writeFunction=mode.rfind("motor_fc16",0)==0?16:6;
        assert(bus.snapshotControlDevice(0,device,target) && target.valid());
        auto source=context(pm); FastBee::OrdinaryControlResult result; uint64_t work=0;
        if(mode=="worker_screen_replaced") { FastBee::ScreenOutputIdentity::publish(1);
            source=context(pm,OC::Source::Screen,(uint64_t(1)<<32)|3,30000); }
        assert(pm.reserveOrdinaryModbusWork(target,source,work,result,mode=="stop_parameter_denied"));
        if(mode.rfind("worker_lock_",0)==0) {
            // Recreate accepted HTTP caller completion before the worker starts.
            terminal(pm,source,false);
            std::promise<void> ready;
            FastBee::OrdinaryControlContext stop;
            uint64_t stopWork=0;
            const bool sustained=mode=="worker_lock_sustained";
            if(mode=="worker_lock_current")assert(pm.activateOrdinaryModbusWork(target,source,work));
            if(mode=="worker_lock_deadline")source.operationDeadlineAt=static_cast<uint32_t>(millis())+30;
            if(mode=="worker_lock_wrap") {
                operationClockOffset=UINT32_MAX-static_cast<uint32_t>(clockUs/1000)-15;
                source.operationDeadlineAt=static_cast<uint32_t>(millis())+50;
            }
            pmWaiting=false;
            std::thread holder([&] {
                RecursiveMutexGuard lock(pm._mutex);
                ready.set_value();
                while(!pmWaiting.load())std::this_thread::sleep_for(std::chrono::microseconds(100));
                std::this_thread::sleep_for(std::chrono::milliseconds(sustained?140:20));
                if(mode=="worker_lock_deadline")clockUs+=40000;
                if(mode=="worker_lock_cancel")terminal(pm,source,true);
                if(mode=="worker_lock_stop") {
                    stop=context(pm,OC::Source::Http,2,30000);
                    FastBee::OrdinaryControlResult stopResult;
                    assert(pm.reserveOrdinaryModbusWork(target,stop,stopWork,stopResult,true));
                    terminal(pm,stop,false);
                }
            });
            ready.get_future().wait();
            const auto began=std::chrono::steady_clock::now();
#ifdef BASELINE_LOCK_REPRO
            assert(!pm.activateOrdinaryModbusWork(target,source,work));
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-began).count();
            assert(elapsed<15 && lastPmWait==0);
            holder.join();
            assert(pm.activateOrdinaryModbusWork(target,source,work));
            assert(pm._ordinaryModbus[0].workerReserved && pm._ordinaryModbus[0].lease.phase==OC::Phase::Executing);
            assert(bus.serial.frames.empty());
            std::cout<<"BASELINE_ACTIVATE_FALSE_WITH_LIVE_RESERVED_WORK_ZERO_UART\n";
#else
            const char* reason=nullptr;
            const bool current=mode=="worker_lock_current";
            const bool accepted=current?pm.ordinaryModbusWorkCurrent(target,source,work,&reason):
                pm.activateOrdinaryModbusWork(target,source,work,&reason);
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-began).count();
            const auto waited=lastPmWait;
            holder.join();
            assert(elapsed<300 && waited<=100 && waited>0 && bus.serial.frames.empty());
            if(sustained) {assert(!accepted && reason && !strcmp(reason,"CONTROL_BUSY"));assert(elapsed>=80);}
            else if(mode=="worker_lock_deadline") {assert(!accepted && reason && !strcmp(reason,"CONTROL_OPERATION_DEADLINE_EXCEEDED"));assert(waited<=30);}
            else if(mode=="worker_lock_cancel" || mode=="worker_lock_stop") {
                assert(!accepted && reason && !strcmp(reason,"CONTROL_WORK_PHASE_RETIRED"));
            } else assert(accepted && !reason);
            const char* initialReason=reason;
            assert(pm.finishOrdinaryModbusWork(target,source,work,!accepted));
            assert(!pm.finishOrdinaryModbusWork(target,source,work,!accepted));
            if(mode=="worker_lock_stop") {
                assert(pm.activateOrdinaryModbusWork(target,stop,stopWork,&reason));
                assert(pm.submitOrdinaryModbusRelay(bus,target,target.function,target.motorRegisters[2],
                    target.stopValue,stop,result,10,false,stopWork).error==ONESHOT_SUCCESS);
                assert(pm.finishOrdinaryModbusWork(target,stop,stopWork,false));
                assert(bus.serial.frames.size()==1 && bus.serial.frames[0][3]==target.motorRegisters[2]);
            } else assert(bus.serial.frames.empty());
            std::cout<<mode<<" elapsedMs="<<elapsed<<" timeoutTicks="<<waited<<" reason="<<(initialReason?initialReason:"none")<<"\n";
#endif
            return 0;
        }
        auto write=[&](const FastBee::OrdinaryControlContext& c,uint64_t token,uint16_t address,uint16_t value) {
            return pm.submitOrdinaryModbusRelay(bus,target,target.function,address,value,c,result,10,false,token);
        };
        if(mode=="worker_cancel_queued" || mode=="worker_screen_replaced") {
            if(mode=="worker_cancel_queued") terminal(pm,source,true);
            else FastBee::ScreenOutputIdentity::publish(2);
            assert(!pm.activateOrdinaryModbusWork(target,source,work));
            assert(pm.finishOrdinaryModbusWork(target,source,work,true)); terminal(pm,source,true);
            assert(bus.serial.frames.empty());return 0;
        }
        if(mode=="stop_after_queued") {
            auto stop=context(pm,OC::Source::Http,2,30000);uint64_t stopWork=0;
            assert(pm.reserveOrdinaryModbusWork(target,stop,stopWork,result,true));
            terminal(pm,stop);
            assert(!pm.activateOrdinaryModbusWork(target,stop,stopWork));
            assert(!pm.activateOrdinaryModbusWork(target,source,work));
            assert(pm.finishOrdinaryModbusWork(target,source,work,true));
            assert(pm.activateOrdinaryModbusWork(target,stop,stopWork));
            assert(write(stop,stopWork,target.motorRegisters[2],target.stopValue).error==ONESHOT_SUCCESS);
            assert(pm.finishOrdinaryModbusWork(target,stop,stopWork,false));
            assert(bus.serial.frames.size()==1 && pm._ordinaryModbus[0].lease.phase==OC::Phase::Held);return 0;
        }
        assert(pm.activateOrdinaryModbusWork(target,source,work));
        if(mode=="worker_caller_terminal") terminal(pm,source);
        if(mode=="worker_handler_replaced") ++bus._delayInstance;
        if(mode=="stop_parameter_denied") {
            assert(write(source,work,target.motorRegisters[3],10).error!=ONESHOT_SUCCESS);
            assert(bus.serial.frames.empty());terminal(pm,source,true);
            assert(pm.finishOrdinaryModbusWork(target,source,work,true));return 0;
        }
        if(mode=="worker_timeout_running") responseHook=[&]{
            const unsigned held=busDepth;busDepth=0;terminal(pm,source,true);busDepth=held;
            assert(pm._ordinaryModbus[0].inFlight && pm._ordinaryModbus[0].workerReserved);
        };
        FastBee::OrdinaryControlContext stop;
        uint64_t stopWork=0;
        if(mode=="stop_during_response") {
            stop=context(pm,OC::Source::Http,2,30000);
            responseHook=[&]{const unsigned held=busDepth;busDepth=0;
                assert(pm.reserveOrdinaryModbusWork(target,stop,stopWork,result,true));terminal(pm,stop);
                assert(!pm.activateOrdinaryModbusWork(target,stop,stopWork));busDepth=held;};
        }
        if(mode=="motor_fc16_short_echo") bus.serial.fault=HardwareSerial::ShortEcho;
        if(mode=="motor_fc16_wrong_echo") bus.serial.fault=HardwareSerial::WrongEcho;
        const auto first=write(source,work,target.motorRegisters[3],10);
        if(mode=="worker_handler_replaced") {
            assert(first.error!=ONESHOT_SUCCESS && !first.outputAttempted && bus.serial.frames.empty());
            terminal(pm,source,true);assert(pm.finishOrdinaryModbusWork(target,source,work,true));return 0;
        }
        if(mode=="motor_fc16_short_echo" || mode=="motor_fc16_wrong_echo") {
            assert(first.error!=ONESHOT_SUCCESS && first.outputAttempted && bus.serial.frames.size()==1);return 0;
        }
        assert(first.error==ONESHOT_SUCCESS && first.outputAttempted && bus.serial.frames.size()==1);
        if(mode=="worker_source_between_writes") FastBee::SensorConfigEpoch::advance();
        if(mode=="worker_source_between_writes" || mode=="worker_timeout_running" || mode=="stop_during_response") {
            assert(write(source,work,target.motorRegisters[4],50).error!=ONESHOT_SUCCESS && bus.serial.frames.size()==1);
            assert(pm.finishOrdinaryModbusWork(target,source,work,true));
            if(mode=="stop_during_response") {
                assert(pm.activateOrdinaryModbusWork(target,stop,stopWork));
                assert(write(stop,stopWork,target.motorRegisters[2],target.stopValue).error==ONESHOT_SUCCESS);
                assert(pm.finishOrdinaryModbusWork(target,stop,stopWork,false));
                assert(bus.serial.frames.size()==2 && bus.serial.frames.back()[3]==target.motorRegisters[2]);
            }return 0;
        }
        assert(pm.finishOrdinaryModbusWork(target,source,work,false));
        assert(!pm.finishOrdinaryModbusWork(target,source,work,false));
        if(mode=="worker_late_terminal" || mode=="stop_same_invocation") {
            uint64_t newer=0;assert(pm.reserveOrdinaryModbusWork(target,source,newer,result,mode=="stop_same_invocation") && newer!=work);
            assert(!pm.finishOrdinaryModbusWork(target,source,work,false));
            assert(write(source,work,target.motorRegisters[3],10).error!=ONESHOT_SUCCESS && bus.serial.frames.size()==1);
            assert(pm.activateOrdinaryModbusWork(target,source,newer));
            assert(write(source,newer,target.motorRegisters[2],target.stopValue).error==ONESHOT_SUCCESS);
            terminal(pm,source);assert(pm.finishOrdinaryModbusWork(target,source,newer,false));return 0;
        }
        assert(mode=="worker_caller_terminal" || mode=="motor_fc16");
        if(mode=="worker_caller_terminal") assert(pm._ordinaryModbus[0].lease.phase==OC::Phase::Idle);
        if(mode=="motor_fc16") assert(bus.serial.frames[0].size()==11);
        return 0;
    }
    if(mode=="register") bus.config.master.devices[0].controlProtocol=1;
    assert(bus.snapshotOrdinaryRelay(0,device,target));
    if(mode=="mixed_slave_mapping") {
        bus.config.master.deviceCount=2;bus.config.master.devices[1]=device;
        std::strcpy(bus.config.master.devices[1].deviceType,"motor");
        // Same numeric addresses in coil/holding spaces are independent maps.
        assert(bus.snapshotOrdinaryRelay(0,device,target) && bus.serial.frames.empty());return 0;
    }
    auto source=context(pm);
    FastBee::OrdinaryControlResult result;
    const uint8_t function=target.function;
    const uint16_t value=function==5?0xff00:1;
    auto submit=[&](bool toggle=false) { return pm.submitOrdinaryModbusRelay(bus,target,function,target.first,
        toggle?0:value,source,result,10,toggle); };
    if(mode=="gate_once" || mode=="gate_revoked") {
        std::atomic<uint32_t> word{5}; FastBee::ModbusWriteTicket ticket;
        ticket.context=source; ticket.gate=&word; ticket.armed=5; ticket.acquiredMs=0;
        if(mode=="gate_revoked") { word.fetch_or(3); assert(!ticket.claim(1000,1000)); }
        else { unsigned a=0,b=0; std::thread first([&]{a=ticket.claim(1000,1000);});
            std::thread second([&]{b=ticket.claim(1000,1000);}); first.join();second.join(); assert(a+b==1); }
        return 0;
    }
    if(mode=="no_context") source={};
    if(mode=="sequence_exhausted") { assert(pm._ordinaryModbus.ensureAllocated()); pm._ordinaryModbus[0].target=target; pm._ordinaryModbus[0].sequence=0x3fffffffU; }
    if(mode=="busy") bus.semaphore.busy=true;
    if(mode=="travel_rejected") bus.blockTravel=true;
    if(mode=="handler_instance") ++bus._delayInstance;
    if(mode=="handler_generation") ++bus._delayConfiguration;
    if(mode=="wrong_function") target.function=6;
    if(mode=="endpoint_mismatch") ++target.first;
    if(mode=="ambiguous_endpoint") {
        bus.config.master.deviceCount=2; bus.config.master.devices[1]=device;
        assert(!bus.snapshotOrdinaryRelayEndpoint(2,5,10,device,target)); assert(bus.serial.frames.empty()); return 0;
    }
    if(mode=="expiry_during_bus_wait") { source=context(pm,OC::Source::Http,2,10); busWaitHook=[&]{clockUs+=10000;}; }
    if(mode=="deadline_during_bus_wait" || mode=="deadline_wrap_expired" || mode=="deadline_wrap_current") {
        if(mode!="deadline_during_bus_wait") operationClockOffset=UINT32_MAX-1100U;
        source.operationDeadlineAt=mode=="deadline_during_bus_wait"?1100U:mode=="deadline_wrap_expired"?20U:500U;
        busWaitHook=[&]{clockUs+=200000;};
    }
    if(mode=="clock_reverse") busWaitHook=[&]{clockUs-=100000;};
    if(mode=="config_during_bus_wait") busWaitHook=[] { FastBee::SensorConfigEpoch::advance(); };
    if(mode=="rule_during_bus_wait") { source.ruleBound=true; source.ruleEpoch=FastBee::RuleOutputIdentity::currentEpoch();
        busWaitHook=[] { FastBee::RuleOutputIdentity::invalidate(); }; }
    if(mode=="screen_during_bus_wait") { FastBee::ScreenOutputIdentity::publish(1);
        source=context(pm,OC::Source::Screen,(uint64_t(1)<<32)|3,100);
        busWaitHook=[] { FastBee::ScreenOutputIdentity::publish(2); }; }
    if(mode=="uart_during_bus_wait") { pm.uartPortById["uart"]=1; assert(pm.uartReceiveGenerations.begin(1)); FastBee::UartReceiveFault::arm(1, pm.uartReceiveGenerations.current(1));
        const auto origin=FastBee::UartEventOrigin::captured(FastBee::SensorConfigEpoch::current(),pm.uartReceiveGenerations.current(1));
        source=context(pm,OC::Source::Rule,1,0,false,0,"",origin);
        busWaitHook=[&]{ pm.uartReceiveGenerations.end(1); assert(pm.uartReceiveGenerations.begin(1)); FastBee::UartReceiveFault::arm(1, pm.uartReceiveGenerations.current(1)); }; }
    if(mode=="cancel_during_bus_wait") busWaitHook=[&]{terminal(pm,source,true);};
    if(mode=="cancel_during_response" || mode=="terminal_during_response") {
        responseHook=[&] {
            // Another task may own PM while this task owns bus. This callback
            // models that other task; bus must never synchronously call PM.
            const unsigned held=busDepth; busDepth=0;
            terminal(pm,source,mode=="cancel_during_response"); busDepth=held;
            assert(pm._ordinaryModbus[0].inFlight && pm._ordinaryModbus[0].lease.invocationActive);
        };
    }
    if(mode=="timeout" || mode=="toggle_read_failed") bus.serial.fault=HardwareSerial::Timeout;
    if(mode=="crc") bus.serial.fault=HardwareSerial::Crc;
    if(mode=="exception") bus.serial.fault=HardwareSerial::Exception;
    if(mode=="short_write") bus.serial.fault=HardwareSerial::ShortWrite;
    if(mode=="short_echo") bus.serial.fault=HardwareSerial::ShortEcho;
    if(mode=="wrong_echo") bus.serial.fault=HardwareSerial::WrongEcho;
    if(mode=="wrong_fc") bus.serial.fault=HardwareSerial::WrongFc;
    if(mode=="toggle_retired_after_read") responseHook=[] { FastBee::SensorConfigEpoch::advance(); };
    if(mode=="allocation_before_write") travelHook=[] { throw std::bad_alloc(); };
    if(mode=="allocation_after_write") responseHook=[] { throw std::bad_alloc(); };
    if(mode=="terminal_lock_unavailable") responseHook=[] { failPmLock=true; };
    const bool toggle=mode=="toggle" || mode=="toggle_read_failed" || mode=="toggle_retired_after_read";
    const auto first=submit(toggle);
    if(mode=="terminal_lock_unavailable") {
        assert(first.error==ONESHOT_CONTROL_REJECTED && first.outputAttempted);
        assert(pm._ordinaryModbus[0].inFlight && pm._ordinaryModbus[0].lease.invocationActive);
        assert((pm._ordinaryModbus[0].gate.load()&3U)==3U && bus.serial.frames.size()==1);
        assert(!pmDepth && !busDepth); return 0;
    }
    if(mode=="allocation_before_write" || mode=="allocation_after_write") {
        assert(first.error!=ONESHOT_SUCCESS && !bus._ordinaryWriteTicket && !bus.isOneShotReading);
        assert(!pm._ordinaryModbus[0].inFlight && first.outputAttempted==(mode=="allocation_after_write"));
        assert(bus.serial.frames.size()==(mode=="allocation_after_write"?1U:0U));
        terminal(pm,source);source=context(pm);assert(submit().error==ONESHOT_SUCCESS);return 0;
    }
    const bool rejected=mode=="no_context" || mode=="sequence_exhausted" || mode=="busy" || mode=="travel_rejected" ||
        mode=="handler_instance" || mode=="handler_generation" || mode=="wrong_function" || mode=="endpoint_mismatch" ||
        mode=="expiry_during_bus_wait" || mode=="deadline_during_bus_wait" || mode=="deadline_wrap_expired" || mode=="clock_reverse" || mode=="config_during_bus_wait" ||
        mode=="rule_during_bus_wait" || mode=="screen_during_bus_wait" || mode=="uart_during_bus_wait" || mode=="cancel_during_bus_wait";
    if(rejected) { assert(first.error!=ONESHOT_SUCCESS && !first.outputAttempted && bus.serial.frames.empty());
        assert(bus._delayWriteSequence==0); return 0; }
    if(bus.serial.fault!=HardwareSerial::None || mode=="toggle_retired_after_read") {
        assert(first.error!=ONESHOT_SUCCESS && bus.serial.frames.size()==1);
        assert(first.outputAttempted==!toggle); return 0;
    }
    assert(first.error==ONESHOT_SUCCESS && first.outputAttempted && bus.serial.frames.size()==(toggle?2U:1U));
    assert(!pm._ordinaryModbus[0].inFlight);
    if(mode=="cancel_during_response" || mode=="terminal_during_response") {
        assert(pm._ordinaryModbus[0].lease.phase==OC::Phase::Idle);
        assert(submit().error!=ONESHOT_SUCCESS && bus.serial.frames.size()==1); return 0;
    }
    if(mode=="source_conflict" || mode=="same_slave_alias") {
        source=context(pm,OC::Source::Http,2,100);
        if(mode=="same_slave_alias") { bus.config.master.deviceCount=2;
            bus.config.master.devices[1]=device; bus.config.master.devices[1].coilBase=20;
            assert(bus.snapshotOrdinaryRelay(1,device,target)); }
        assert(submit().error!=ONESHOT_SUCCESS && bus.serial.frames.size()==1); return 0;
    }
    if(mode=="different_slave") { bus.config.master.deviceCount=2;
        bus.config.master.devices[1]=device; bus.config.master.devices[1].slaveAddress=3;
        assert(bus.snapshotOrdinaryRelay(1,device,target)); source=context(pm,OC::Source::Http,2,100);
        assert(submit().error==ONESHOT_SUCCESS && bus.serial.frames.size()==2); return 0; }
    if(mode=="prefix_then_failure" || mode=="failure_retained") {
        bus.serial.fault=HardwareSerial::Timeout; const auto failed=submit();
        assert(failed.error!=ONESHOT_SUCCESS && failed.outputAttempted && bus.serial.frames.size()==2);
        bus.serial.fault=HardwareSerial::None; assert(submit().error!=ONESHOT_SUCCESS && bus.serial.frames.size()==2);
        terminal(pm,source); assert(pm._ordinaryModbus[0].failures==1 && pm._ordinaryModbus[0].firstFailureOperation);
        source=context(pm); assert(submit().error==ONESHOT_SUCCESS);
        assert(pm._ordinaryModbus[0].failures==1); return 0;
    }
    terminal(pm,source);
    if(mode=="retired_replay") { assert(submit().error!=ONESHOT_SUCCESS && bus.serial.frames.size()==1); return 0; }
    if(mode=="same_actor_replace" || mode=="review_mismatch") {
        source=context(pm,OC::Source::Http,2,100); assert(submit().error==ONESHOT_SUCCESS); terminal(pm,source);
        if(mode=="review_mismatch") { char key[41]; target.key(key);
            source=context(pm,OC::Source::Http,3,100,true,result.generation+1,key); }
        else source=context(pm,OC::Source::Http,2,100);
        assert((submit().error==ONESHOT_SUCCESS)==(mode=="same_actor_replace"));
        return 0;
    }
    assert(mode=="success" || mode=="register" || mode=="toggle" || mode=="deadline_wrap_current"); return 0;
}
