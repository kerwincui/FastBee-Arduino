#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/MotorControlService.h"
#include "core/MotorCommandQueue.h"
#include <array>
#include <cassert>
#include <vector>

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
    uint8_t slaveAddress = 3, motorDecimals = 1;
    bool enabled = true, limited = false;
    uint16_t motorLastPulse = 50;
    int32_t motorCurrentPosition = 0, motorMoveStep = 5, motorMaxPosition = 100, motorMinPosition = -100;
    bool hasMotorSoftLimit() const { return limited; }
};
constexpr int MODBUS_MASTER = 1, ONESHOT_SUCCESS = 0;
struct OneShotResult { int error = 0; uint16_t data[2] = {23, 0}; };
struct Frame { uint16_t reg, value; uint8_t retries; bool write; };
struct ModbusHandler {
    ModbusSubDevice dev;
    std::vector<Frame> frames;
    int failRegister = -1;
    uint32_t latency = 0;
    int getMode() { return MODBUS_MASTER; }
    uint8_t getSubDeviceCount() { return 1; }
    const ModbusSubDevice& getSubDevice(uint8_t) { return dev; }
    OneShotResult readRegistersOnce(uint8_t, uint8_t, uint16_t reg, uint16_t count, bool, uint16_t, uint8_t retries) {
        assert(count == 1); frames.push_back({reg, 0, retries, false}); nowMs += latency;
        OneShotResult result; result.error = reg == failRegister ? 1 : 0; return result;
    }
    OneShotResult writeRegisterOnce(uint8_t, uint16_t reg, uint16_t value, bool, uint16_t, uint8_t retries) {
        frames.push_back({reg, value, retries, true}); nowMs += latency;
        OneShotResult result; result.error = reg == failRegister ? 1 : 0; return result;
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
    service().commands = {}; service().states = {}; service().worker = nullptr;
    bus = {}; mqtt.reports.clear(); PeriphExecManager::getInstance().events.clear();
    failWorker = failLock = false; nowMs = 100;
}
int main() {
    auto& motors = MotorControlService::instance();
    uint32_t operation = 0; String error;
    reset();
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

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true;
    bus.dev.motorCurrentPosition = 90;
    assert(motors.submit(ref, "forward", 0, "limited", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && bus.frames[0].reg == 0 && bus.frames[0].value == 10);
    assert(bus.frames[1].reg == 1 && bus.dev.motorCurrentPosition == 100);
    assert(motors.submit(ref, "forward", 0, "at-limit", operation, error)); runWorker();
    assert(bus.frames.size() == 2);

    reset(); ref = motors.referenceForSlave(3); bus.dev.limited = true; bus.failRegister = 0;
    assert(motors.submit(ref, "forward", 0, "failed-pulse", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].reg == 0);
    assert(mqtt.reports.empty());

    reset(); ref = motors.referenceForSlave(3); bus.failRegister = 1;
    assert(motors.submit(ref, "forward", 0, "lost-reply", operation, error)); runWorker();
    assert(bus.frames.size() == 1 && bus.frames[0].retries == 0);
    assert(motors.operation(operation, result.to<JsonObject>()) && result["success"] == false);

    reset(); ref = motors.referenceForSlave(3);
    assert(motors.submit(ref, "readStatus", 0, "read", operation, error)); runWorker();
    assert(bus.frames.size() == 2 && !bus.frames[0].write && !bus.frames[1].write);
    MotorControlState state; assert(motors.state(ref, state) && state.known && state.speed == 23 && state.precision == 1);
    nowMs += 15001; assert(motors.state(ref, state) && !state.known);

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
    std::puts("Production motor service passed: actual dispatch, readback, stop, limits, lost replies, deadlines, allocation/lock failure, binding and reporting.");
}
