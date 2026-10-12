#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/MotorControlService.h"
#include "core/MotorCommandQueue.h"
#include "core/ReportInstanceBinding.h"
#include "core/ActionReportPayload.h"
#include "protocols/MotorProtocol.h"
#include "protocols/ModbusMotorReference.h"
#include <map>
#include "core/MotorTravelAdmission.h"
#include "protocols/MqttClientLease.h"
#include <array>
#include <cassert>
#include <vector>
#include <functional>
#include <new>
#include <cstdlib>

static bool hasPsram = true, failPsram = false, failAlloc = false;
static std::vector<unsigned> allocations;
static bool psramFound() { return hasPsram; }
static void* heap_caps_malloc(size_t bytes, unsigned caps) {
    allocations.push_back(caps);
    if (failAlloc || (failPsram && (caps & MALLOC_CAP_SPIRAM))) return nullptr;
    void* result = malloc(bytes);
    if (result) MotorMemoryMock::regions.push_back({reinterpret_cast<uintptr_t>(result), bytes,
                                                   (caps & MALLOC_CAP_SPIRAM) == 0});
    return result;
}

using TickType_t = uint32_t;
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1, pdPASS = 1;
#define pdMS_TO_TICKS(v) (v)
static uint32_t nowMs = 100;
static unsigned pmDepth = 0;
static int64_t esp_timer_get_time() { return static_cast<int64_t>(nowMs)*1000; }
static bool failWorkerValidation=false;
struct RecursiveMutexGuard {
    bool held;
    explicit RecursiveMutexGuard(void*, uint32_t timeout=UINT32_MAX) : held(!failWorkerValidation || timeout==0 || timeout==UINT32_MAX) { if(held)++pmDepth; }
    ~RecursiveMutexGuard() { if(held)--pmDepth; }
    bool isLocked() const { return held; }
};
namespace FastBee { struct UartPortLease { static constexpr uint8_t PORT_COUNT=3; }; }
static uint32_t motorNow() { return nowMs; }
#define millis motorNow
static bool failWorker = false, failLock = false;
static void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
static int xSemaphoreTake(void*, TickType_t) { return failLock ? 0 : pdTRUE; }
static void xSemaphoreGive(void*) {}
static int xTaskCreate(void (*)(void*), const char*, int, void*, int, TaskHandle_t* task) {
    if (failWorker) return 0;
    *task = reinterpret_cast<void*>(2); return pdPASS;
}
static void xTaskNotifyGive(void*) {}
static void vTaskDelete(void*) {}
struct Yield {};
static uint32_t ulTaskNotifyTake(int, uint32_t) { throw Yield{}; }
#define strlcpy motor_fixture_strlcpy
static size_t strlcpy(char* out, const char* input, size_t capacity) {
    const size_t length = std::strlen(input);
    if (capacity) { std::memcpy(out, input, std::min(length, capacity - 1)); out[std::min(length, capacity - 1)] = 0; }
    return length;
}
struct ModbusSubDevice {
    char sensorId[32] = "sensor", name[24] = "Motor", deviceType[16] = "motor";
    uint16_t motorRegs[5] = {1, 2, 3, 5, 0};
    MotorProtocol motorProtocol;
    uint8_t slaveAddress = 3, motorDecimals = 1;
    uint8_t channelCount = 1;
    bool enabled = true, limited = false;
    uint16_t motorLastPulse = 50;
    int32_t motorCurrentPosition = 0, motorMoveStep = 5, motorMaxPosition = 100, motorMinPosition = -100;
    bool hasMotorSoftLimit() const { return limited; }
};
constexpr int MODBUS_MASTER = 1, ONESHOT_SUCCESS = 0, ONESHOT_TIMEOUT = 1, ONESHOT_CRC_ERROR = 2,
    ONESHOT_EXCEPTION = 3, ONESHOT_NOT_INITIALIZED = 4, ONESHOT_BUSY = 5, ONESHOT_TRAVEL_BLOCKED = 6,
    ONESHOT_CONTROL_REJECTED = 7;
struct OneShotResult { int error = 0; uint16_t data[32] = {23, 0}; uint16_t count = 1; uint8_t exceptionCode = 0; const char* travelError = nullptr; bool outputAttempted = false; const char* controlReason = nullptr; };
struct Frame { uint16_t reg, value; uint8_t retries; bool write; uint8_t fc = 6; };
static std::function<void()> onWrite;
static std::function<void()> onRead;
struct ModbusHandler {
    ModbusSubDevice dev;
    uint32_t instanceId = 1, configuration = 1;
    bool snapshotOrdinaryMotorReference(const String& ref, ModbusSubDevice& out, FastBee::ModbusControlTarget& target) {
        assert(!pmDepth);
        if (!dev.enabled || ref != FastBee::motorControlReference(dev)) return false;
        target = {}; target.kind=FastBee::ModbusControlTarget::Kind::Motor;
        target.instance=instanceId;target.configuration=configuration;target.slave=dev.slaveAddress;
        target.function=dev.motorProtocol.writeFunction;target.stopValue=dev.motorProtocol.stopValue;
        std::memcpy(target.motorRegisters,dev.motorRegs,sizeof(target.motorRegisters));
        out=dev;return target.valid();
    }
    OneShotResult writeOrdinaryFrameOnce(const FastBee::ModbusControlTarget&,const FastBee::ModbusOutputFrame&,
        const FastBee::ModbusWriteTicket&,uint16_t,uint8_t*,size_t,size_t*) {
        assert(false && "Motor service must use its retained worker path");
        OneShotResult result;result.error=ONESHOT_CONTROL_REJECTED;return result;
    }
    OneShotResult writeOrdinaryRelayOnce(const FastBee::ModbusControlTarget& target, uint8_t function,
        uint16_t address,uint16_t value,const FastBee::ModbusWriteTicket& ticket,uint16_t timeout,bool toggle) {
        assert(!pmDepth && !toggle && function==dev.motorProtocol.writeFunction);
        ModbusSubDevice snapshot;FastBee::ModbusControlTarget current;
        OneShotResult result;result.error=ONESHOT_CONTROL_REJECTED;
        if (!snapshotOrdinaryMotorReference(FastBee::motorControlReference(dev),snapshot,current) ||
            !target.sameMapping(current) || !ticket.claim(nowMs, nowMs)) return result;
        const size_t before=frames.size();
        result=function==16 ? writeMultipleRegistersOnce(dev.slaveAddress,address,1,&value,true,timeout,0) :
            writeRegisterOnce(dev.slaveAddress,address,value,true,timeout,0);
        result.outputAttempted=frames.size()!=before;
        result.controlReason=result.error==ONESHOT_SUCCESS?"CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY":"CONTROL_RESULT_UNKNOWN";
        return result;
    }
    std::vector<Frame> frames;
    int failRegister = -1;
    int failureCode = ONESHOT_TIMEOUT;
    uint16_t resultCount = 1;
    uint16_t readSpeed = 23, readPulse = 23;
    uint32_t latency = 0;
    bool travelBlocked = false, pulseReadbackRequired = false;
    bool guarded = false;
    bool parametersFollowWrites = false;
    bool snapshotBusyOnce = false;
    FastBee::MotorTravelAdmission admission;
    unsigned confirmations = 0;
    bool getMotorTravel(uint8_t, FastBee::MotorTravelAdmission& out, String& token) {
        out = {}; token = "test-token";
        if (snapshotBusyOnce) { snapshotBusyOnce = false; return false; }
        if (guarded) { out = admission; return true; }
        if (pulseReadbackRequired) {
            auto& m = out.mapping;
            m.valid = m.finite = true; m.slave = 3; m.readFunction = 3; m.writeFunction = 6;
            const uint16_t regs[] = {1,2,3,5,0}; std::copy(regs, regs+5, m.regs);
            m.minimum = -100; m.maximum = 100;
            out.position.configure(-100,100); out.position.confirm(0); out.stationary = true;
        }
        return true;
    }
    bool confirmMotorTravel(uint8_t, const String& token, bool, int32_t, bool stationary, String& error) {
        ++confirmations;
        if (token != "test-token" || !stationary) { error = "TRAVEL_STATE_CHANGED"; return false; }
        return true;
    }
    int getMode() { return MODBUS_MASTER; }
    uint8_t getSubDeviceCount() { return 1; }
    const ModbusSubDevice& getSubDevice(uint8_t) { return dev; }
    OneShotResult readRegistersOnce(uint8_t, uint8_t fc, uint16_t reg, uint16_t count, bool, uint16_t, uint8_t retries) {
        assert(count == 1); frames.push_back({reg, 0, retries, false, fc}); nowMs += latency;
        if (onRead) { auto callback = std::move(onRead); onRead = {}; callback(); }
        OneShotResult result; result.error = reg == failRegister ? failureCode : 0;
        result.count = resultCount; result.exceptionCode = result.error == ONESHOT_EXCEPTION ? 2 : 0;
        result.data[0] = reg == dev.motorProtocol.readRegister(dev.motorRegs, false) ? readSpeed : readPulse;
        if (guarded) {
            const uint8_t request[] = {dev.slaveAddress, fc, uint8_t(reg>>8), uint8_t(reg), 0, 1, 0, 0};
            const uint8_t reply[] = {dev.slaveAddress, fc, 2, uint8_t(result.data[0]>>8), uint8_t(result.data[0]), 0, 0};
            admission.after(request, sizeof(request), reply, result.count == 1 ? sizeof(reply) : 0, !result.error, nowMs);
        }
        return result;
    }
    OneShotResult writeRegisterOnce(uint8_t, uint16_t reg, uint16_t value, bool, uint16_t, uint8_t retries) {
        const uint8_t fc = dev.motorProtocol.writeFunction;
        std::vector<uint8_t> request{dev.slaveAddress, fc, uint8_t(reg>>8), uint8_t(reg), uint8_t(value>>8), uint8_t(value), 0, 0};
        if (fc == 16) request = {dev.slaveAddress, fc, request[2], request[3], 0, 1, 2, request[4], request[5], 0, 0};
        if (guarded) if (const char* reason = admission.before(request.data(), request.size(), nowMs)) {
            OneShotResult blocked; blocked.error = ONESHOT_TRAVEL_BLOCKED; blocked.travelError = reason; return blocked;
        }
        if (travelBlocked && reg != dev.motorRegs[2]) {
            OneShotResult blocked; blocked.error = ONESHOT_TRAVEL_BLOCKED;
            blocked.travelError = "TRAVEL_RANGE_EXCEEDED"; return blocked;
        }
        frames.push_back({reg, value, retries, true}); nowMs += latency;
        if (onWrite) { auto callback = std::move(onWrite); onWrite = {}; callback(); }
        OneShotResult result; result.error = reg == failRegister ? 1 : 0;
        if (!result.error && parametersFollowWrites) {
            if (reg == dev.motorRegs[3]) readSpeed = value;
            if (reg == dev.motorRegs[4]) readPulse = value;
        }
        if (guarded) {
            uint8_t reply[8]{}; std::copy(request.begin(), request.begin()+6, reply);
            admission.after(request.data(), request.size(), reply, sizeof(reply), !result.error, nowMs);
        }
        return result;
    }
    OneShotResult writeMultipleRegistersOnce(uint8_t slave, uint16_t reg, uint16_t count,
                                              const uint16_t* values, bool control, uint16_t timeout, uint8_t retries) {
        assert(count == 1 && control && retries == 0);
        auto result = writeRegisterOnce(slave, reg, *values, control, timeout, retries);
        if (result.error != ONESHOT_TRAVEL_BLOCKED) frames.back().fc = 16;
        return result;
    }
    void updateMotorRuntime(uint8_t, int32_t pos, uint16_t pulse) { dev.motorCurrentPosition = pos; dev.motorLastPulse = pulse; }
    String buildSensorId(uint8_t, uint8_t) { return "motor_sensor"; }
};
static ModbusHandler bus;
class MQTTClient {
public:
    std::vector<String> reports;
    uint32_t instanceId = 11;
    bool connected = true, accepting = true;
    uint32_t getReportInstanceId() const { return instanceId; }
    bool getIsConnected() const { return connected; }
    bool queueReportData(const String& report) {
        if (!accepting) return false;
        reports.push_back(report); return true;
    }
};
static MQTTClient mqtt;
static bool mqttAvailable = true;
struct PeripheralManager {
    uint32_t epoch = 7;
    std::atomic<uint32_t> generation{19};
    bool busy = false;
    void* _mutex=reinterpret_cast<void*>(1);
    uint64_t _nextOrdinaryControlOperation=0,_nextOrdinaryControlGeneration=0,_ordinaryModbusRetiredFloor=0;
    FastBee::ModbusOrdinarySlots _ordinaryModbus;
    struct Port { bool owned() const { return true; } } uartPortLeases[3];
    std::map<String,uint8_t> uartPortById{{String("uart"),1}};
    struct Generations {
        std::atomic<uint32_t>* value;
        uint32_t current(uint8_t) const { return value->load(); }
        const std::atomic<uint32_t>* handle(uint8_t) const { return value; }
    } uartReceiveGenerations{&generation};
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) {
        return origin.unbound() || (!busy && origin.matches(epoch, generation.load()));
    }
    bool beginOrdinaryModbusMaintenance(uint64_t,uint32_t,FastBee::OrdinaryControlContext&);
    bool beginOrdinaryControl(FastBee::OrdinaryControl::Source,uint64_t,uint32_t,bool,uint64_t,
        const String&,uint32_t,const FastBee::UartEventOrigin&,FastBee::OrdinaryControlContext&) {
        assert(false && "Motor worker must keep its original context, not issue maintenance authority"); return false;
    }
    void finishOrdinaryModbusLocked(const FastBee::OrdinaryControlContext&,bool);
    void serviceOrdinaryModbusLocked();
    FastBee::ModbusOrdinarySlot* acquireOrdinaryModbusLocked(const FastBee::ModbusControlTarget&,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,uint64_t,uint64_t=0);
    OneShotResult submitOrdinaryModbusRelay(ModbusHandler&,const FastBee::ModbusControlTarget&,uint8_t,uint16_t,uint16_t,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,uint16_t=0,bool=false,uint64_t=0);
    OneShotResult submitOrdinaryModbusFrame(ModbusHandler&,const FastBee::ModbusControlTarget&,
        const FastBee::ModbusOutputFrame&,const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&,
        uint16_t=0,uint8_t* =nullptr,size_t=0,size_t* =nullptr,uint64_t=0);
    bool reserveOrdinaryModbusWork(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,
        uint64_t&,FastBee::OrdinaryControlResult&,bool=false);
    bool activateOrdinaryModbusWork(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,uint64_t, const char** rejectionReason=nullptr);
    bool ordinaryModbusWorkCurrent(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,uint64_t, const char** rejectionReason=nullptr);
    bool finishOrdinaryModbusWork(const FastBee::ModbusControlTarget&,const FastBee::OrdinaryControlContext&,uint64_t,bool);
    void reset() {
        for(auto& slot:_ordinaryModbus) { slot.~ModbusOrdinarySlot(); new(&slot) FastBee::ModbusOrdinarySlot(); }
        epoch=7;generation.store(19);busy=false;
        _nextOrdinaryControlOperation=0;_nextOrdinaryControlGeneration=0;_ordinaryModbusRetiredFloor=0;
    }
};
#include "modbus_backend_production.inc"

static std::function<void()> onLease;
struct ProtocolManager {
    struct ModbusHandlerLease { ModbusHandler* get() { return &bus; } };
    ModbusHandlerLease acquireModbusHandler(uint32_t) {
        if (onLease) { auto callback = std::move(onLease); onLease = {}; callback(); }
        return {};
    }
    FastBee::MqttClientLifetime mqttLifetime;
    FastBee::MqttClientLease acquireMQTTClient() {
        FastBee::MqttClientLifetime::Read read(mqttLifetime);
        return FastBee::MqttClientLease(std::move(read), mqttAvailable ? &mqtt : nullptr);
    }
};
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { static FastBeeFramework value; return &value; }
    ProtocolManager* getProtocolManager() { static ProtocolManager value; return &value; }
};
struct PeriphExecManager {
    std::vector<String> events;
    std::vector<FastBee::UartEventOrigin> attemptedOrigins;
    struct ReportAttempt { String payload; uint32_t origin, current; };
    std::vector<ReportAttempt> reportAttempts, retainedReports;
    bool canRetain = true;
    static PeriphExecManager& getInstance() { static PeriphExecManager value; return value; }
    void triggerEventById(const String&, const String& event, const FastBee::UartEventOrigin& origin) {
        attemptedOrigins.push_back(origin);
        if (PeripheralManager::getInstance().isUartEventOriginCurrent(origin)) events.push_back(event);
    }
    bool queuePendingReport(const String& payload, uint32_t origin, uint32_t current) {
        ReportAttempt attempt{payload,origin,current}; reportAttempts.push_back(attempt);
        if (!canRetain || !origin || (current && current != origin)) return false;
        retainedReports.push_back(attempt); return true;
    }
};
#include "motor_service_production.inc"

static bool submitOutput(MotorControlService& motors,const String& ref,const String& action,int value,
    const String& request,uint32_t& operation,String& error) {
    if(action=="readStatus") return motors.submit(ref,action,value,request,operation,error);
    auto& pm=PeripheralManager::getInstance();FastBee::OrdinaryControlContext control;
    control.configEpoch=FastBee::SensorConfigEpoch::current();
    control.request.actor={FastBee::OrdinaryControl::Source::Mqtt,1};
    control.request.operation=++pm._nextOrdinaryControlOperation;
    const bool accepted=motors.submit(ref,action,value,request,operation,error,&control);
    { RecursiveMutexGuard lock(pm._mutex); pm.finishOrdinaryModbusLocked(control,!accepted); }
    return accepted;
}
static bool submitRuleOutput(MotorControlService& motors,const String& ref,const String& action,int value,
    const String& request,uint32_t budget,uint16_t timeout,uint32_t& operation,String& error,
    const FastBee::UartEventOrigin& origin={}) {
    auto& pm=PeripheralManager::getInstance();FastBee::OrdinaryControlContext control;
    control.configEpoch=FastBee::SensorConfigEpoch::current();control.uartOrigin=origin;
    control.operationDeadlineAt=nowMs+budget;
    control.request.actor={FastBee::OrdinaryControl::Source::Rule,1};
    control.request.operation=++pm._nextOrdinaryControlOperation;
    const bool accepted=motors.submitRule(ref,action,value,request,budget,timeout,operation,error,origin,&control);
    { RecursiveMutexGuard lock(pm._mutex); pm.finishOrdinaryModbusLocked(control,!accepted); }
    return accepted;
}

static void runWorker() { try { worker(nullptr); } catch (const Yield&) {} }
static void reset() {
    service()->commands = {}; service()->states = {}; service()->worker = nullptr;
    bus = {}; mqtt = {}; mqttAvailable = true; PeriphExecManager::getInstance().events.clear();
    PeriphExecManager::getInstance().reportAttempts.clear();
    PeriphExecManager::getInstance().retainedReports.clear();
    PeriphExecManager::getInstance().canRetain = true;
    PeriphExecManager::getInstance().attemptedOrigins.clear();
    PeripheralManager::getInstance().reset(); onLease = {};
    failWorker = failLock = false; nowMs = 100; onWrite = {}; onRead = {};
    uint32_t operation = 0; String error;
    auto& motors = MotorControlService::instance();
    assert(submitOutput(motors,motors.referenceForSlave(3), "readStatus", 0, "", operation, error));
    runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    bus.frames.clear(); service()->commands = {}; service()->worker = nullptr;
}
int main(int argc, char** argv) {
    while(FastBee::SensorConfigEpoch::current()<7) FastBee::SensorConfigEpoch::advance();
    const std::string mode = argc > 1 ? argv[1] : "psram";
    hasPsram = mode != "internal";
    failPsram = mode == "fallback";
    failAlloc = mode == "failure";
    auto& motors = MotorControlService::instance();
    uint32_t operation = 0; String error;
    if (mode == "no-rules") {
        MotorCommands::Command cmd;
        cmd.reportInstanceId = 11;
        reportAcknowledgement(cmd,"sensor","stop",0);
        assert(cmd.reportState == MotorCommands::ReportState::Queued && mqtt.reports.size() == 1);
        mqtt.accepting = false;
        reportAcknowledgement(cmd,"sensor","stop",0);
        assert(cmd.reportState == MotorCommands::ReportState::Dropped);
        assert(PeriphExecManager::getInstance().reportAttempts.empty());
        const std::string hugeId(600, 'x');
        reportAcknowledgement(cmd,hugeId.c_str(),"stop",0);
        assert(cmd.reportState == MotorCommands::ReportState::InvalidPayload && mqtt.reports.size() == 1);
        reportAcknowledgement(cmd,"","stop",0);
        assert(cmd.reportState == MotorCommands::ReportState::InvalidPayload && mqtt.reports.size() == 1);
        assert(bus.frames.empty()); return 0;
    }
    if (failAlloc) {
        assert(!submitOutput(motors,motors.referenceForSlave(3), "forward", 0, "no-memory", operation, error));
        MotorControlState state; JsonDocument result;
        assert(!motors.state("motor", state));
        assert(!motors.operation(1, result.to<JsonObject>()));
        assert(bus.frames.empty());
        assert(allocations.size() == 2);
        return 0;
    }
    reset();
    assert(allocations.size() == (failPsram ? 2U : 1U));
    assert(allocations.back() == ((hasPsram && !failPsram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT));
    std::printf("Motor service cache: %zu bytes, allocation: %s\n", sizeof(Service), mode.c_str());
    String ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "forward", 0, "forward", operation, error));
    const uint32_t original = operation;
    assert(submitOutput(motors,ref, "forward", 0, "forward", operation, error) && operation == original);
    runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 1 && bus.frames[0].retries == 0);
    JsonDocument result;
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == true && result["motionKnown"] == false);
    assert(mqtt.reports.size() == 1 && PeriphExecManager::getInstance().events.size() == 1);
    assert(PeriphExecManager::getInstance().events[0].indexOf("motor-result") >= 0);
    // Replay after completion returns the original result without another
    // physical start-register write or another event/MQTT publication.
    assert(submitOutput(motors,ref,"forward",0,"forward",operation,error) && operation==original);
    runWorker(); assert(bus.frames.size()==1 && mqtt.reports.size()==1);
    // A duplicate arriving while the actual Modbus write is in progress is
    // deduplicated too; count frames at the bus boundary, not in a fake queue.
    reset(); ref=motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"reverse",0,"running-reverse",operation,error));
    const uint32_t running=operation;
    onWrite=[&](){uint32_t duplicate=0;String detail;
        assert(submitOutput(motors,ref,"reverse",0,"running-reverse",duplicate,detail) && duplicate==running);};
    runWorker(); assert(bus.frames.size()==1 && bus.frames[0].reg==2 && bus.frames[0].retries==0);
    assert(mqtt.reports.size()==1 && PeriphExecManager::getInstance().events.size()==1);

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    bus.dev.motorCurrentPosition = 90;
    assert(submitOutput(motors,ref, "forward", 0, "limited", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 1);
    assert(bus.dev.motorCurrentPosition == 90); // ACK must never advance coordinates.
    bus.travelBlocked = true;
    assert(submitOutput(motors,ref, "forward", 0, "at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 1);
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == false);
    assert(result["error"] == "TRAVEL_RANGE_EXCEEDED");

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    bus.dev.motorCurrentPosition = -90;
    assert(submitOutput(motors,ref, "reverse", 0, "reverse-limited", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 2 && bus.dev.motorCurrentPosition == -90);
    bus.travelBlocked = true;
    assert(submitOutput(motors,ref, "reverse", 0, "reverse-at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 1);
    assert(submitOutput(motors,ref, "stop", 0, "stop-at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[1].reg == 3);

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true; bus.travelBlocked = true;
    assert(submitOutput(motors,ref, "forward", 0, "failed-pulse", operation, error)); runWorker();
    assert(bus.frames.empty());
    assert(mqtt.reports.empty());

    reset(); ref = motors.referenceForSlave(3); bus.failRegister = 1;
    assert(submitOutput(motors,ref, "forward", 0, "lost-reply", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].retries == 0);
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == false);
    const uint32_t uncertain=operation;
    assert(submitOutput(motors,ref,"forward",0,"lost-reply",operation,error) && operation==uncertain);
    runWorker(); assert(bus.frames.size()==1);
    assert(motors.operation(operation,result.to<JsonObject>()) && result["success"]==false);
    assert(mqtt.reports.empty() && PeriphExecManager::getInstance().events.empty());

    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "readStatus", 0, "read", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    MotorControlState state; assert(motors.state(ref, state) && state.known && state.speed == 23 && state.precision == 1);
    nowMs += 15001; assert(motors.state(ref, state) && !state.known);

    // Queued polling does not disable controls. A real control is accepted
    // and reaches the bus before the lower-priority background read.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "readStatus", 0, "poll", operation, error));
    const uint32_t queuedRead = operation;
    assert(motors.state(ref, state) && !state.pending);
    assert(motors.operation(queuedRead, result.to<JsonObject>()) && result["pending"] == true);
    assert(submitOutput(motors,ref, "setSpeed", 42, "user-speed", operation, error));
    assert(motors.state(ref, state) && state.pending);
    assert(!submitOutput(motors,ref, "forward", 0, "second-control", operation, error));
    runWorker();
    assert(bus.frames.size() == 3 && bus.frames[0].write && bus.frames[0].reg == 5 && bus.frames[0].value == 42);
    assert(!bus.frames[1].write && !bus.frames[2].write);
    assert(motors.state(ref, state) && !state.pending);

    // Reproduce a disconnected/sluggish motor while its read is RUNNING.
    // The touch command queues once, stays visibly pending until completed,
    // and readStatus's public operation status remains accurate.
    reset(); ref = motors.referenceForSlave(3); bus.failRegister = 5; bus.latency = 1000;
    assert(submitOutput(motors,ref, "readStatus", 0, "slow-poll", operation, error));
    const uint32_t runningRead = operation;
    uint32_t clicked = 0;
    onRead = [&]() {
        assert(motors.state(ref, state) && !state.pending);
        assert(motors.operation(runningRead, result.to<JsonObject>()) && result["pending"] == true);
        assert(submitOutput(motors,ref, "forward", 0, "touch-during-poll", clicked, error));
        assert(motors.state(ref, state) && state.pending);
        assert(!submitOutput(motors,ref, "reverse", 0, "double-touch", operation, error));
    };
    runWorker();
    assert(bus.frames.size() == 4);
    for (const auto& frame : bus.frames) assert(!frame.write);
    assert(motors.operation(clicked, result.to<JsonObject>()) && result["success"] == false);
    assert(String(result["error"].as<const char*>()).indexOf("TIMEOUT") >= 0);
    assert(motors.state(ref, state) && !state.pending && !state.known);

    // Stop remains available during a read and cancels a queued movement.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "readStatus", 0, "stop-poll", operation, error));
    uint32_t cancelled = 0;
    onRead = [&]() {
        assert(submitOutput(motors,ref, "reverse", 0, "queued-motion", cancelled, error));
        assert(submitOutput(motors,ref, "stop", 0, "urgent-stop", operation, error));
    };
    runWorker();
    assert(bus.frames.size() == 2 && bus.frames[1].write && bus.frames[1].reg == 3);
    assert(motors.operation(cancelled, result.to<JsonObject>()) && result["pending"] == false && result["success"] == false);
    assert(motors.state(ref, state) && !state.pending);

    // Readback failures do not replace the stop acknowledgement or its ID.
    const uint32_t stopped = operation;
    bus.failRegister = 5;
    assert(submitOutput(motors,ref, "readStatus", 0, "after-stop", operation, error)); runWorker();
    assert(motors.state(ref, state) && !state.known && state.readError[0]);
    assert(state.operationId == stopped && state.commandKnown && state.commandSuccess);
    assert(!strcmp(state.lastAction, "stop") && !state.error[0]);
    assert(submitOutput(motors,ref, "reverse", 0, "unreadable-start", operation, error));
    bus.frames.clear(); runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    assert(motors.state(ref, state) && state.commandKnown && !state.commandSuccess);
    assert(!strcmp(state.lastAction, "reverse") && state.error[0]);
    const uint32_t failedStart = operation; bus.failRegister = -1;
    assert(submitOutput(motors,ref, "readStatus", 0, "recovered-read", operation, error)); runWorker();
    assert(motors.state(ref, state) && state.known && !state.readError[0]);
    assert(state.operationId == failedStart && state.commandKnown && !state.commandSuccess && state.error[0]);

    reset(); ref = motors.referenceForSlave(3); bus.readPulse = 0;
    assert(submitOutput(motors,ref, "readStatus", 0, "read-zero-pulse", operation, error)); runWorker();
    assert(motors.state(ref, state) && state.known && state.pulse == 0); bus.frames.clear();
    assert(submitOutput(motors,ref, "forward", 0, "zero-pulse-forward", operation, error)); runWorker();
    assert(submitOutput(motors,ref, "reverse", 0, "zero-pulse-reverse", operation, error)); runWorker();
    assert(!bus.frames.empty());
    for(const auto& frame:bus.frames) if(frame.write) assert(frame.reg==bus.dev.motorRegs[4] && frame.value==100);
    const size_t zeroDefaults = bus.frames.size();
    assert(submitOutput(motors,ref, "stop", 0, "zero-pulse-stop", operation, error)); runWorker();
    assert(bus.frames.size() == zeroDefaults + 1 && bus.frames.back().reg == 3);

    // No pulse prewrite remains. STOP follows an already-dispatched start.
    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    assert(submitOutput(motors,ref, "forward", 0, "preparing-move", operation, error));
    const uint32_t preparing = operation;
    onWrite = [&]() { assert(submitOutput(motors,ref, "stop", 0, "interrupt-prewrite", operation, error)); };
    runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 1 && bus.frames[1].reg == 3);
    // The already-ACKed START remains evidence, but the cancelled invocation
    // cannot publish success after a newer STOP has claimed this motor.
    assert(motors.operation(preparing, result.to<JsonObject>()) && result["success"] == false);
    assert(result["error"] == "Cancelled by stop" && result["pending"] == false);
    assert(result["terminalAcknowledged"] == true && result["outputAttemptKnown"] == true);
    assert(result["outputAttempted"] == true && result["physicalOutputVerified"] == false);
    assert(result["outputSteps"].size() == 1);
    assert(result["outputSteps"][0]["address"] == 1 && result["outputSteps"][0]["value"] == 1);
    assert(result["outputSteps"][0]["attempted"] == true &&
           result["outputSteps"][0]["commandAcknowledged"] == true &&
           result["outputSteps"][0]["physicalOutputVerified"] == false);
    const uint32_t interruptStop = operation;
    assert(motors.operation(interruptStop, result.to<JsonObject>()) &&
           result["success"] == true && result["pending"] == false);
    assert(result["outputSteps"].size() == 1 &&
           result["outputSteps"][0]["address"] == 3 &&
           result["outputSteps"][0]["commandAcknowledged"] == true);
    assert(mqtt.reports.size() == 1 && mqtt.reports.back().indexOf("stop:1") >= 0);
    assert(PeriphExecManager::getInstance().events.size() == 1);
    assert(motors.state(ref, state) && state.commandKnown && state.commandSuccess &&
           state.operationId == interruptStop && !strcmp(state.lastAcknowledgedAction, "stop"));
    // Repeating the original request returns its cancelled record, never START.
    assert(submitOutput(motors,ref,"forward",0,"preparing-move",operation,error) && operation == preparing);
    runWorker(); assert(bus.frames.size() == 2 && mqtt.reports.size() == 1);
    assert(motors.operation(preparing,result.to<JsonObject>()) &&
           result["success"] == false && result["outputSteps"].size() == 1);

    reset(); ref = motors.referenceForSlave(3); nowMs += 15001;
    assert(submitOutput(motors,ref, "forward", 0, "stale-parameters", operation, error));
    runWorker();
    assert(bus.frames.size() == 3 && !bus.frames[0].write && !bus.frames[1].write && bus.frames[2].reg == 1);
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == true);
    const auto staleOperation = operation;
    assert(submitOutput(motors,ref, "forward", 0, "stale-parameters", operation, error) && operation == staleOperation);
    runWorker(); assert(bus.frames.size() == 3);
    assert(submitOutput(motors,ref, "stop", 0, "stop-without-feedback", operation, error));
    runWorker(); assert(bus.frames.size() == 4 && bus.frames[3].reg == 3);

    reset(); ref = motors.referenceForSlave(3); nowMs += 15001; bus.failRegister = 5;
    assert(submitOutput(motors,ref, "forward", 0, "refresh-failed", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == false);

    reset(); ref = motors.referenceForSlave(3); nowMs += 15001;
    onRead = [&]() { assert(submitOutput(motors,ref, "stop", 0, "stop-during-refresh", operation, error)); };
    assert(submitOutput(motors,ref, "forward", 0, "refresh-interrupted", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && bus.frames[1].reg == 3 && bus.frames[1].write);

    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "setPulse", 0, "zero-pulse", operation, error)); runWorker();
    bus.readPulse = 0;
    bus.frames.clear();
    assert(submitOutput(motors,ref, "forward", 0, "unverified-pulse", operation, error));
    runWorker();
    assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
    for(const auto& frame:bus.frames) if(frame.write) assert(frame.reg==bus.dev.motorRegs[4] && frame.value==100);

    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "forward", 0, "launch", operation, error));
    assert(submitOutput(motors,ref, "stop", 0, "stop", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 3);

    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "forward", 0, "rebind", operation, error));
    bus.dev.motorRegs[0] = 55; runWorker(); assert(bus.frames.empty());

    reset(); ref = motors.referenceForSlave(3); failWorker = true;
    assert(!submitOutput(motors,ref, "forward", 0, "no-worker", operation, error));
    failWorker = false; failLock = true;
    assert(!submitOutput(motors,ref, "forward", 0, "no-lock", operation, error));

    reset(); ref = motors.referenceForSlave(3);
    assert(submitRuleOutput(motors,ref, "forward", 7, "rule", 5000, 1000, operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].value == 7);
    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    assert(submitRuleOutput(motors,ref, "forward", 7, "rule-limited", 5000, 1000, operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].value == 7); // Transport validates the action word.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitRuleOutput(motors,ref, "forward", 7, "expired-rule", 5000, 1000, operation, error));
    nowMs += 5000; runWorker(); assert(bus.frames.empty());
    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true; bus.latency = 3000;
    assert(submitRuleOutput(motors,ref, "forward", 7, "expired-between-writes", 5000, 1000, operation, error));
    runWorker(); assert(bus.frames.size() == 1 && bus.frames[0].reg == 1);
    // Generic layout: one action register, distinct words, FC04 feedback at
    // separate addresses and FC16 writes. No device/model name is consulted.
    reset();
    bus.dev.motorRegs[0] = bus.dev.motorRegs[1] = bus.dev.motorRegs[2] = 100;
    bus.dev.motorRegs[3] = 101; bus.dev.motorRegs[4] = 102;
    auto& profile = bus.dev.motorProtocol;
    profile.configured = true; profile.readFunction = 4; profile.writeFunction = 16;
    profile.speedReadRegister = 200; profile.pulseReadRegister = 201;
    profile.forwardValue = 17; profile.reverseValue = 34; profile.stopValue = 0;
    ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "readStatus", 0, "generic-read", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 200 && bus.frames[1].reg == 201);
    assert(bus.frames[0].fc == 4 && bus.frames[1].fc == 4);
    bus.frames.clear();
    assert(submitOutput(motors,ref, "forward", 0, "generic-forward", operation, error)); runWorker();
    assert(submitOutput(motors,ref, "reverse", 0, "generic-reverse", operation, error)); runWorker();
    assert(submitOutput(motors,ref, "stop", 0, "generic-stop", operation, error)); runWorker();
    assert(bus.frames.size() == 3 && bus.frames[0].value == 17 && bus.frames[1].value == 34 && bus.frames[2].value == 0);
    for (const auto& frame : bus.frames) assert(frame.reg == 100 && frame.fc == 16 && frame.retries == 0);
    // Rules must also use the profile's direction value, never legacy raw 7.
    assert(submitRuleOutput(motors,ref, "reverse", 7, "generic-rule", 5000, 1000, operation, error)); runWorker();
    assert(bus.frames.back().value == 34);
    assert(submitOutput(motors,ref, "setSpeed", 49, "generic-speed", operation, error)); runWorker();
    assert(bus.frames.back().reg == 101 && bus.frames.back().value == 49 && bus.frames.back().fc == 16);
    assert(motors.state(ref, state) && !state.known);
    const size_t written = bus.frames.size();
    assert(submitOutput(motors,ref, "forward", 0, "generic-dirty", operation, error)); runWorker();
    assert(bus.frames.size() == written + 3 && !bus.frames[written].write && !bus.frames[written+1].write);
    assert(bus.frames.back().write && bus.frames.back().reg == 100 && bus.frames.back().value == 17);

    // Every execution-semantic change invalidates queued commands and feedback.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "forward", 0, "profile-changed", operation, error));
    bus.dev.motorProtocol.stopValue = 0; runWorker(); assert(bus.frames.empty());
    assert(motors.referenceForSlave(3) != ref);
    reset(); ref = motors.referenceForSlave(3);
    bus.dev.motorProtocol.configured = true;
    bus.dev.motorProtocol.speedReadRegister = 5; bus.dev.motorProtocol.pulseReadRegister = 0;
    assert(motors.referenceForSlave(3) != ref); // Explicit profiles change rule-value semantics too.

    // Unsafe/non-finite capability cannot be bypassed by known parameters.
    reset(); bus.dev.motorProtocol.finitePulseMove = false; ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "readStatus", 0, "nonfinite-read", operation, error)); runWorker(); bus.frames.clear();
    assert(submitOutput(motors,ref, "forward", 0, "nonfinite", operation, error)); runWorker(); assert(bus.frames.empty());
    assert(submitOutput(motors,ref, "stop", 0, "nonfinite-stop", operation, error)); runWorker(); assert(bus.frames.size() == 1);

    for (int reason : {ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR, ONESHOT_EXCEPTION, ONESHOT_NOT_INITIALIZED, ONESHOT_BUSY}) {
        reset(); ref = motors.referenceForSlave(3); bus.failRegister = 5; bus.failureCode = reason;
        assert(submitOutput(motors,ref, "readStatus", 0, "error-detail", operation, error)); runWorker();
        assert(motors.state(ref, state) && !state.known);
        assert(strstr(state.readError, "speed s3 FC03 r5") && strstr(state.readError, "pulse s3 FC03 r0 OK"));
        if (reason == ONESHOT_EXCEPTION) assert(strstr(state.readError, "EXCEPTION(0x02)"));
        bus.frames.clear(); assert(submitOutput(motors,ref, "forward", 0, "error-start", operation, error)); runWorker();
        assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    }
    reset(); ref = motors.referenceForSlave(3); bus.resultCount = 0;
    assert(submitOutput(motors,ref, "readStatus", 0, "empty-read", operation, error)); runWorker();
    assert(motors.state(ref, state) && !state.known && strstr(state.readError, "SHORT_REPLY"));
    reset(); ref = motors.referenceForSlave(3); bus.pulseReadbackRequired = true;
    assert(submitOutput(motors,ref, "forward", 0, "pulse-invalidated-by-stop", operation, error)); runWorker();
    assert(bus.frames.size() == 3 && !bus.frames[0].write && !bus.frames[1].write && bus.frames[2].write);
    // Run the real travel admission together with the production service.
    // Fresh parameters permit STOP/start without two redundant reads or creating
    // physical-rest feedback or narrowing the possible position interval.
    for (bool generic : {false, true}) {
        auto prepareMoving = [&]() {
            reset(); bus.guarded = true; bus.dev.limited = true;
            if (generic) {
                bus.dev.motorRegs[0] = bus.dev.motorRegs[1] = bus.dev.motorRegs[2] = 100;
                bus.dev.motorRegs[3] = 101; bus.dev.motorRegs[4] = 102;
                auto& p = bus.dev.motorProtocol;
                p.configured = true; p.readFunction = 4; p.writeFunction = 16;
                p.speedReadRegister = 200; p.pulseReadRegister = 201;
                p.forwardValue = 17; p.reverseValue = 34; p.stopValue = 0;
            }
            FastBee::MotorTravelMapping mapping;
            mapping.required = mapping.valid = mapping.finite = true; mapping.slave = 3;
            const auto& p = bus.dev.motorProtocol;
            mapping.readFunction = p.readFunction; mapping.writeFunction = p.writeFunction;
            mapping.pulseReadRegister = p.readRegister(bus.dev.motorRegs, true);
            std::copy(bus.dev.motorRegs,bus.dev.motorRegs+5,mapping.regs);
            mapping.values[0]=p.forwardValue; mapping.values[1]=p.reverseValue; mapping.values[2]=p.stopValue;
            mapping.minimum=-100; mapping.maximum=100;
            bus.admission.configure(mapping); assert(bus.admission.confirmPosition(0,true));
            ref = motors.referenceForSlave(3);
            assert(submitOutput(motors,ref,"readStatus",0,"guarded-read",operation,error)); runWorker();
            assert(submitOutput(motors,ref,"forward",0,"guarded-first",operation,error)); runWorker();
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
            assert(bus.admission.position.lower==0 && bus.admission.position.upper==23);
            bus.frames.clear();
        };
        prepareMoving();
        assert(submitOutput(motors,ref,"reverse",0,"guarded-reverse",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(bus.frames.size()==2 && bus.frames[0].write && bus.frames[1].write);
        assert(bus.frames[0].reg==bus.dev.motorRegs[2] && bus.frames[0].value==bus.dev.motorProtocol.stopValue);
        assert(bus.frames[1].reg==bus.dev.motorRegs[1] && bus.frames[1].value==bus.dev.motorProtocol.reverseValue);
        for(const auto& frame:bus.frames)assert(frame.retries==0);
        assert(bus.admission.position.lower==-23 && bus.admission.position.upper==23 && !bus.admission.stationary);
        const auto once=operation;
        assert(submitOutput(motors,ref,"reverse",0,"guarded-reverse",operation,error)&&operation==once);
        runWorker();assert(bus.frames.size()==2);
        // Repeated commands retain the full accumulated bounds.
        for(unsigned i=0;i<3;++i){
            assert(submitOutput(motors,ref,"reverse",0,String("bounded-")+String(i),operation,error));runWorker();
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        }
        assert(bus.admission.position.lower==-92 && bus.admission.position.upper==23);
        assert(submitOutput(motors,ref,"reverse",0,"range-exhausted",operation,error));runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
        assert(String(result["error"].as<const char*>())=="TRAVEL_RANGE_EXCEEDED");
        assert(bus.admission.position.lower==-92 && bus.admission.position.upper==23);

        // A short-lived snapshot lock cannot be interpreted as permission to
        // skip STOP. Preparation recovers in this same explicit command.
        prepareMoving(); bus.snapshotBusyOnce=true;
        assert(submitOutput(motors,ref,"reverse",0,"snapshot-busy-reverse",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(bus.frames.size()==4 && bus.frames[0].write && !bus.frames[1].write &&
               !bus.frames[2].write && bus.frames[3].write);
        assert(bus.frames[0].reg==bus.dev.motorRegs[2] && bus.frames[0].value==bus.dev.motorProtocol.stopValue);
        assert(bus.frames[3].reg==bus.dev.motorRegs[1] && bus.frames[3].value==bus.dev.motorProtocol.reverseValue);
        assert(bus.admission.position.lower==-23 && bus.admission.position.upper==23);
        for(const auto& frame:bus.frames)assert(frame.retries==0);

        prepareMoving();
        bus.dev.motorProtocol.commandPosition = true;
        bus.dev.motorProtocol.startupReference = true;
        bus.dev.motorProtocol.startupPosition = 0;
        auto commandedMap = bus.admission.mapping; commandedMap.commandPosition = true;
        commandedMap.startupReference = true; commandedMap.startupPosition = 0;
        bus.admission.configure(commandedMap);
        assert(bus.admission.position.referenced && !bus.admission.stationary);
        ref = motors.referenceForSlave(3);
        for (unsigned cycle = 0; cycle < 40; ++cycle) {
            if (cycle == 20) { bus.admission = {}; bus.admission.configure(commandedMap); }
            nowMs += 61000; // Expire cached feedback and completed queue slots.
            for (const char* action : {"forward", "reverse"}) {
                bus.frames.clear();
                assert(submitOutput(motors,ref, action, 0, String(action)+String(cycle), operation, error)); runWorker();
                assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == true);
                unsigned starts = 0;
                for (const auto& frame : bus.frames) {
                    assert(frame.retries == 0);
                    if (frame.write && ((frame.reg == bus.dev.motorRegs[0] && frame.value == bus.dev.motorProtocol.forwardValue) ||
                        (frame.reg == bus.dev.motorRegs[1] && frame.value == bus.dev.motorProtocol.reverseValue))) ++starts;
                }
                assert(starts == 1);
                assert(bus.frames.front().write && bus.frames.front().reg == bus.dev.motorRegs[2]);
                MotorControlState current; assert(motors.state(ref, current) && current.commandPosition);
                assert(current.estimatedPosition == (!strcmp(action, "forward") ? 23 : 0));
            }
            assert(bus.admission.position.lower == 0 && bus.admission.position.upper == 0);
        }
        nowMs += 16000;
        MotorControlState cached; assert(motors.state(ref, cached));
        assert(cached.parametersCached && !cached.parametersFresh && !cached.known && cached.pulse == 23);

        prepareMoving();
        auto manualMap = bus.admission.mapping; manualMap.minimum = manualMap.maximum = 0;
        bus.admission.configure(manualMap); bus.dev.limited = false;
        assert(submitOutput(motors,ref,"reverse",0,"default-manual-start",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(!bus.admission.position.referenced && bus.frames.size()==4);
        bus.frames.clear();
        assert(submitOutput(motors,ref,"forward",0,"fast-manual-reverse",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(bus.frames.size()==2 && bus.frames[0].write && bus.frames[1].write);
        assert(bus.frames[0].value==bus.dev.motorProtocol.stopValue);
        assert(bus.frames[1].value==bus.dev.motorProtocol.forwardValue);
        assert(submitOutput(motors,ref,"stop",0,"manual-stop",operation,error)); runWorker();
        bus.frames.clear();
        assert(submitOutput(motors,ref,"reverse",0,"already-stopped-start",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(bus.frames.size()==1 && bus.frames[0].write);
        bus.readSpeed=bus.readPulse=0; bus.parametersFollowWrites=true;
        assert(submitOutput(motors,ref,"readStatus",0,"factory-zeros",operation,error)); runWorker();
        bus.frames.clear();
        assert(submitOutput(motors,ref,"forward",0,"factory-defaults-click",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(bus.readSpeed==10 && bus.readPulse==100 && !bus.admission.position.referenced);
        assert(motors.state(ref,state) && state.known && state.speed==10 && state.pulse==100);
        unsigned defaultStarts=0;
        for(const auto& frame:bus.frames) {
            assert(frame.retries==0);
            if(frame.write && frame.reg==bus.dev.motorRegs[0] && frame.value==bus.dev.motorProtocol.forwardValue) ++defaultStarts;
        }
        assert(defaultStarts==1);

        auto prepareManual = [&](uint16_t speed, uint16_t pulse) {
            prepareMoving();
            auto mapping = bus.admission.mapping; mapping.minimum = mapping.maximum = 0;
            bus.admission.configure(mapping); bus.dev.limited = false;
            bus.readSpeed = speed; bus.readPulse = pulse; bus.parametersFollowWrites = true;
            assert(submitOutput(motors,ref,"readStatus",0,"manual-parameters",operation,error)); runWorker();
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
            bus.frames.clear();
        };
        auto startCount = [&]() {
            unsigned count = 0;
            for (const auto& frame : bus.frames) {
                assert(frame.retries == 0);
                assert(frame.fc == (frame.write ? bus.dev.motorProtocol.writeFunction : bus.dev.motorProtocol.readFunction));
                if (frame.write && ((frame.reg == bus.dev.motorRegs[0] && frame.value == bus.dev.motorProtocol.forwardValue) ||
                    (frame.reg == bus.dev.motorRegs[1] && frame.value == bus.dev.motorProtocol.reverseValue))) ++count;
            }
            return count;
        };
        // Only zero parameters get defaults. Nonzero values, including the
        // 16-bit boundary, survive a single first click in either direction.
        const uint16_t initialParameters[][2] = {{0,0},{0,65535},{65535,0},{31,73}};
        for (const auto& initial : initialParameters) for (const char* action : {"forward","reverse"}) {
            prepareManual(initial[0], initial[1]);
            assert(submitOutput(motors,ref,action,0,"manual-default-matrix",operation,error)); runWorker();
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
            const uint16_t expectedSpeed = initial[0] ? initial[0] : 10;
            const uint16_t expectedPulse = initial[1] ? initial[1] : 100;
            assert(bus.readSpeed == expectedSpeed && bus.readPulse == expectedPulse);
            assert(motors.state(ref,state) && state.known && state.speed == expectedSpeed && state.pulse == expectedPulse);
            unsigned speedWrites = 0, pulseWrites = 0;
            for (const auto& frame : bus.frames) if (frame.write) {
                if (frame.reg == bus.dev.motorRegs[3]) { ++speedWrites; assert(frame.value == 10); }
                if (frame.reg == bus.dev.motorRegs[4]) { ++pulseWrites; assert(frame.value == 100); }
            }
            assert(speedWrites == (initial[0] ? 0U : 1U) && pulseWrites == (initial[1] ? 0U : 1U));
            assert(startCount() == 1 && !bus.admission.position.referenced);
            if (!initial[0] || !initial[1]) {
                const auto n = bus.frames.size(); assert(n >= 3);
                assert(!bus.frames[n-3].write && bus.frames[n-3].reg == bus.dev.motorProtocol.readRegister(bus.dev.motorRegs,false));
                assert(!bus.frames[n-2].write && bus.frames[n-2].reg == bus.dev.motorProtocol.readRegister(bus.dev.motorRegs,true));
                assert(bus.frames[n-1].write);
            }
            const auto once = operation; const auto frames = bus.frames.size();
            assert(submitOutput(motors,ref,action,0,"manual-default-matrix",operation,error) && operation == once);
            runWorker(); assert(bus.frames.size() == frames);
        }
        // Parameter-write failure, echoed-but-ignored writes, failed readback
        // and short replies cannot produce a START or replay on reconnection.
        enum class DefaultFailure { SPEED_WRITE, PULSE_WRITE, IGNORED_WRITE, SPEED_READ, PULSE_READ, SHORT_READ };
        for (auto failure : {DefaultFailure::SPEED_WRITE,DefaultFailure::PULSE_WRITE,DefaultFailure::IGNORED_WRITE,
                             DefaultFailure::SPEED_READ,DefaultFailure::PULSE_READ,DefaultFailure::SHORT_READ})
        for (const char* action : {"forward","reverse"}) {
            prepareManual(0,0);
            if (failure == DefaultFailure::SPEED_WRITE) bus.failRegister = bus.dev.motorRegs[3];
            if (failure == DefaultFailure::PULSE_WRITE) bus.failRegister = bus.dev.motorRegs[4];
            if (failure == DefaultFailure::IGNORED_WRITE) bus.parametersFollowWrites = false;
            if (failure == DefaultFailure::SPEED_READ || failure == DefaultFailure::PULSE_READ || failure == DefaultFailure::SHORT_READ) {
                onRead = [&]() {
                    assert(bus.readSpeed == 10 && bus.readPulse == 100);
                    if (failure == DefaultFailure::SHORT_READ) bus.resultCount = 0;
                    else bus.failRegister = bus.dev.motorProtocol.readRegister(bus.dev.motorRegs,failure == DefaultFailure::PULSE_READ);
                };
            }
            assert(submitOutput(motors,ref,action,0,"default-failed",operation,error)); runWorker();
            const auto failed = operation;
            assert(motors.operation(failed,result.to<JsonObject>()) && result["success"] == false);
            assert(String(result["error"].as<const char*>()).length() > 0 && startCount() == 0);
            if (failure == DefaultFailure::IGNORED_WRITE) assert(result["error"] == "Motor default parameters were not accepted");
            if (failure == DefaultFailure::SHORT_READ) assert(String(result["error"].as<const char*>()).indexOf("SHORT_REPLY") >= 0);
            const auto frames = bus.frames.size();
            bus.failRegister = -1; bus.resultCount = 1; bus.parametersFollowWrites = true;
            assert(submitOutput(motors,ref,action,0,"default-failed",operation,error) && operation == failed);
            runWorker(); assert(bus.frames.size() == frames);
            assert(submitOutput(motors,ref,action,0,"default-recovered-new-click",operation,error)); runWorker();
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
            assert(startCount() == 1 && bus.readSpeed == 10 && bus.readPulse == 100);
            assert(motors.operation(failed,result.to<JsonObject>()) && result["success"] == false);
        }
        // STOP during either default write takes priority over the requested
        // direction. Completing the parameter write must not release a START.
        for (unsigned parameter = 0; parameter < 2; ++parameter) for (const char* action : {"forward","reverse"}) {
            prepareManual(0,0); uint32_t stopOperation = 0, startOperation = 0;
            std::function<void()> interruptDefault;
            interruptDefault = [&]() {
                if (bus.frames.back().reg == bus.dev.motorRegs[3+parameter])
                    assert(submitOutput(motors,ref,"stop",0,"stop-during-default",stopOperation,error));
                else onWrite = interruptDefault;
            };
            onWrite = interruptDefault;
            assert(submitOutput(motors,ref,action,0,"default-cancelled",startOperation,error)); runWorker();
            assert(stopOperation && startCount() == 0);
            assert(motors.operation(startOperation,result.to<JsonObject>()) && result["success"] == false);
            assert(result["error"] == "Cancelled by stop");
            assert(motors.operation(stopOperation,result.to<JsonObject>()) && result["success"] == true);
            assert(bus.frames.back().write && bus.frames.back().reg == bus.dev.motorRegs[2]);
            assert(bus.frames.back().value == bus.dev.motorProtocol.stopValue);
        }

        prepareMoving();bus.failRegister=bus.dev.motorRegs[2];
        assert(submitOutput(motors,ref,"reverse",0,"stop-fails",operation,error));runWorker();
        assert(bus.frames.size()==1 && bus.frames[0].write && bus.admission.position.referenced);
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
        assert(String(result["error"].as<const char*>()).indexOf("pre-start stop")>=0);
        assert(bus.admission.position.lower==0 && bus.admission.position.upper==23);
        assert(!bus.admission.stationary && !bus.admission.stopAcknowledged && !bus.admission.pulseKnown);
        const uint32_t failedStopOperation = operation;
        bus.failRegister=-1;
        // Restoring communication does not replay the failed request.
        assert(submitOutput(motors,ref,"reverse",0,"stop-fails",operation,error) && operation==failedStopOperation);
        runWorker(); assert(bus.frames.size()==1);
        bus.frames.clear();
        assert(submitOutput(motors,ref,"reverse",0,"stop-recovered-new-click",operation,error));runWorker();
        assert(bus.frames.size()==4 && bus.frames[0].write && !bus.frames[1].write && !bus.frames[2].write && bus.frames[3].write);
        assert(bus.frames[0].reg==bus.dev.motorRegs[2] && bus.frames[0].value==bus.dev.motorProtocol.stopValue);
        assert(bus.frames[3].reg==bus.dev.motorRegs[1] && bus.frames[3].value==bus.dev.motorProtocol.reverseValue);
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
        assert(bus.admission.position.lower==-23 && bus.admission.position.upper==23);
        for(const auto& frame:bus.frames)assert(frame.retries==0);

        prepareMoving();bus.failRegister=bus.dev.motorProtocol.readRegister(bus.dev.motorRegs,false);
        nowMs += 16000;
        assert(submitOutput(motors,ref,"reverse",0,"read-after-stop-fails",operation,error));runWorker();
        assert(bus.frames.size()==3 && bus.frames[0].write && !bus.frames[1].write && !bus.frames[2].write);
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
        assert(bus.admission.position.referenced && bus.admission.position.upper==23 && !bus.admission.stationary);

        prepareMoving();
        uint32_t requested=0;
        onWrite=[&](){assert(submitOutput(motors,ref,"stop",0,"operator-stop-during-preparation",operation,error));};
        assert(submitOutput(motors,ref,"reverse",0,"cancel-prepared-start",requested,error));runWorker();
        for(const auto& frame:bus.frames)if(frame.write)assert(frame.value==bus.dev.motorProtocol.stopValue && frame.reg==bus.dev.motorRegs[2]);
        assert(motors.operation(requested,result.to<JsonObject>()) && result["success"] == false);
        assert(bus.admission.position.lower==0 && bus.admission.position.upper==23);

        // Setting speed/pulse does not require a reference or configured
        // travel range. The first write is preceded by a single STOP.
        for (bool configured : {false, true}) for (const char* action : {"setSpeed", "setPulse"}) {
            prepareMoving(); bus.admission.invalidate();
            if (!configured) {
                auto mapping = bus.admission.mapping; mapping.maximum = mapping.minimum;
                bus.admission.configure(mapping);
            }
            assert(submitOutput(motors,ref,action,100,"unreferenced-parameter",operation,error));runWorker();
            assert(bus.frames.size()==2 && bus.frames[0].reg==bus.dev.motorRegs[2]);
            assert(bus.frames[1].reg==bus.dev.motorRegs[!strcmp(action,"setSpeed")?3:4] && bus.frames[1].value==100);
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == true);
            assert(!bus.admission.position.referenced && !bus.admission.stationary && !bus.admission.pulseKnown);
            const uint32_t once=operation;
            assert(submitOutput(motors,ref,action,100,"unreferenced-parameter",operation,error)&&operation==once);
            runWorker();assert(bus.frames.size()==2);
            bus.frames.clear();
            assert(submitOutput(motors,ref,"forward",0,"still-no-reference",operation,error));runWorker();
            assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == !configured);
            if(configured) for(const auto& frame:bus.frames)assert(!frame.write);
            else assert(bus.frames.back().write && bus.frames.back().value==bus.dev.motorProtocol.forwardValue);
        }
        prepareMoving();bus.admission.invalidate();bus.failRegister=bus.dev.motorRegs[2];
        assert(submitOutput(motors,ref,"setPulse",100,"parameter-stop-fails",operation,error));runWorker();
        assert(bus.frames.size()==1 && bus.frames[0].reg==bus.dev.motorRegs[2]);
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
        prepareMoving();bus.admission.invalidate();
        uint32_t parameterOperation=0;
        onWrite=[&](){assert(submitOutput(motors,ref,"stop",0,"cancel-parameter",operation,error));};
        assert(submitOutput(motors,ref,"setPulse",100,"parameter-cancelled",parameterOperation,error));runWorker();
        assert(bus.frames.size()==2);
        for(const auto& frame:bus.frames)assert(frame.reg==bus.dev.motorRegs[2] && frame.value==bus.dev.motorProtocol.stopValue);
        assert(motors.operation(parameterOperation,result.to<JsonObject>()) && result["success"] == false);

        prepareMoving();bus.latency=3000;
        assert(submitRuleOutput(motors,ref,"reverse",0,"pre-start-deadline",5000,1000,operation,error));runWorker();
        assert(bus.frames.size()==1 && bus.frames[0].reg==bus.dev.motorRegs[2]);
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);

        prepareMoving();
        assert(submitRuleOutput(motors,ref,"reverse",0,"uart-pre-start-stop",5000,1000,operation,error,
                                FastBee::UartEventOrigin::captured(7,19)));
        onWrite=[](){ ++PeripheralManager::getInstance().generation; }; runWorker();
        assert(bus.frames.size()==1 && bus.frames[0].reg==bus.dev.motorRegs[2]);
        assert(motors.operation(operation,result.to<JsonObject>()) && result["error"] == "uart_source_unconfirmed");
    }
    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "forward", 0, "shared-forward", operation, error)); runWorker();
    assert(motors.state(ref, state) && !strcmp(state.lastAcknowledgedAction, "forward"));
    assert(submitOutput(motors,ref, "setSpeed", 70, "shared-speed", operation, error)); runWorker();
    assert(motors.state(ref, state) && !strcmp(state.lastAcknowledgedAction, "forward"));
    nowMs += 5000;
    assert(submitOutput(motors,ref, "readStatus", 0, "shared-read", operation, error)); runWorker();
    assert(motors.state(ref, state) && state.known && !strcmp(state.lastAcknowledgedAction, "forward"));
    JsonDocument lookup; bool busy = false;
    failLock = true;
    assert(!motors.operation(operation, lookup.to<JsonObject>(), &busy) && busy);
    failLock = false;
    assert(!motors.operation(0, lookup.to<JsonObject>(), &busy) && !busy);
    assert(motors.operation(operation, lookup.to<JsonObject>(), &busy) && !busy);
    assert(lookup["parametersKnown"].as<bool>() && !lookup["motionKnown"].as<bool>());
    bus.failRegister = bus.dev.motorRegs[1];
    assert(submitOutput(motors,ref, "reverse", 0, "failed-reverse", operation, error)); runWorker();
    assert(motors.state(ref, state) && !state.commandSuccess && !strcmp(state.lastAcknowledgedAction, "forward"));
    assert(submitOutput(motors,ref, "stop", 0, "shared-stop", operation, error)); runWorker();
    assert(motors.state(ref, state) && !strcmp(state.lastAcknowledgedAction, "stop"));
    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref, "forward", 0, "reference-race", operation, error));
    assert(!motors.confirmTravel(ref, "test-token", true, 50, true, error));
    assert(error == "TRAVEL_COMMAND_PENDING" && bus.confirmations == 0);
    onWrite = [&]() {
        assert(!motors.confirmTravel(ref, "test-token", true, 50, true, error));
        assert(error == "TRAVEL_COMMAND_PENDING" && bus.confirmations == 0);
    };
    runWorker(); bus.frames.clear();
    assert(!motors.confirmTravel(ref, "old-token", true, 50, true, error));
    assert(!motors.confirmTravel(ref, "test-token", true, 50, false, error));
    assert(motors.confirmTravel(ref, "test-token", true, 50, true, error));
    assert(bus.frames.empty());
    const auto uart = FastBee::UartEventOrigin::captured(7,19);
    auto& source = PeripheralManager::getInstance();
    auto& events = PeriphExecManager::getInstance();
    // Admission rejects stale, malformed and currently unconfirmable sources.
    for (int scenario = 0; scenario < 3; ++scenario) {
        reset(); ref = motors.referenceForSlave(3);
        auto origin = uart;
        if (scenario == 0) ++source.generation;
        if (scenario == 1) origin = FastBee::UartEventOrigin::captured(0,0);
        if (scenario == 2) source.busy = true;
        operation = 777;
        assert(!submitRuleOutput(motors,ref,"forward",1,"uart-rejected",5000,1000,operation,error,origin));
        assert(operation == 0 && error == "uart_source_unconfirmed");
        assert(bus.frames.empty() && !service()->worker);
    }
    // A queued command retains its capture; worker never rebinds it.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitRuleOutput(motors,ref,"forward",1,"uart-queued",5000,1000,operation,error,uart));
    ++source.generation; runWorker();
    assert(bus.frames.empty() && mqtt.reports.empty() && events.events.empty());
    assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
    assert(result["error"] == "uart_source_unconfirmed");
    // Waiting for the handler may retire the UART after entry validation.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitRuleOutput(motors,ref,"forward",1,"uart-lease",5000,1000,operation,error,uart));
    onLease = [&](){ ++source.epoch; }; runWorker();
    assert(bus.frames.empty());
    assert(motors.operation(operation,result.to<JsonObject>()) && result["error"] == "uart_source_unconfirmed");
    // Read substeps cannot publish a mixed/retired parameter snapshot.
    for (int retireAfter = 1; retireAfter <= 2; ++retireAfter) {
        reset(); ref = motors.referenceForSlave(3);
        assert(submitRuleOutput(motors,ref,"readStatus",0,"uart-read",5000,1000,operation,error,uart));
        onRead = [&](){
            if (retireAfter == 1) ++source.generation;
            else onRead = [&](){ ++source.generation; };
        };
        runWorker();
        assert(bus.frames.size() == static_cast<size_t>(retireAfter));
        assert(motors.operation(operation,result.to<JsonObject>()) && result["parametersKnown"] == false);
        assert(result["error"] == "uart_source_unconfirmed" && events.events.empty());
    }
    // Recursive parameter refresh carries the token and cannot proceed to start.
    reset(); ref = motors.referenceForSlave(3); nowMs += 16000;
    assert(submitRuleOutput(motors,ref,"forward",1,"uart-refresh",5000,1000,operation,error,uart));
    onRead = [&](){ ++source.generation; }; runWorker();
    assert(bus.frames.size() == 1 && !bus.frames[0].write);
    assert(motors.operation(operation,result.to<JsonObject>()) && result["error"] == "uart_source_unconfirmed");
    // Successful result event inherits the exact token.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitRuleOutput(motors,ref,"forward",1,"uart-valid",5000,1000,operation,error,uart)); runWorker();
    assert(bus.frames.size() == 1 && events.events.size() == 1);
    assert(events.attemptedOrigins.size() == 1 && events.attemptedOrigins[0].matches(7,19));
    // A write that already happened stays in the accepted output prefix.
    // Source retirement suppresses result events and stops the invocation.
    reset(); ref = motors.referenceForSlave(3);
    assert(submitRuleOutput(motors,ref,"forward",1,"uart-write",5000,1000,operation,error,uart));
    onWrite = [&](){ ++source.generation; }; runWorker();
    assert(bus.frames.size() == 1 && mqtt.reports.empty() && events.events.empty());
    assert(events.attemptedOrigins.empty());
    assert(motors.operation(operation,result.to<JsonObject>()) && result["success"] == false);
    assert(result["outputSteps"].size()==1 && result["outputSteps"][0]["commandAcknowledged"]==true);
    // Ordinary stop cannot coalesce into a bound stop that will be rejected.
    reset(); ref = motors.referenceForSlave(3);
    uint32_t boundStop = 0;
    assert(submitRuleOutput(motors,ref,"stop",0,"uart-stop",5000,1000,boundStop,error,uart));
    ++source.generation; source.busy = true;
    assert(submitOutput(motors,ref,"stop",0,"manual-stop",operation,error) && operation != boundStop);
    runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == bus.dev.motorRegs[2]);
    assert(events.attemptedOrigins.size() == 1 && events.attemptedOrigins[0].unbound());
    // Report admission is separate from the write ACK and from broker delivery.
    for (int scenario = 0; scenario < 10; ++scenario) {
        reset(); ref = motors.referenceForSlave(3);
        if (scenario == 3) mqttAvailable = false; // Unknown at admission stays unknown.
        if (scenario == 8) mqtt.instanceId = 0; // Test-only/unknown identity.
        if (scenario == 9) { bus.dev.sensorId[0] = '\0'; ref = motors.referenceForSlave(3); }
        assert(submitOutput(motors,ref,"forward",1,"report-origin",operation,error));
        const uint32_t firstOperation = operation;
        if (scenario == 0) mqtt.connected = false;
        if (scenario == 1) mqtt.accepting = false;
        if (scenario == 2) mqtt.instanceId = 22;
        if (scenario == 3) mqttAvailable = true;
        if (scenario == 4) mqttAvailable = false;
        if (scenario == 5) { mqtt.accepting = false; events.canRetain = false; }
        if (scenario == 6) onWrite = [&](){mqtt.instanceId=22;};
        if (scenario == 7) onWrite = [&](){strlcpy(bus.dev.sensorId,"new-mapping",sizeof(bus.dev.sensorId));};
        runWorker();
        // Changing the sensor ID changes the motor reference, so its cached
        // parameters must be read again before issuing the single start.
        if (scenario == 9) {
            assert(bus.frames.size() == 3);
            assert(!bus.frames[0].write && bus.frames[0].reg == bus.dev.motorRegs[3]);
            assert(!bus.frames[1].write && bus.frames[1].reg == bus.dev.motorRegs[4]);
        } else {
            assert(bus.frames.size() == 1);
        }
        assert(bus.frames.back().write && bus.frames.back().reg == bus.dev.motorRegs[0]);
        assert(events.events.size() == 1);
        assert(motors.operation(firstOperation,result.to<JsonObject>()) && result["success"] == true);
        assert(result["reportDeliveryVerified"] == false);
        const bool retained = scenario == 0 || scenario == 1 || scenario == 4;
        const bool queued = scenario == 7;
        const char* expected = retained ? "retained_locally" : queued ? "queued_locally" :
            scenario == 9 ? "invalid_payload" : "not_retained";
        assert(result["reportAdmission"] == expected);
        assert(events.retainedReports.size() == (retained ? 1U : 0U));
        assert(mqtt.reports.size() == (queued ? 1U : 0U));
        if (queued) {
            JsonDocument payload; assert(!deserializeJson(payload,mqtt.reports[0].c_str()));
            assert(payload[0]["id"] == "sensor" && payload[0]["remark"] == "command acknowledged");
        } else if (scenario != 9) {
            assert(events.reportAttempts.size() == 1);
            assert(events.reportAttempts[0].origin == (scenario == 3 || scenario == 8 ? 0U : 11U));
            assert(events.reportAttempts[0].current == (scenario == 4 || scenario == 8 ? 0U :
                scenario == 2 || scenario == 6 ? 22U : 11U));
        }
        if (scenario == 2) {
            // Replaying the same operation after rebuilding MQTT does not
            // replace its report origin or emit a second acknowledgement.
            assert(submitOutput(motors,ref,"forward",1,"report-origin",operation,error) && operation == firstOperation);
            runWorker(); assert(bus.frames.size() == 1 && events.reportAttempts.size() == 1);
        }
    }
    reset(); ref = motors.referenceForSlave(3);
    bus.dev.channelCount = 2;
    assert(submitOutput(motors,ref,"stop",0,"multichannel-report",operation,error));
    onWrite = [&](){
        auto* protocols = FastBeeFramework::getInstance()->getProtocolManager();
        FastBee::MqttClientLifetime::Write writer(protocols->mqttLifetime);
        assert(writer); // The admission lease never spans the bus write.
        bus.dev.channelCount = 1;
    };
    runWorker();
    JsonDocument payload; assert(mqtt.reports.size() == 1 && !deserializeJson(payload,mqtt.reports[0].c_str()));
    assert(payload[0]["id"] == "sensor_ch0");
    assert(motors.operation(operation,result.to<JsonObject>()) && result["reportAdmission"] == "queued_locally");
    reset(); ref = motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"readStatus",0,"no-ack-report",operation,error)); runWorker();
    assert(motors.operation(operation,result.to<JsonObject>()) && result["reportAdmission"] == "not_applicable");
    assert(mqtt.reports.empty() && events.reportAttempts.empty());
    reset(); ref = motors.referenceForSlave(3);
    uint32_t cancelledReport = 0;
    assert(submitOutput(motors,ref,"forward",1,"cancelled-report",cancelledReport,error));
    assert(submitOutput(motors,ref,"stop",0,"stop-cancel",operation,error));
    assert(motors.operation(cancelledReport,result.to<JsonObject>()) && result["reportAdmission"] == "not_produced");
    assert(motors.operation(operation,result.to<JsonObject>()) && result["reportAdmission"] == "pending");
    runWorker(); assert(mqtt.reports.size() == 1);

    // MQTT properties retain their frozen identifier and requested enum/value,
    // while only the shared worker publishes an acknowledged result.
    for(int scenario=0;scenario<3;++scenario) {
        reset();ref=motors.referenceForSlave(3);
        auto& pm=PeripheralManager::getInstance();FastBee::OrdinaryControlContext control;
        control.configEpoch=FastBee::SensorConfigEpoch::current();control.request.operation=++pm._nextOrdinaryControlOperation;
        control.request.actor={FastBee::OrdinaryControl::Source::Mqtt,77};
        if(scenario==1) {bus.dev.motorProtocol.configured=true;bus.dev.motorProtocol.stopValue=9;ref=motors.referenceForSlave(3);}
        const char* action=scenario==1?"stop":"setSpeed";
        const char* property=scenario==1?"sensor_oper":"sensor_spd";
        const uint16_t requested=scenario==1?0:42;
        if(scenario==2)control.request.actor.source=FastBee::OrdinaryControl::Source::Rule;
        const bool accepted=motors.submitMqtt(ref,action,42,"mqtt-property",property,requested,operation,error,control);
        {RecursiveMutexGuard lock(pm._mutex);pm.finishOrdinaryModbusLocked(control,!accepted);}
        assert(accepted==(scenario!=2));
        if(!accepted){assert(bus.frames.empty() && mqtt.reports.empty());continue;}
        onWrite=[] {strlcpy(bus.dev.sensorId,"replacement",sizeof(bus.dev.sensorId));};
        runWorker();assert(bus.frames.size()==1 && mqtt.reports.size()==1);
        assert(bus.frames[0].value==(scenario==1?9:42));
        JsonDocument propertyResult;assert(!deserializeJson(propertyResult,mqtt.reports[0].c_str()));
        assert(propertyResult[0]["id"]==property && propertyResult[0]["value"]==String(requested).c_str());
        // A replay with another frozen report identity cannot obtain this result.
        strlcpy(bus.dev.sensorId,"sensor",sizeof(bus.dev.sensorId));
        uint32_t conflicting=0;
        assert(!motors.submitMqtt(ref,action,42,"mqtt-property","other",requested,conflicting,error,control));
        assert(bus.frames.size()==1 && mqtt.reports.size()==1);
    }
    // While the worker owns an in-flight frame, its private result has not
    // reached the queue. The operation API must report unknown, not zero.
    reset();ref=motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"forward",0,"running-observation",operation,error));
    onWrite=[&](){
        JsonDocument pending;assert(motors.operation(operation,pending.to<JsonObject>()));
        assert(pending["pending"]==true && pending["success"]==false);
        assert(pending["outputAttemptKnown"]==false && pending["outputAttempted"].isNull());
    };
    runWorker();
    assert(motors.operation(operation,result.to<JsonObject>()));
    assert(result["outputAttemptKnown"]==true && result["outputAttempted"]==true && result["outputSteps"].size()==1);
    // A lease wait consumes the existing caller deadline; queue admission does
    // not create a fresh operation budget after the snapshot.
    reset();ref=motors.referenceForSlave(3);
    FastBee::OrdinaryControlContext deadlineControl;
    auto& deadlinePm=PeripheralManager::getInstance();
    deadlineControl.configEpoch=FastBee::SensorConfigEpoch::current();
    deadlineControl.request.actor={FastBee::OrdinaryControl::Source::Rule,42};
    deadlineControl.request.operation=++deadlinePm._nextOrdinaryControlOperation;
    deadlineControl.operationDeadlineAt=nowMs+5000;
    onLease=[] { nowMs+=2500; };
    assert(motors.submitRule(ref,"forward",0,"original-deadline",5000,1000,operation,error,{},&deadlineControl));
    { RecursiveMutexGuard lock(deadlinePm._mutex);deadlinePm.finishOrdinaryModbusLocked(deadlineControl,false); }
    for(const auto& c:service()->commands) if(c.id==operation) assert(c.deadlineMs==deadlineControl.operationDeadlineAt);
    nowMs+=2600;runWorker();
    assert(bus.frames.empty() && motors.operation(operation,result.to<JsonObject>()));
    assert(result["success"]==false && result["terminalAcknowledged"]==true);
    // New M2 integration checks execute the production service and PM backend.
    reset();ref=motors.referenceForSlave(3);
    assert(!motors.submit(ref,"forward",0,"no-context",operation,error));
    assert(bus.frames.empty() && !service()->worker && error=="CONTROL_CONTEXT_UNCONFIRMED");
    reset();ref=motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"forward",0,"old-handler",operation,error));
    ++bus.instanceId;runWorker();
    assert(bus.frames.empty() && motors.operation(operation,result.to<JsonObject>()));
    assert(result["success"]==false && result["terminalAcknowledged"]==true && result["pending"]==false);
    reset();ref=motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"forward",0,"caller-timeout",operation,error));
    for(const auto& c:service()->commands) if(c.id==operation) {
        auto& pm=PeripheralManager::getInstance();RecursiveMutexGuard lock(pm._mutex);
        pm.finishOrdinaryModbusLocked(c.control,true);
    }
    runWorker();assert(bus.frames.empty() && motors.operation(operation,result.to<JsonObject>()));
    assert(result["success"]==false && result["terminalAcknowledged"]==true);
    reset();ref=motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"forward",0,"allocation-exception",operation,error));
    onLease=[] { throw std::bad_alloc(); };runWorker();
    assert(bus.frames.empty() && motors.operation(operation,result.to<JsonObject>()));
    assert(result["error"]=="CONTROL_MOTOR_WORK_EXCEPTION" && result["terminalAcknowledged"]==true);
    reset();ref=motors.referenceForSlave(3);
    assert(submitOutput(motors,ref,"forward",0,"actor-replay",operation,error));runWorker();
    FastBee::OrdinaryControlContext otherActor;auto& pm=PeripheralManager::getInstance();
    otherActor.configEpoch=FastBee::SensorConfigEpoch::current();
    otherActor.request.actor={FastBee::OrdinaryControl::Source::Http,42};
    otherActor.request.operation=++pm._nextOrdinaryControlOperation;otherActor.request.holdMs=30000;
    uint32_t otherOperation=0;
    assert(!motors.submit(ref,"forward",0,"actor-replay",otherOperation,error,&otherActor) && !otherOperation);
    assert(bus.frames.size()==1);
    reset();ref=motors.referenceForSlave(3);
    bus.readSpeed=0;bus.readPulse=0;bus.parametersFollowWrites=true;bus.failRegister=bus.dev.motorRegs[0];nowMs+=16000;
    assert(submitOutput(motors,ref,"forward",0,"prefix-history",operation,error));runWorker();
    assert(motors.operation(operation,result.to<JsonObject>()) && result["success"]==false);
    assert(result["outputSteps"].size()==3);
    assert(result["outputSteps"][0]["commandAcknowledged"]==true && result["outputSteps"][1]["commandAcknowledged"]==true);
    assert(result["outputSteps"][2]["attempted"]==true && result["outputSteps"][2]["commandAcknowledged"]==false);
    assert(result["physicalOutputVerified"]==false && result["terminalAcknowledged"]==true);
    // The first explicit click has no prior status-poll cache. Preserve the
    // refreshed pulse in the runtime bus after preparing and sending START.
    for (const char* action : {"forward", "reverse"}) for (bool zeros : {false, true}) {
        reset(); ref=motors.referenceForSlave(3);
        service()->states={}; bus.frames.clear();
        bus.dev.motorLastPulse=0; bus.readSpeed=zeros ? 0 : 41;
        bus.readPulse=zeros ? 0 : 73; bus.parametersFollowWrites=true;
        assert(submitOutput(motors,ref,action,0,"first-uncached-click",operation,error)); runWorker();
        assert(motors.operation(operation,result.to<JsonObject>()) && result["success"]==true);
        unsigned starts=0,reads=0;
        for(const auto& frame:bus.frames) {
            assert(frame.retries==0);
            if(!frame.write) ++reads;
            if(frame.write && (frame.reg==bus.dev.motorRegs[0] || frame.reg==bus.dev.motorRegs[1])) ++starts;
        }
        assert(starts==1 && reads==(zeros ? 4U : 2U));
        assert(bus.dev.motorLastPulse==(zeros ? 100 : 73));
        assert(motors.state(ref,state) && state.known && state.pulse==(zeros ? 100 : 73));
    }

    // Both production worker admission and between-step source checks preserve
    // CONTROL_BUSY instead of pretending the live reservation was retired.
    for(bool duringExecute : {false,true}) {
        reset();ref=motors.referenceForSlave(3);
        assert(submitOutput(motors,ref,"forward",0,"pm-validation-busy",operation,error));
        if(duringExecute)onLease=[] {failWorkerValidation=true;};
        else failWorkerValidation=true;
        runWorker();failWorkerValidation=false;
        assert(motors.operation(operation,result.to<JsonObject>()));
        assert(result["error"]=="CONTROL_BUSY" && result["success"]==false);
        assert(result["terminalAcknowledged"]==true && result["pending"]==false);
        assert(bus.frames.empty());
    }
    std::puts("Production motor service + PM passed: source work reservations, queued/running stop, terminal identity and accepted prefix.");

}
