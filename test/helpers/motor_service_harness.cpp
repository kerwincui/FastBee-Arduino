#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/MotorControlService.h"
#include "core/MotorCommandQueue.h"
#include "protocols/MotorProtocol.h"
#include <array>
#include <cassert>
#include <vector>
#include <functional>
#include <new>
#include <cstdlib>

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
static bool hasPsram = true, failPsram = false, failAlloc = false;
static std::vector<unsigned> allocations;
static bool psramFound() { return hasPsram; }
static void* heap_caps_malloc(size_t bytes, unsigned caps) {
    allocations.push_back(caps);
    if (failAlloc || (failPsram && (caps & MALLOC_CAP_SPIRAM))) return nullptr;
    return malloc(bytes);
}

using TickType_t = uint32_t;
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1, pdPASS = 1;
#define pdMS_TO_TICKS(v) (v)
static uint32_t nowMs = 100;
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
    bool enabled = true, limited = false;
    uint16_t motorLastPulse = 50;
    int32_t motorCurrentPosition = 0, motorMoveStep = 5, motorMaxPosition = 100, motorMinPosition = -100;
    bool hasMotorSoftLimit() const { return limited; }
};
constexpr int MODBUS_MASTER = 1, ONESHOT_SUCCESS = 0, ONESHOT_TIMEOUT = 1, ONESHOT_CRC_ERROR = 2,
    ONESHOT_EXCEPTION = 3, ONESHOT_NOT_INITIALIZED = 4, ONESHOT_BUSY = 5;
struct OneShotResult { int error = 0; uint16_t data[2] = {23, 0}; uint16_t count = 1; uint8_t exceptionCode = 0; };
struct Frame { uint16_t reg, value; uint8_t retries; bool write; uint8_t fc = 6; };
static std::function<void()> onWrite;
static std::function<void()> onRead;
struct ModbusHandler {
    ModbusSubDevice dev;
    std::vector<Frame> frames;
    int failRegister = -1;
    int failureCode = ONESHOT_TIMEOUT;
    uint16_t resultCount = 1;
    uint16_t readSpeed = 23, readPulse = 23;
    uint32_t latency = 0;
    int getMode() { return MODBUS_MASTER; }
    uint8_t getSubDeviceCount() { return 1; }
    const ModbusSubDevice& getSubDevice(uint8_t) { return dev; }
    OneShotResult readRegistersOnce(uint8_t, uint8_t fc, uint16_t reg, uint16_t count, bool, uint16_t, uint8_t retries) {
        assert(count == 1); frames.push_back({reg, 0, retries, false, fc}); nowMs += latency;
        if (onRead) { auto callback = std::move(onRead); onRead = {}; callback(); }
        OneShotResult result; result.error = reg == failRegister ? failureCode : 0;
        result.count = resultCount; result.exceptionCode = result.error == ONESHOT_EXCEPTION ? 2 : 0;
        result.data[0] = reg == dev.motorProtocol.readRegister(dev.motorRegs, false) ? readSpeed : readPulse; return result;
    }
    OneShotResult writeRegisterOnce(uint8_t, uint16_t reg, uint16_t value, bool, uint16_t, uint8_t retries) {
        frames.push_back({reg, value, retries, true}); nowMs += latency;
        if (onWrite) { auto callback = std::move(onWrite); onWrite = {}; callback(); }
        OneShotResult result; result.error = reg == failRegister ? 1 : 0; return result;
    }
    OneShotResult writeMultipleRegistersOnce(uint8_t slave, uint16_t reg, uint16_t count,
                                              const uint16_t* values, bool control, uint16_t timeout, uint8_t retries) {
        assert(count == 1 && control && retries == 0);
        auto result = writeRegisterOnce(slave, reg, *values, control, timeout, retries);
        frames.back().fc = 16; return result;
    }
    void updateMotorRuntime(uint8_t, int32_t pos, uint16_t pulse) { dev.motorCurrentPosition = pos; dev.motorLastPulse = pulse; }
    String buildSensorId(uint8_t, uint8_t) { return "motor_sensor"; }
};
static ModbusHandler bus;
struct MQTTClient {
    std::vector<String> reports;
    void queueReportData(const String& report) { reports.push_back(report); }
};
static MQTTClient mqtt;
struct ProtocolManager {
    struct ModbusHandlerLease { ModbusHandler* get() { return &bus; } };
    ModbusHandlerLease acquireModbusHandler(uint32_t) { return {}; }
    MQTTClient* getMQTTClient() { return &mqtt; }
};
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { static FastBeeFramework value; return &value; }
    ProtocolManager* getProtocolManager() { static ProtocolManager value; return &value; }
};
struct PeriphExecManager {
    std::vector<String> events;
    static PeriphExecManager& getInstance() { static PeriphExecManager value; return value; }
    void triggerEventById(const String&, const String& event) { events.push_back(event); }
};
#include "motor_service_production.inc"
static void runWorker() { try { worker(nullptr); } catch (const Yield&) {} }
static void reset() {
    service()->commands = {}; service()->states = {}; service()->worker = nullptr;
    bus = {}; mqtt.reports.clear(); PeriphExecManager::getInstance().events.clear();
    failWorker = failLock = false; nowMs = 100; onWrite = {}; onRead = {};
    uint32_t operation = 0; String error;
    auto& motors = MotorControlService::instance();
    assert(motors.submit(motors.referenceForSlave(3), "readStatus", 0, "", operation, error));
    runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    bus.frames.clear(); service()->commands = {}; service()->worker = nullptr;
}
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "psram";
    hasPsram = mode != "internal";
    failPsram = mode == "fallback";
    failAlloc = mode == "failure";
    auto& motors = MotorControlService::instance();
    uint32_t operation = 0; String error;
    if (failAlloc) {
        assert(!motors.submit(motors.referenceForSlave(3), "forward", 0, "no-memory", operation, error));
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
    assert(motors.submit(ref, "forward", 0, "forward", operation, error));
    const uint32_t original = operation;
    assert(motors.submit(ref, "forward", 0, "forward", operation, error) && operation == original);
    runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 1 && bus.frames[0].retries == 0);
    JsonDocument result;
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == true && result["motionKnown"] == false);
    assert(mqtt.reports.size() == 1 && PeriphExecManager::getInstance().events.size() == 1);
    assert(PeriphExecManager::getInstance().events[0].indexOf("motor-result") >= 0);
    // Replay after completion returns the original result without another
    // physical start-register write or another event/MQTT publication.
    assert(motors.submit(ref,"forward",0,"forward",operation,error) && operation==original);
    runWorker(); assert(bus.frames.size()==1 && mqtt.reports.size()==1);
    // A duplicate arriving while the actual Modbus write is in progress is
    // deduplicated too; count frames at the bus boundary, not in a fake queue.
    reset(); ref=motors.referenceForSlave(3);
    assert(motors.submit(ref,"reverse",0,"running-reverse",operation,error));
    const uint32_t running=operation;
    onWrite=[&](){uint32_t duplicate=0;String detail;
        assert(motors.submit(ref,"reverse",0,"running-reverse",duplicate,detail) && duplicate==running);};
    runWorker(); assert(bus.frames.size()==1 && bus.frames[0].reg==2 && bus.frames[0].retries==0);
    assert(mqtt.reports.size()==1 && PeriphExecManager::getInstance().events.size()==1);

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    bus.dev.motorCurrentPosition = 90;
    assert(motors.submit(ref, "forward", 0, "limited", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 0 && bus.frames[0].value == 10);
    assert(bus.frames[1].reg == 1 && bus.dev.motorCurrentPosition == 100);
    assert(motors.submit(ref, "forward", 0, "at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 2);

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    bus.dev.motorCurrentPosition = -90;
    assert(motors.submit(ref, "reverse", 0, "reverse-limited", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 0 && bus.frames[0].value == 10);
    assert(bus.frames[1].reg == 2 && bus.frames[1].value == 1 && bus.dev.motorCurrentPosition == -100);
    assert(motors.submit(ref, "reverse", 0, "reverse-at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 2);
    assert(motors.submit(ref, "stop", 0, "stop-at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 3 && bus.frames[2].reg == 3);

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true; bus.failRegister = 0;
    assert(motors.submit(ref, "forward", 0, "failed-pulse", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 0);
    assert(mqtt.reports.empty());

    reset(); ref = motors.referenceForSlave(3); bus.failRegister = 1;
    assert(motors.submit(ref, "forward", 0, "lost-reply", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].retries == 0);
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == false);
    const uint32_t uncertain=operation;
    assert(motors.submit(ref,"forward",0,"lost-reply",operation,error) && operation==uncertain);
    runWorker(); assert(bus.frames.size()==1);
    assert(motors.operation(operation,result.to<JsonObject>()) && result["success"]==false);
    assert(mqtt.reports.empty() && PeriphExecManager::getInstance().events.empty());

    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "readStatus", 0, "read", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    MotorControlState state; assert(motors.state(ref, state) && state.known && state.speed == 23 && state.precision == 1);
    nowMs += 15001; assert(motors.state(ref, state) && !state.known);

    // Queued polling does not disable controls. A real control is accepted
    // and reaches the bus before the lower-priority background read.
    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "readStatus", 0, "poll", operation, error));
    const uint32_t queuedRead = operation;
    assert(motors.state(ref, state) && !state.pending);
    assert(motors.operation(queuedRead, result.to<JsonObject>()) && result["pending"] == true);
    assert(motors.submit(ref, "setSpeed", 42, "user-speed", operation, error));
    assert(motors.state(ref, state) && state.pending);
    assert(!motors.submit(ref, "forward", 0, "second-control", operation, error));
    runWorker();
    assert(bus.frames.size() == 3 && bus.frames[0].write && bus.frames[0].reg == 5 && bus.frames[0].value == 42);
    assert(!bus.frames[1].write && !bus.frames[2].write);
    assert(motors.state(ref, state) && !state.pending);

    // Reproduce a disconnected/sluggish motor while its read is RUNNING.
    // The touch command queues once, stays visibly pending until completed,
    // and readStatus's public operation status remains accurate.
    reset(); ref = motors.referenceForSlave(3); bus.failRegister = 5; bus.latency = 1000;
    assert(motors.submit(ref, "readStatus", 0, "slow-poll", operation, error));
    const uint32_t runningRead = operation;
    uint32_t clicked = 0;
    onRead = [&]() {
        assert(motors.state(ref, state) && !state.pending);
        assert(motors.operation(runningRead, result.to<JsonObject>()) && result["pending"] == true);
        assert(motors.submit(ref, "forward", 0, "touch-during-poll", clicked, error));
        assert(motors.state(ref, state) && state.pending);
        assert(!motors.submit(ref, "reverse", 0, "double-touch", operation, error));
    };
    runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    assert(motors.operation(clicked, result.to<JsonObject>()) && result["success"] == false);
    assert(String(result["error"].as<const char*>()).indexOf("unconfirmed") >= 0);
    assert(motors.state(ref, state) && !state.pending && !state.known);

    // Stop remains available during a read and cancels a queued movement.
    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "readStatus", 0, "stop-poll", operation, error));
    uint32_t cancelled = 0;
    onRead = [&]() {
        assert(motors.submit(ref, "reverse", 0, "queued-motion", cancelled, error));
        assert(motors.submit(ref, "stop", 0, "urgent-stop", operation, error));
    };
    runWorker();
    assert(bus.frames.size() == 2 && bus.frames[1].write && bus.frames[1].reg == 3);
    assert(motors.operation(cancelled, result.to<JsonObject>()) && result["pending"] == false && result["success"] == false);
    assert(motors.state(ref, state) && !state.pending);

    // Readback failures do not replace the stop acknowledgement or its ID.
    const uint32_t stopped = operation;
    bus.failRegister = 5;
    assert(motors.submit(ref, "readStatus", 0, "after-stop", operation, error)); runWorker();
    assert(motors.state(ref, state) && !state.known && state.readError[0]);
    assert(state.operationId == stopped && state.commandKnown && state.commandSuccess);
    assert(!strcmp(state.lastAction, "stop") && !state.error[0]);
    assert(motors.submit(ref, "reverse", 0, "unreadable-start", operation, error));
    bus.frames.clear(); runWorker(); assert(bus.frames.empty());
    assert(motors.state(ref, state) && state.commandKnown && !state.commandSuccess);
    assert(!strcmp(state.lastAction, "reverse") && state.error[0]);
    const uint32_t failedStart = operation; bus.failRegister = -1;
    assert(motors.submit(ref, "readStatus", 0, "recovered-read", operation, error)); runWorker();
    assert(motors.state(ref, state) && state.known && !state.readError[0]);
    assert(state.operationId == failedStart && state.commandKnown && !state.commandSuccess && state.error[0]);

    reset(); ref = motors.referenceForSlave(3); bus.readPulse = 0;
    assert(motors.submit(ref, "readStatus", 0, "read-zero-pulse", operation, error)); runWorker();
    assert(motors.state(ref, state) && state.known && state.pulse == 0); bus.frames.clear();
    assert(motors.submit(ref, "forward", 0, "zero-pulse-forward", operation, error)); runWorker();
    assert(motors.submit(ref, "reverse", 0, "zero-pulse-reverse", operation, error)); runWorker();
    assert(bus.frames.empty());
    assert(motors.submit(ref, "stop", 0, "zero-pulse-stop", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 3);

    // Stop arriving during a pulse prewrite must prevent the later start.
    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    assert(motors.submit(ref, "forward", 0, "preparing-move", operation, error));
    const uint32_t preparing = operation;
    onWrite = [&]() { assert(motors.submit(ref, "stop", 0, "interrupt-prewrite", operation, error)); };
    runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 0 && bus.frames[1].reg == 3);
    assert(motors.operation(preparing, result.to<JsonObject>()) && result["success"] == false);
    assert(motors.state(ref, state) && state.commandKnown && state.commandSuccess && state.operationId == operation);

    reset(); ref = motors.referenceForSlave(3); nowMs += 15001;
    assert(motors.submit(ref, "forward", 0, "stale-parameters", operation, error));
    runWorker(); assert(bus.frames.empty());
    assert(motors.submit(ref, "stop", 0, "stop-without-feedback", operation, error));
    runWorker(); assert(bus.frames.size() == 1 && bus.frames[0].reg == 3);

    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "setPulse", 0, "zero-pulse", operation, error)); runWorker();
    bus.frames.clear();
    assert(motors.submit(ref, "forward", 0, "unverified-pulse", operation, error));
    runWorker(); assert(bus.frames.empty());

    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "forward", 0, "launch", operation, error));
    assert(motors.submit(ref, "stop", 0, "stop", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 3);

    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "forward", 0, "rebind", operation, error));
    bus.dev.motorRegs[0] = 55; runWorker(); assert(bus.frames.empty());

    reset(); ref = motors.referenceForSlave(3); failWorker = true;
    assert(!motors.submit(ref, "forward", 0, "no-worker", operation, error));
    failWorker = false; failLock = true;
    assert(!motors.submit(ref, "forward", 0, "no-lock", operation, error));

    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submitRule(ref, "forward", 7, "rule", 5000, 1000, operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].value == 7);
    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    assert(motors.submitRule(ref, "forward", 7, "rule-limited", 5000, 1000, operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].value == 7 && bus.frames[1].value == 1);
    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submitRule(ref, "forward", 7, "expired-rule", 5000, 1000, operation, error));
    nowMs += 5000; runWorker(); assert(bus.frames.empty());
    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true; bus.latency = 3000;
    assert(motors.submitRule(ref, "forward", 7, "expired-between-writes", 5000, 1000, operation, error));
    runWorker(); assert(bus.frames.size() == 1 && bus.frames[0].reg == 0);
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
    assert(motors.submit(ref, "readStatus", 0, "generic-read", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 200 && bus.frames[1].reg == 201);
    assert(bus.frames[0].fc == 4 && bus.frames[1].fc == 4);
    bus.frames.clear();
    assert(motors.submit(ref, "forward", 0, "generic-forward", operation, error)); runWorker();
    assert(motors.submit(ref, "reverse", 0, "generic-reverse", operation, error)); runWorker();
    assert(motors.submit(ref, "stop", 0, "generic-stop", operation, error)); runWorker();
    assert(bus.frames.size() == 3 && bus.frames[0].value == 17 && bus.frames[1].value == 34 && bus.frames[2].value == 0);
    for (const auto& frame : bus.frames) assert(frame.reg == 100 && frame.fc == 16 && frame.retries == 0);
    // Rules must also use the profile's direction value, never legacy raw 7.
    assert(motors.submitRule(ref, "reverse", 7, "generic-rule", 5000, 1000, operation, error)); runWorker();
    assert(bus.frames.back().value == 34);
    assert(motors.submit(ref, "setSpeed", 49, "generic-speed", operation, error)); runWorker();
    assert(bus.frames.back().reg == 101 && bus.frames.back().value == 49 && bus.frames.back().fc == 16);
    assert(motors.state(ref, state) && !state.known);
    const size_t written = bus.frames.size();
    assert(motors.submit(ref, "forward", 0, "generic-dirty", operation, error)); runWorker();
    assert(bus.frames.size() == written);

    // Every execution-semantic change invalidates queued commands and feedback.
    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "forward", 0, "profile-changed", operation, error));
    bus.dev.motorProtocol.stopValue = 0; runWorker(); assert(bus.frames.empty());
    assert(motors.referenceForSlave(3) != ref);
    reset(); ref = motors.referenceForSlave(3);
    bus.dev.motorProtocol.configured = true;
    bus.dev.motorProtocol.speedReadRegister = 5; bus.dev.motorProtocol.pulseReadRegister = 0;
    assert(motors.referenceForSlave(3) != ref); // Explicit profiles change rule-value semantics too.

    // Unsafe/non-finite capability cannot be bypassed by known parameters.
    reset(); bus.dev.motorProtocol.finitePulseMove = false; ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "readStatus", 0, "nonfinite-read", operation, error)); runWorker(); bus.frames.clear();
    assert(motors.submit(ref, "forward", 0, "nonfinite", operation, error)); runWorker(); assert(bus.frames.empty());
    assert(motors.submit(ref, "stop", 0, "nonfinite-stop", operation, error)); runWorker(); assert(bus.frames.size() == 1);

    for (int reason : {ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR, ONESHOT_EXCEPTION, ONESHOT_NOT_INITIALIZED, ONESHOT_BUSY}) {
        reset(); ref = motors.referenceForSlave(3); bus.failRegister = 5; bus.failureCode = reason;
        assert(motors.submit(ref, "readStatus", 0, "error-detail", operation, error)); runWorker();
        assert(motors.state(ref, state) && !state.known);
        assert(strstr(state.readError, "speed s3 FC03 r5") && strstr(state.readError, "pulse s3 FC03 r0 OK"));
        if (reason == ONESHOT_EXCEPTION) assert(strstr(state.readError, "EXCEPTION(0x02)"));
        bus.frames.clear(); assert(motors.submit(ref, "forward", 0, "error-start", operation, error)); runWorker();
        assert(bus.frames.empty());
    }
    reset(); ref = motors.referenceForSlave(3); bus.resultCount = 0;
    assert(motors.submit(ref, "readStatus", 0, "empty-read", operation, error)); runWorker();
    assert(motors.state(ref, state) && !state.known && strstr(state.readError, "SHORT_REPLY"));
    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "forward", 0, "shared-forward", operation, error)); runWorker();
    assert(motors.state(ref, state) && !strcmp(state.lastAcknowledgedAction, "forward"));
    assert(motors.submit(ref, "setSpeed", 70, "shared-speed", operation, error)); runWorker();
    assert(motors.state(ref, state) && !strcmp(state.lastAcknowledgedAction, "forward"));
    nowMs += 5000;
    assert(motors.submit(ref, "readStatus", 0, "shared-read", operation, error)); runWorker();
    assert(motors.state(ref, state) && state.known && !strcmp(state.lastAcknowledgedAction, "forward"));
    JsonDocument lookup; bool busy = false;
    failLock = true;
    assert(!motors.operation(operation, lookup.to<JsonObject>(), &busy) && busy);
    failLock = false;
    assert(!motors.operation(0, lookup.to<JsonObject>(), &busy) && !busy);
    assert(motors.operation(operation, lookup.to<JsonObject>(), &busy) && !busy);
    assert(lookup["parametersKnown"].as<bool>() && !lookup["motionKnown"].as<bool>());
    bus.failRegister = bus.dev.motorRegs[1];
    assert(motors.submit(ref, "reverse", 0, "failed-reverse", operation, error)); runWorker();
    assert(motors.state(ref, state) && !state.commandSuccess && !strcmp(state.lastAcknowledgedAction, "forward"));
    assert(motors.submit(ref, "stop", 0, "shared-stop", operation, error)); runWorker();
    assert(motors.state(ref, state) && !strcmp(state.lastAcknowledgedAction, "stop"));
    std::puts("Production motor service passed: generic protocol, shared acknowledgements, truthful state, lock contention, stop and guarded dispatch.");
}
