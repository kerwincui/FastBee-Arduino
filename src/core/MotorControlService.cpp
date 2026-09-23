#include "core/MotorControlService.h"
#include "core/MotorCommandQueue.h"
#if FASTBEE_ENABLE_MODBUS && defined(ARDUINO_ARCH_ESP32)
#include "core/FastBeeFramework.h"
#include "core/PeriphExecManager.h"
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#include "protocols/MQTTClient.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <freertos/task.h>

namespace {
using MotorCommands::Phase;
using MotorCommands::Command;
struct MotorState { char ref[48]{}; MotorControlState value; };
struct Service : MotorCommands::Queue {
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    TaskHandle_t worker = nullptr;
    // Normal requests retain deduplication results; eight independent stop
    // slots remain available even when that cache is full.
    std::array<MotorState, 8> states{};
};
Service& service() { static Service s; return s; }
struct Lock {
    bool held;
    explicit Lock(TickType_t timeout = pdMS_TO_TICKS(100)) : held(service().mutex && xSemaphoreTake(service().mutex, timeout) == pdTRUE) {}
    ~Lock() { if (held) xSemaphoreGive(service().mutex); }
};
ProtocolManager::ModbusHandlerLease lease() {
    auto* fw = FastBeeFramework::getInstance();
    auto* pm = fw ? fw->getProtocolManager() : nullptr;
    return pm ? pm->acquireModbusHandler(pdMS_TO_TICKS(100)) : ProtocolManager::ModbusHandlerLease{};
}
String reference(const ModbusSubDevice& dev) {
    uint32_t hash = 2166136261UL;
    auto add = [&](const uint8_t* bytes, size_t count) { while (count--) hash = (hash ^ *bytes++) * 16777619UL; };
    add(reinterpret_cast<const uint8_t*>(dev.sensorId), strlen(dev.sensorId));
    add(reinterpret_cast<const uint8_t*>(dev.motorRegs), sizeof(dev.motorRegs));
    add(&dev.motorDecimals, sizeof(dev.motorDecimals));
    char text[48]; snprintf(text, sizeof(text), "motor.%u.%08lx", dev.slaveAddress, static_cast<unsigned long>(hash));
    return String(text);
}
int findMotor(ModbusHandler& bus, const String& ref) {
    if (bus.getMode() != MODBUS_MASTER) return -1;
    for (uint8_t i = 0; i < bus.getSubDeviceCount(); ++i) {
        const auto& dev = bus.getSubDevice(i);
        if (dev.enabled && !strcmp(dev.deviceType, "motor") && reference(dev) == ref) return i;
    }
    return -1;
}
bool transactionTimeout(Command& cmd, uint16_t& timeout) {
    timeout = cmd.timeoutMs;
    if (!cmd.deadlineMs) return true;
    const int32_t remaining = static_cast<int32_t>(cmd.deadlineMs - millis());
    // Reserve the Modbus driver's bounded 2 s bus-acquire wait before its
    // single response attempt. Never start a queued movement after expiry.
    if (remaining <= 2100) { strlcpy(cmd.error, "Motor command deadline exceeded", sizeof(cmd.error)); return false; }
    timeout = static_cast<uint16_t>(std::min<int32_t>(timeout ? timeout : 1000, remaining - 2000));
    return true;
}
void execute(Command& cmd) {
    auto owner = lease();
    auto* bus = owner.get();
    const int index = bus ? findMotor(*bus, String(cmd.ref)) : -1;
    if (index < 0) { strlcpy(cmd.error, "Motor unavailable or configuration changed", sizeof(cmd.error)); return; }
    const auto dev = bus->getSubDevice(static_cast<uint8_t>(index));
    uint16_t timeout = 0;
    if (!transactionTimeout(cmd, timeout)) return;
    const bool forward = !strcmp(cmd.action, "forward"), reverse = !strcmp(cmd.action, "reverse");
    const bool move = forward || reverse;
    uint16_t reg = 0, value = static_cast<uint16_t>(cmd.value);
    if (!strcmp(cmd.action, "readStatus")) {
        auto speed = bus->readRegistersOnce(dev.slaveAddress, 3, dev.motorRegs[3], 1, true, timeout, 0);
        if (!transactionTimeout(cmd, timeout)) return;
        auto pulse = bus->readRegistersOnce(dev.slaveAddress, 3, dev.motorRegs[4], 1, true, timeout, 0);
        cmd.success = speed.error == ONESHOT_SUCCESS && pulse.error == ONESHOT_SUCCESS;
        cmd.known = cmd.success;
        if (cmd.success) { cmd.speed = speed.data[0]; cmd.pulse = pulse.data[0]; }
        else strlcpy(cmd.error, "Motor status unavailable", sizeof(cmd.error));
        return;
    }
    if (move) { reg = dev.motorRegs[forward ? 0 : 1]; value = cmd.legacyValue ? value : 1; }
    else if (!strcmp(cmd.action, "stop")) { reg = dev.motorRegs[2]; value = cmd.legacyValue ? value : 1; }
    else reg = dev.motorRegs[!strcmp(cmd.action, "setSpeed") ? 3 : 4];
    uint16_t pulse = dev.motorLastPulse;
    int32_t nextPosition = dev.motorCurrentPosition;
    if (move && dev.hasMotorSoftLimit()) {
        const int64_t room = forward ? static_cast<int64_t>(dev.motorMaxPosition) - dev.motorCurrentPosition
                                     : static_cast<int64_t>(dev.motorCurrentPosition) - dev.motorMinPosition;
        int32_t desired = cmd.legacyValue && cmd.value > 1 ? cmd.value : pulse ? pulse : dev.motorMoveStep;
        if (room <= 0 || desired <= 0) { strlcpy(cmd.error, "Motor soft limit or pulse not configured", sizeof(cmd.error)); return; }
        pulse = static_cast<uint16_t>(std::min<int64_t>(65535, std::min<int64_t>(desired, room)));
        auto result = bus->writeRegisterOnce(dev.slaveAddress, dev.motorRegs[4], pulse, true, timeout, 0);
        if (result.error != ONESHOT_SUCCESS) { strlcpy(cmd.error, "Pulse setting not confirmed; motor not started", sizeof(cmd.error)); return; }
        nextPosition += forward ? pulse : -static_cast<int32_t>(pulse);
        value = 1;
    }
    // A lost reply is ambiguous for edge-triggered motor registers. Never
    // inherit the master's automatic retry policy for these commands.
    if (!transactionTimeout(cmd, timeout)) return;
    auto result = bus->writeRegisterOnce(dev.slaveAddress, reg, value, true, timeout, 0);
    cmd.success = result.error == ONESHOT_SUCCESS;
    if (!cmd.success) {
        strlcpy(cmd.error, "Command result unconfirmed; no automatic retry", sizeof(cmd.error));
        return;
    }
    if (!strcmp(cmd.action, "setPulse")) pulse = value;
    bus->updateMotorRuntime(static_cast<uint8_t>(index), nextPosition, pulse);
#if FASTBEE_ENABLE_MQTT
    auto* framework = FastBeeFramework::getInstance();
    auto* protocols = framework ? framework->getProtocolManager() : nullptr;
    auto* mqtt = protocols ? protocols->getMQTTClient() : nullptr;
    if (mqtt) {
        const char* key = forward ? "fwd" : reverse ? "rev" : !strcmp(cmd.action, "setSpeed") ? "spd" : !strcmp(cmd.action, "setPulse") ? "pls" : "stop";
        JsonDocument report;
        auto item = report.to<JsonArray>().add<JsonObject>();
        item["id"] = bus->buildSensorId(static_cast<uint8_t>(index), 0);
        item["value"] = String(key) + ":" + String(value);
        item["remark"] = "command acknowledged";
        String payload; serializeJson(report, payload);
        mqtt->queueReportData(payload);
    }
#endif
#if FASTBEE_ENABLE_PERIPH_EXEC
    // This is a result event, not a second request. The event matcher blocks
    // same-motor motion rules while permitting unrelated automation.
    JsonDocument event;
    event["d"] = index; event["a"] = cmd.action;
    event["origin"] = "motor-result"; event["operationId"] = cmd.id;
    String payload; serializeJson(event, payload);
    PeriphExecManager::getInstance().triggerEventById("mc:" + String(index), payload);
#endif
}
void worker(void*) {
    uint32_t lastWork = millis();
    while (true) {
        int selected = -1; Command cmd; bool retire = false;
        {
            Lock lock(portMAX_DELAY);
            if (lock.held) {
                selected = service().selectNext();
                if (selected >= 0) { service().commands[selected].phase = Phase::RUNNING; cmd = service().commands[selected]; }
                else if (millis() - lastWork >= 60000U) { service().worker = nullptr; retire = true; }
            }
        }
        if (selected < 0) {
            if (retire) { vTaskDelete(nullptr); return; }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); continue;
        }
        execute(cmd);
        lastWork = millis();
        cmd.phase = Phase::DONE; cmd.finishedMs = millis();
        {
            Lock lock(portMAX_DELAY);
            if (lock.held) {
                service().commands[selected] = cmd;
                for (auto& state : service().states) if (!strcmp(state.ref, cmd.ref)) {
                    if (!strcmp(cmd.action, "readStatus")) {
                        state.value.known = cmd.known;
                        if (cmd.known) { state.value.speed = cmd.speed; state.value.pulse = cmd.pulse; state.value.updatedMs = cmd.finishedMs; }
                    } else {
                        strlcpy(state.value.lastAction, cmd.action, sizeof(state.value.lastAction));
                        // Parameter acknowledgement does not refresh a full readback.
                        if (cmd.success && !strcmp(cmd.action, "setSpeed")) state.value.speed = static_cast<uint16_t>(cmd.value);
                        if (cmd.success && !strcmp(cmd.action, "setPulse")) state.value.pulse = static_cast<uint16_t>(cmd.value);
                    }
                    strlcpy(state.value.error, cmd.error, sizeof(state.value.error));
                }
            }
        }
    }
}
}

MotorControlService& MotorControlService::instance() { static MotorControlService s; return s; }
String MotorControlService::referenceForSlave(uint8_t slave) {
    auto owner = lease(); auto* bus = owner.get();
    if (!bus || bus->getMode() != MODBUS_MASTER) return String();
    for (uint8_t i = 0; i < bus->getSubDeviceCount(); ++i) {
        const auto& dev = bus->getSubDevice(i);
        if (dev.enabled && dev.slaveAddress == slave && !strcmp(dev.deviceType, "motor")) return reference(dev);
    }
    return String();
}
bool MotorControlService::validReference(const String& ref) {
    auto owner = lease(); return owner.get() && findMotor(*owner.get(), ref) >= 0;
}
void MotorControlService::appendChoices(JsonArray out) {
    auto owner = lease(); auto* bus = owner.get();
    if (!bus || bus->getMode() != MODBUS_MASTER) return;
    for (uint8_t i = 0; i < bus->getSubDeviceCount(); ++i) {
        const auto& dev = bus->getSubDevice(i);
        if (!dev.enabled || strcmp(dev.deviceType, "motor")) continue;
        JsonObject item = out.add<JsonObject>();
        item["id"] = reference(dev); item["controlRef"] = item["id"];
        item["label"] = dev.name; item["kind"] = "control"; item["type"] = "motor";
        item["group"] = "motor"; item["available"] = true; item["slaveAddress"] = dev.slaveAddress;
        item["rangeMin"] = 0; item["rangeMax"] = 65535;
        item["precision"] = dev.motorDecimals; item["motionFeedback"] = false;
    }
}
bool MotorControlService::submit(const String& ref, const String& action, int value,
        const String& requestId, uint32_t& operationId, String& error) {
    return submitImpl(ref, action, value, requestId, 0, 0, false, operationId, error);
}
bool MotorControlService::submitRule(const String& ref, const String& action, int value,
        const String& requestId, uint32_t budgetMs, uint16_t timeoutMs, uint32_t& operationId, String& error) {
    if (budgetMs <= 2100) { error = "Motor command deadline exceeded"; return false; }
    return submitImpl(ref, action, value, requestId, std::min<uint32_t>(budgetMs, 30000),
                      std::max<uint16_t>(100, std::min<uint16_t>(timeoutMs, 5000)), true, operationId, error);
}
bool MotorControlService::submitImpl(const String& ref, const String& action, int value,
        const String& requestId, uint32_t budgetMs, uint16_t timeoutMs, bool legacyValue,
        uint32_t& operationId, String& error) {
    operationId = 0;
    const bool read = action == "readStatus", stop = action == "stop";
    if ((!read && !stop && action != "forward" && action != "reverse" && action != "setSpeed" && action != "setPulse") ||
        value < 0 || value > 65535 || ref.length() >= 48 || requestId.length() >= 64 ||
        (!read && requestId.isEmpty())) { error = "Invalid motor command or request ID"; return false; }
    // The scale belongs to the validated device identity, never the layout's
    // free-form display precision. Cache it even before the first readback.
    uint8_t precision = 0;
    {
        auto owner = lease(); auto* bus = owner.get();
        const int index = bus ? findMotor(*bus, ref) : -1;
        if (index < 0) { error = "Motor unavailable or configuration changed"; return false; }
        precision = std::min<uint8_t>(bus->getSubDevice(static_cast<uint8_t>(index)).motorDecimals, 6);
    }
    Lock lock;
    if (!lock.held) { error = "Motor service busy"; return false; }
    auto& s = service();
    int slot = -1; const char* queueError = nullptr;
    if (!s.reserve(ref.c_str(), action.c_str(), value, requestId.c_str(), millis(), slot, operationId, queueError)) {
        error = queueError; return false;
    }
    if (operationId) return true;
    MotorState* state = nullptr;
    for (auto& item : s.states) if (ref == item.ref) { state = &item; break; }
    if (!state) for (auto& item : s.states) if (!item.ref[0]) { state = &item; strlcpy(item.ref, ref.c_str(), sizeof(item.ref)); break; }
    if (!state) for (auto& item : s.states) {
        bool busy = false;
        for (const auto& c : s.commands) if (!strcmp(c.ref, item.ref) && c.phase != Phase::EMPTY && c.phase != Phase::DONE) busy = true;
        if (!busy && millis() - item.value.updatedMs >= 60000U) { item = MotorState{}; state = &item; strlcpy(item.ref, ref.c_str(), sizeof(item.ref)); break; }
    }
    // An unavailable optional display cache must never veto a stop request.
    if (!state && !stop) { error = "Motor state capacity reached"; return false; }
    if (state) state->value.precision = precision;
    if (!s.worker && xTaskCreate(worker, "motor_control", 6144, nullptr, 1, &s.worker) != pdPASS) {
        s.worker = nullptr; error = "Motor worker unavailable"; return false;
    }
    if (stop) s.cancelQueued(ref.c_str(), millis());
    auto& c = s.commands[slot]; c = Command{};
    c.id = s.allocateId(); c.order = ++s.nextOrder;
    c.deadlineMs = budgetMs ? millis() + budgetMs : 0;
    if (budgetMs && !c.deadlineMs) c.deadlineMs = 1;
    c.timeoutMs = timeoutMs; c.legacyValue = legacyValue;
    strlcpy(c.ref, ref.c_str(), sizeof(c.ref)); strlcpy(c.action, action.c_str(), sizeof(c.action));
    strlcpy(c.requestId, requestId.c_str(), sizeof(c.requestId)); c.value = value; c.phase = Phase::QUEUED;
    operationId = c.id; xTaskNotifyGive(s.worker); return true;
}
bool MotorControlService::operation(uint32_t id, JsonObject out) {
    Lock lock; if (!lock.held) return false;
    for (const auto& c : service().commands) if (c.id == id && c.phase != Phase::EMPTY) {
        out["operationId"] = id; out["pending"] = c.phase != Phase::DONE;
        out["success"] = c.phase == Phase::DONE && c.success; out["action"] = c.action;
        out["error"] = c.error; out["motionKnown"] = false;
        if (c.known) { out["speed"] = c.speed; out["pulse"] = c.pulse; }
        return true;
    }
    return false;
}
bool MotorControlService::state(const String& ref, MotorControlState& out) {
    Lock lock; if (!lock.held) return false;
    for (const auto& item : service().states) if (ref == item.ref) {
        out = item.value; out.pending = false;
        if (millis() - out.updatedMs > 15000U) out.known = false;
        for (const auto& cmd : service().commands) if (ref == cmd.ref && (cmd.phase == Phase::QUEUED || cmd.phase == Phase::RUNNING)) out.pending = true;
        return true;
    }
    return false;
}
#endif
