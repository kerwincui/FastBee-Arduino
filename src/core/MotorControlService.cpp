#include "core/MotorControlService.h"
#include "core/MotorCommandQueue.h"
#include "protocols/ModbusMotorReference.h"
#include "core/ReportInstanceBinding.h"
#include "core/ActionReportPayload.h"
#if FASTBEE_ENABLE_MODBUS && defined(ARDUINO_ARCH_ESP32)
#include "core/FastBeeFramework.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#include "protocols/MQTTClient.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/task.h>

namespace {
using MotorCommands::Phase;
using MotorCommands::Command;
using MotorCommands::ReportState;
struct MotorState { char ref[48]{}; MotorControlState value; };
struct Service : MotorCommands::Queue {
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    TaskHandle_t worker = nullptr;
    // Normal requests retain deduplication results; eight independent stop
    // slots remain available even when that cache is full.
    std::array<MotorState, 8> states{};
};
Service* service() {
    // This task-only history cache is neither DMA data nor a task stack.
    // Prefer PSRAM so panel controls do not consume weather's internal reserve.
    static Service* s = []() -> Service* {
        void* storage = nullptr;
#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
        if (psramFound()) storage = heap_caps_malloc(sizeof(Service), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!storage) storage = heap_caps_malloc(sizeof(Service), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        return storage ? new (storage) Service() : nullptr;
    }();
    return s;
}
struct Lock {
    Service* state;
    bool held;
    explicit Lock(TickType_t timeout = pdMS_TO_TICKS(100)) : state(service()),
        held(state && state->mutex && xSemaphoreTake(state->mutex, timeout) == pdTRUE) {}
    ~Lock() { if (held) xSemaphoreGive(state->mutex); }
};
ProtocolManager::ModbusHandlerLease lease() {
    auto* fw = FastBeeFramework::getInstance();
    auto* pm = fw ? fw->getProtocolManager() : nullptr;
    return pm ? pm->acquireModbusHandler(pdMS_TO_TICKS(100)) : ProtocolManager::ModbusHandlerLease{};
}
String reference(const ModbusSubDevice& dev) { return FastBee::motorControlReference(dev); }

int findMotor(ModbusHandler& bus, const String& ref) {
    if (bus.getMode() != MODBUS_MASTER) return -1;
    for (uint8_t i = 0; i < bus.getSubDeviceCount(); ++i) {
        const auto& dev = bus.getSubDevice(i);
        if (dev.enabled && !strcmp(dev.deviceType, "motor") && reference(dev) == ref) return i;
    }
    return -1;
}

bool snapshotMotor(ModbusHandler& bus, const String& ref, ModbusSubDevice& device,
    FastBee::ModbusControlTarget& target) {
    return bus.snapshotOrdinaryMotorReference(ref, device, target);
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
bool cancelledByStop(Command& cmd) {
    Lock lock;
    if (!lock.held) {
        strlcpy(cmd.error, "Motor service busy; command not started", sizeof(cmd.error));
        return true;
    }
    for (const auto& queued : lock.state->commands) {
        if (queued.phase == Phase::QUEUED && !strcmp(queued.ref, cmd.ref) &&
            !strcmp(queued.action, "stop")) {
            strlcpy(cmd.error, "Cancelled by stop", sizeof(cmd.error));
            return true;
        }
    }
    return false;
}
const char* busErrorName(const OneShotResult& result, bool read) {
    switch (result.error) {
        case ONESHOT_SUCCESS: return read && result.count != 1 ? "SHORT_REPLY" : "OK";
        case ONESHOT_TIMEOUT: return "TIMEOUT";
        case ONESHOT_CRC_ERROR: return "CRC_ERROR";
        case ONESHOT_EXCEPTION: return "EXCEPTION";
        case ONESHOT_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case ONESHOT_BUSY: return "BUSY";
        default: return "UNKNOWN_ERROR";
    }
}
void describeResult(char* out, size_t size, const char* action, uint8_t slave,
                    uint8_t fc, uint16_t reg, const OneShotResult& result, bool read) {
    if (result.error == ONESHOT_EXCEPTION)
        snprintf(out, size, "%s s%u FC%02X r%u EXCEPTION(0x%02X)", action, slave, fc, reg, result.exceptionCode);
    else snprintf(out, size, "%s s%u FC%02X r%u %s", action, slave, fc, reg, busErrorName(result, read));
}
OneShotResult writeMotor(ModbusHandler& bus, const ModbusSubDevice& dev, Command& cmd,
                          uint16_t reg, uint16_t value, uint16_t timeout) {
    OneShotResult denied; denied.error = ONESHOT_CONTROL_REJECTED;
    if (cmd.outputStepCount >= Command::maxOutputSteps) {
        strlcpy(cmd.error, "Motor output evidence capacity exceeded", sizeof(cmd.error)); return denied;
    }
    FastBee::OrdinaryControlResult control;
    const auto result = PeripheralManager::getInstance().submitOrdinaryModbusRelay(bus, cmd.target,
        dev.motorProtocol.writeFunction, reg, value, cmd.control, control, timeout, false, cmd.workGeneration);
    auto& step = cmd.outputSteps[cmd.outputStepCount++];
    step.address = reg; step.value = value; step.function = dev.motorProtocol.writeFunction;
    step.error = static_cast<uint8_t>(result.error); step.attempted = result.outputAttempted;
    step.acknowledged = result.error == ONESHOT_SUCCESS;
    return result;
}
#if FASTBEE_ENABLE_MQTT
FastBee::MqttClientLease reportClient() {
    auto* framework = FastBeeFramework::getInstance();
    auto* protocols = framework ? framework->getProtocolManager() : nullptr;
    return protocols ? protocols->acquireMQTTClient() : FastBee::MqttClientLease{};
}
void reportAcknowledgement(Command& cmd, const char* sensorId, const char* key, uint16_t value) {
    cmd.reportState = ReportState::InvalidPayload;
    if (!sensorId || !sensorId[0]) return;
    char reportValue[24];
    const int length = snprintf(reportValue, sizeof(reportValue), "%s:%u", key, static_cast<unsigned>(value));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(reportValue)) return;
    JsonDocument report;
    auto item = report.to<JsonArray>().add<JsonObject>();
    item["id"] = cmd.mqttReportId[0] ? cmd.mqttReportId : sensorId;
    if (cmd.mqttReportId[0]) item["value"] = String(cmd.mqttRequestedValue);
    else item["value"] = reportValue;
    item["remark"] = "command acknowledged";
    if (report.overflowed()) return;
    const size_t bytes = measureJson(report);
    if (!bytes || bytes > FastBee::kActionReportPayloadBytes) return;
    String payload;
    if (!payload.reserve(bytes + 1) || serializeJson(report, payload) != bytes || payload.length() != bytes) return;
    auto mqtt = reportClient();
    const auto admission = FastBee::submitBoundReport(mqtt.get(), cmd.reportInstanceId, payload,
        [](const String& data, uint32_t origin, uint32_t current) {
#if FASTBEE_ENABLE_PERIPH_EXEC
            return PeriphExecManager::getInstance().queuePendingReport(data, origin, current);
#else
            (void)data; (void)origin; (void)current;
            return false;
#endif
        });
    cmd.reportState = admission == FastBee::BoundReportAdmission::Queued ? ReportState::Queued :
        admission == FastBee::BoundReportAdmission::Retained ? ReportState::Retained : ReportState::Dropped;
}
#endif
bool commandSourceCurrent(Command& cmd) {
    if (cmd.control.operationDeadlineAt &&
        static_cast<int32_t>(static_cast<uint32_t>(millis()) - cmd.control.operationDeadlineAt) >= 0) {
        strlcpy(cmd.error, "Motor command deadline exceeded", sizeof(cmd.error)); return false;
    }
    auto& pm = PeripheralManager::getInstance();
    if (!pm.isUartEventOriginCurrent(cmd.uartOrigin)) {
        strlcpy(cmd.error, "uart_source_unconfirmed", sizeof(cmd.error)); return false;
    }
    const char* reason = nullptr;
    if (!cmd.workGeneration || pm.ordinaryModbusWorkCurrent(cmd.target, cmd.control, cmd.workGeneration, &reason)) return true;
    strlcpy(cmd.error, reason ? reason : "CONTROL_MOTOR_SOURCE_UNCONFIRMED", sizeof(cmd.error)); return false;
}

bool readMotorParameters(Command& cmd, ModbusHandler& bus, const ModbusSubDevice& dev,
                         int index, uint16_t& speedValue, uint16_t& pulseValue) {
    uint16_t timeout = 0, speedRead = 0, pulseRead = 0;
    bool speedOk = false, pulseOk = false;
    char speedError[60]{}, pulseError[60]{};
    const auto& protocol = dev.motorProtocol;
    const uint16_t speedReg = protocol.readRegister(dev.motorRegs, false);
    const uint16_t pulseReg = protocol.readRegister(dev.motorRegs, true);
    if (!transactionTimeout(cmd, timeout) || !commandSourceCurrent(cmd)) return false;
    {
        const auto speed = bus.readRegistersOnce(dev.slaveAddress, protocol.readFunction, speedReg, 1, true, timeout, 0);
        speedOk = speed.error == ONESHOT_SUCCESS && speed.count == 1;
        if (speedOk) speedRead = speed.data[0];
        describeResult(speedError, sizeof(speedError), "speed", dev.slaveAddress, protocol.readFunction, speedReg, speed, true);
    }
    if (cancelledByStop(cmd) || !transactionTimeout(cmd, timeout) || !commandSourceCurrent(cmd)) return false;
    {
        const auto pulse = bus.readRegistersOnce(dev.slaveAddress, protocol.readFunction, pulseReg, 1, true, timeout, 0);
        pulseOk = pulse.error == ONESHOT_SUCCESS && pulse.count == 1;
        if (pulseOk) pulseRead = pulse.data[0];
        describeResult(pulseError, sizeof(pulseError), "pulse", dev.slaveAddress, protocol.readFunction, pulseReg, pulse, true);
    }
    if (!commandSourceCurrent(cmd)) return false;
    if (!speedOk || !pulseOk) {
        snprintf(cmd.error, sizeof(cmd.error), "%s; %s", speedError, pulseError); return false;
    }
    speedValue = speedRead; pulseValue = pulseRead;
    bus.updateMotorRuntime(static_cast<uint8_t>(index), dev.motorCurrentPosition, pulseRead);
    return true;
}

bool completeMotorOutput(Command& cmd, ModbusHandler& bus, const ModbusSubDevice& dev,
                         int index, const char* action, uint16_t reg, uint16_t value, uint16_t pulse) {
    const bool move = !strcmp(action, "forward") || !strcmp(action, "reverse");
    uint16_t timeout = 0;
    if ((move && cancelledByStop(cmd)) || !transactionTimeout(cmd, timeout) || !commandSourceCurrent(cmd)) return false;
    {
        const auto result = writeMotor(bus, dev, cmd, reg, value, timeout);
        if (result.error != ONESHOT_SUCCESS) {
            if (result.error == ONESHOT_TRAVEL_BLOCKED)
                strlcpy(cmd.error, result.travelError ? result.travelError : "TRAVEL_BLOCKED", sizeof(cmd.error));
            else {
                char detail[80];
                describeResult(detail, sizeof(detail), action, dev.slaveAddress, dev.motorProtocol.writeFunction, reg, result, false);
                snprintf(cmd.error, sizeof(cmd.error), "%s; unconfirmed, no retry", detail);
            }
            return false;
        }
    }
    if ((strcmp(action, "stop") && cancelledByStop(cmd)) || !commandSourceCurrent(cmd)) return false;
    if (!strcmp(action, "setPulse")) pulse = value;
    bus.updateMotorRuntime(static_cast<uint8_t>(index), dev.motorCurrentPosition, pulse);
#if FASTBEE_ENABLE_MQTT
    char reportSensorId[sizeof(dev.sensorId) + 5]{};
    if (std::memchr(dev.sensorId, '\0', sizeof(dev.sensorId)) && dev.sensorId[0])
        snprintf(reportSensorId, sizeof(reportSensorId), "%s%s", dev.sensorId, dev.channelCount > 1 ? "_ch0" : "");
    const char* key = !strcmp(action, "forward") ? "fwd" : !strcmp(action, "reverse") ? "rev" :
        !strcmp(action, "setSpeed") ? "spd" : !strcmp(action, "setPulse") ? "pls" : "stop";
    reportAcknowledgement(cmd, reportSensorId, key, value);
#else
    cmd.reportState = ReportState::Disabled;
#endif
#if FASTBEE_ENABLE_PERIPH_EXEC
    JsonDocument event;
    event["d"] = index; event["a"] = action;
    event["origin"] = "motor-result"; event["operationId"] = cmd.id;
    String payload; serializeJson(event, payload);
    PeriphExecManager::getInstance().triggerEventById("mc:" + String(index), payload, cmd.uartOrigin);
#endif
    return true;
}

bool applyMotorParameter(Command& cmd, ModbusHandler& bus, const ModbusSubDevice& dev,
                         int index, const char* action, uint16_t value, uint16_t pulse) {
    FastBee::MotorTravelAdmission travel; String token;
    if (!bus.getMotorTravel(static_cast<uint8_t>(index), travel, token)) {
        strlcpy(cmd.error, "TRAVEL_BUS_BUSY", sizeof(cmd.error)); return false;
    }
    if (travel.mapping.required && !travel.stationary && !travel.stopAcknowledged) {
        uint16_t timeout = 0;
        if (cancelledByStop(cmd) || !transactionTimeout(cmd, timeout) || !commandSourceCurrent(cmd)) return false;
        {
            const auto stopped = writeMotor(bus, dev, cmd, dev.motorRegs[2], dev.motorProtocol.stopValue, timeout);
            if (stopped.error != ONESHOT_SUCCESS) {
                describeResult(cmd.error, sizeof(cmd.error), "pre-parameter stop", dev.slaveAddress,
                               dev.motorProtocol.writeFunction, dev.motorRegs[2], stopped, false);
                return false;
            }
        }
        Lock lock;
        if (!lock.held) {
            strlcpy(cmd.error, "Motor service busy; parameter not written", sizeof(cmd.error)); return false;
        }
        for (auto& state : lock.state->states) if (!strcmp(state.ref, cmd.ref)) {
            strlcpy(state.value.lastAcknowledgedAction, "stop", sizeof(state.value.lastAcknowledgedAction));
            state.value.acknowledgedMs = millis(); state.value.known = false;
        }
    }
    if (cancelledByStop(cmd)) return false;
    const uint16_t reg = dev.motorRegs[!strcmp(action, "setSpeed") ? 3 : 4];
    return completeMotorOutput(cmd, bus, dev, index, action, reg, value, pulse);
}

void execute(Command& cmd) {
    auto sourceCurrent = [&]() { return commandSourceCurrent(cmd); };
    if (!sourceCurrent()) return;
    auto owner = lease();
    if (!sourceCurrent()) return;
    auto* bus = owner.get();
    ModbusSubDevice dev; FastBee::ModbusControlTarget target;
    if (!bus || !snapshotMotor(*bus, String(cmd.ref), dev, target) || !cmd.target.sameMapping(target)) {
        strlcpy(cmd.error, "CONTROL_MOTOR_TARGET_STALE", sizeof(cmd.error)); return;
    }
    const int index = target.index;
    const auto& protocol = dev.motorProtocol;
    if (!protocol.valid) { strlcpy(cmd.error, "Invalid motor protocol; control blocked", sizeof(cmd.error)); return; }
    uint16_t timeout = 0;
    if (!transactionTimeout(cmd, timeout)) return;
    const bool forward = !strcmp(cmd.action, "forward"), reverse = !strcmp(cmd.action, "reverse");
    const bool move = forward || reverse;
    uint16_t reg = 0, value = static_cast<uint16_t>(cmd.value);
    if (!strcmp(cmd.action, "readStatus")) {
        cmd.success = readMotorParameters(cmd, *bus, dev, index, cmd.speed, cmd.pulse);
        cmd.known = cmd.success;
        return;
    }
    uint16_t currentPulse = dev.motorLastPulse;
    if (move) {
        if (!protocol.finitePulseMove) {
            strlcpy(cmd.error, "Finite-pulse motion not confirmed; start blocked", sizeof(cmd.error)); return;
        }
        MotorControlState feedback;
        if (!MotorControlService::instance().state(String(cmd.ref), feedback)) {
            strlcpy(cmd.error, "Motor parameters unconfirmed; start blocked", sizeof(cmd.error));
            return;
        }
        if (feedback.stopRequired) {
            if (cancelledByStop(cmd) || !transactionTimeout(cmd, timeout)) return;
            if (!sourceCurrent()) return;
            const auto stopped = writeMotor(*bus, dev, cmd, dev.motorRegs[2], protocol.stopValue, timeout);
            if (stopped.error != ONESHOT_SUCCESS) {
                describeResult(cmd.error, sizeof(cmd.error), "pre-start stop", dev.slaveAddress,
                               protocol.writeFunction, dev.motorRegs[2], stopped, false);
                return;
            }
            // Acknowledge the command, without claiming physical rest or
            // narrowing the reserved position interval.
            Lock lock;
            if (!lock.held) {
                strlcpy(cmd.error, "Motor service busy; command not started", sizeof(cmd.error));
                return;
            }
            for (auto& state : lock.state->states) if (!strcmp(state.ref, cmd.ref)) {
                strlcpy(state.value.lastAcknowledgedAction, "stop", sizeof(state.value.lastAcknowledgedAction));
                state.value.acknowledgedMs = millis();
            }
        }
        auto refreshParameters = [&]() -> bool {
            // Refresh expired feedback inside the same queued operation. Never
            // retry the requested start or infer parameters from input fields.
            if (!readMotorParameters(cmd, *bus, dev, index, feedback.speed, feedback.pulse)) return false;
            Lock lock;
            if (!lock.held) {
                strlcpy(cmd.error, "Motor service busy; command not started", sizeof(cmd.error));
                return false;
            }
            for (auto& state : lock.state->states) if (!strcmp(state.ref, cmd.ref)) {
                state.value.known = true;
                state.value.speed = feedback.speed; state.value.pulse = feedback.pulse;
                state.value.updatedMs = millis(); state.value.readError[0] = '\0';
            }
            feedback.known = true;
            return true;
        };
        if (!feedback.known || feedback.readbackRequired || !strcmp(feedback.travelReason, "TRAVEL_FRESH_PULSE_READBACK_REQUIRED")) {
            if (!refreshParameters()) return;
        }
        if (!feedback.speed || !feedback.pulse) {
            // An untouched controller may read zero. Supply bounded jog defaults
            // on this explicit click, then verify them; never start on an echo alone.
            for (unsigned parameter = 0; parameter < 2; ++parameter) {
                if (parameter ? feedback.pulse != 0 : feedback.speed != 0) continue;
                // Reuse the admitted operation and its evidence prefix. Never
                // create a recursive execution frame or retry a physical write.
                const ReportState requestedReportState = cmd.reportState;
                const bool applied = applyMotorParameter(cmd, *bus, dev, index,
                    parameter ? "setPulse" : "setSpeed", parameter ? 100 : 10, feedback.pulse);
                cmd.reportState = requestedReportState;
                if (!applied) return;
            }
            if (!refreshParameters()) return;
            if (!feedback.speed || !feedback.pulse) {
                strlcpy(cmd.error, "Motor default parameters were not accepted", sizeof(cmd.error)); return;
            }
        }
        if (cancelledByStop(cmd)) return;
        currentPulse = feedback.pulse;
        reg = dev.motorRegs[forward ? 0 : 1];
        value = cmd.legacyValue && !protocol.configured ? value : forward ? protocol.forwardValue : protocol.reverseValue;
    }
    else if (!strcmp(cmd.action, "stop")) {
        reg = dev.motorRegs[2]; value = protocol.stopValue; // Exact saved emergency stop; no arbitrary rule alias.
    }
    else {
        cmd.success = applyMotorParameter(cmd, *bus, dev, index, cmd.action, value, currentPulse);
        return;
    }
    // START still passes through the bus interlock and never replays an
    // ambiguous output. Preserve the fresh pulse after default preparation.
    cmd.success = completeMotorOutput(cmd, *bus, dev, index, cmd.action, reg, value, currentPulse);
}

void worker(void*) {
    uint32_t lastWork = millis();
    while (true) {
        int selected = -1; Command cmd; bool retire = false;
        {
            Lock lock(portMAX_DELAY);
            if (lock.held) {
                selected = lock.state->selectNext();
                if (selected >= 0) { lock.state->commands[selected].phase = Phase::RUNNING; cmd = lock.state->commands[selected]; }
                else if (millis() - lastWork >= 60000U) { lock.state->worker = nullptr; retire = true; }
            }
        }
        if (selected < 0) {
            if (retire) { vTaskDelete(nullptr); return; }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); continue;
        }
        try {
            const char* reason = nullptr;
            if (cmd.workGeneration && !PeripheralManager::getInstance().activateOrdinaryModbusWork(
                    cmd.target, cmd.control, cmd.workGeneration, &reason))
                strlcpy(cmd.error, !PeripheralManager::getInstance().isUartEventOriginCurrent(cmd.uartOrigin) ?
                    "uart_source_unconfirmed" : cmd.control.operationDeadlineAt &&
                    static_cast<int32_t>(static_cast<uint32_t>(millis()) - cmd.control.operationDeadlineAt) >= 0 ?
                    "Motor command deadline exceeded" : reason ? reason : "CONTROL_MOTOR_WORK_RETIRED", sizeof(cmd.error));
            else execute(cmd);
        } catch (...) {
            cmd.success = false;
            strlcpy(cmd.error, "CONTROL_MOTOR_WORK_EXCEPTION", sizeof(cmd.error));
        }
        if (cmd.reportState == ReportState::Pending) cmd.reportState = ReportState::NotProduced;
        lastWork = millis();
        cmd.phase = Phase::DONE; cmd.finishedMs = millis();
        bool published = false;
        {
            Lock lock(portMAX_DELAY);
            if (lock.held && lock.state->commands[selected].id == cmd.id &&
                lock.state->commands[selected].phase == Phase::RUNNING) {
                lock.state->commands[selected] = cmd;
                published = true;
                for (auto& state : lock.state->states) if (!strcmp(state.ref, cmd.ref)) {
                    if (!strcmp(cmd.action, "readStatus")) {
                        state.value.known = cmd.known;
                        if (cmd.known) { state.value.speed = cmd.speed; state.value.pulse = cmd.pulse; state.value.updatedMs = cmd.finishedMs; }
                        strlcpy(state.value.readError, cmd.error, sizeof(state.value.readError));
                    } else {
                        if (cmd.success && (!strcmp(cmd.action, "forward") ||
                            !strcmp(cmd.action, "reverse") || !strcmp(cmd.action, "stop"))) {
                            strlcpy(state.value.lastAcknowledgedAction, cmd.action, sizeof(state.value.lastAcknowledgedAction));
                            state.value.acknowledgedMs = cmd.finishedMs;
                        }
                        if (state.value.operationId == cmd.id) {
                            state.value.commandKnown = true;
                            state.value.commandSuccess = cmd.success;
                            state.value.commandUpdatedMs = cmd.finishedMs;
                            strlcpy(state.value.error, cmd.error, sizeof(state.value.error));
                        }
                        // Parameter acknowledgement does not refresh a full readback.
                        if (cmd.success && !strcmp(cmd.action, "setSpeed")) state.value.speed = static_cast<uint16_t>(cmd.value);
                        if (cmd.success && !strcmp(cmd.action, "setPulse")) state.value.pulse = static_cast<uint16_t>(cmd.value);
                        if (!strcmp(cmd.action, "setSpeed") || !strcmp(cmd.action, "setPulse")) state.value.known = false;
                    }
                }
            }
        }
        // The result becomes terminal before the PM work reservation is
        // acknowledged. A caller timeout cannot release a running worker.
        if (published && cmd.workGeneration) {
            const bool terminal = PeripheralManager::getInstance().finishOrdinaryModbusWork(
                cmd.target, cmd.control, cmd.workGeneration, !cmd.success);
            Lock lock(portMAX_DELAY);
            if (lock.held && lock.state->commands[selected].id == cmd.id &&
                lock.state->commands[selected].phase == Phase::DONE)
                lock.state->commands[selected].terminalAcknowledged = terminal;
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
    FastBee::ModbusControlTarget target; return snapshotTarget(ref, target);
}
bool MotorControlService::snapshotTarget(const String& ref, FastBee::ModbusControlTarget& target) {
    auto owner = lease(); ModbusSubDevice device;
    return owner.get() && snapshotMotor(*owner.get(), ref, device, target);
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
        item["group"] = "motor"; item["available"] = dev.motorProtocol.valid; item["slaveAddress"] = dev.slaveAddress;
        item["finitePulseMove"] = dev.motorProtocol.finitePulseMove;
        item["rangeMin"] = 0; item["rangeMax"] = 65535;
        item["precision"] = dev.motorDecimals; item["motionFeedback"] = false;
        item["softLimitEnabled"] = dev.hasMotorSoftLimit();
        item["minPosition"] = dev.motorMinPosition; item["maxPosition"] = dev.motorMaxPosition;
        item["positionFeedback"] = false;
    }
}
bool MotorControlService::submit(const String& ref, const String& action, int value,
        const String& requestId, uint32_t& operationId, String& error,
        const FastBee::OrdinaryControlContext* control) {
    return submitImpl(ref, action, value, requestId, 0, 0, false, operationId, error,
        control ? control->uartOrigin : FastBee::UartEventOrigin{}, control);
}
bool MotorControlService::submitMqtt(const String& ref, const String& action, int value,
        const String& requestId, const String& reportId, uint16_t requestedValue,
        uint32_t& operationId, String& error, const FastBee::OrdinaryControlContext& control) {
    if (control.request.actor.source != FastBee::OrdinaryControl::Source::Mqtt ||
        !control.request.actor.identity || !control.uartOrigin.unbound() || reportId.isEmpty() ||
        reportId.length() >= sizeof(Command::mqttReportId) || reportId.length() != std::strlen(reportId.c_str())) {
        operationId = 0; error = "CONTROL_MQTT_MOTOR_IDENTITY_INVALID"; return false;
    }
    // Preserve unconfigured legacy register values and explicitly saved
    // protocol values, without constructing a new operation deadline.
    return submitImpl(ref, action, value, requestId, 0, 0, true, operationId, error,
        control.uartOrigin, &control, reportId, requestedValue);
}
bool MotorControlService::submitRule(const String& ref, const String& action, int value,
        const String& requestId, uint32_t budgetMs, uint16_t timeoutMs, uint32_t& operationId, String& error,
        const FastBee::UartEventOrigin& uartOrigin, const FastBee::OrdinaryControlContext* control) {
    if (budgetMs <= 2100) { error = "Motor command deadline exceeded"; return false; }
    return submitImpl(ref, action, value, requestId, std::min<uint32_t>(budgetMs, 30000),
                      std::max<uint16_t>(100, std::min<uint16_t>(timeoutMs, 5000)), true, operationId, error, uartOrigin, control);
}
bool MotorControlService::submitImpl(const String& ref, const String& action, int value,
        const String& requestId, uint32_t budgetMs, uint16_t timeoutMs, bool legacyValue,
        uint32_t& operationId, String& error, const FastBee::UartEventOrigin& uartOrigin,
        const FastBee::OrdinaryControlContext* control,
        const String& mqttReportId, uint16_t mqttRequestedValue) {
    operationId = 0;
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        error = "uart_source_unconfirmed"; return false;
    }
    const bool read = action == "readStatus", stop = action == "stop";
    if (!read && (!control || !control->request.operation ||
        control->uartOrigin.required != uartOrigin.required || control->uartOrigin.configEpoch != uartOrigin.configEpoch ||
        control->uartOrigin.receiveGeneration != uartOrigin.receiveGeneration)) {
        error = "CONTROL_CONTEXT_UNCONFIRMED"; return false;
    }
    if ((!read && !stop && action != "forward" && action != "reverse" && action != "setSpeed" && action != "setPulse") ||
        value < 0 || value > 65535 || ref.length() >= 48 || requestId.length() >= 64 ||
        (!read && requestId.isEmpty())) { error = "Invalid motor command or request ID"; return false; }
    // Snapshot at admission, not worker execution. Release the MQTT lease
    // before waiting on the motor bus, queue mutex or any physical operation.
    uint32_t reportInstanceId = 0;
#if FASTBEE_ENABLE_MQTT
    if (!read) reportInstanceId = FastBee::captureReportInstance([] { return reportClient(); });
#endif
    // The scale belongs to the validated device identity, never the layout's
    // free-form display precision. Cache it even before the first readback.
    uint8_t precision = 0;
    FastBee::ModbusControlTarget target;
    bool softLimitEnabled = false;
    int32_t minPosition = 0, maxPosition = 0, estimatedPosition = 0;
    {
        auto owner = lease(); auto* bus = owner.get();
        ModbusSubDevice dev;
        if (!bus || !snapshotMotor(*bus, ref, dev, target)) {
            error = "Motor unavailable or configuration changed"; return false;
        }
        precision = std::min<uint8_t>(dev.motorDecimals, 6);
        softLimitEnabled = dev.hasMotorSoftLimit();
        minPosition = dev.motorMinPosition; maxPosition = dev.motorMaxPosition;
        estimatedPosition = dev.motorCurrentPosition;
    }
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        error = "uart_source_unconfirmed"; return false;
    }
    Lock lock;
    if (!lock.held) { error = "Motor service busy"; return false; }
    auto& s = *lock.state;
    int slot = -1; const char* queueError = nullptr;
    if (!s.reserve(ref.c_str(), action.c_str(), value, requestId.c_str(), millis(), slot, operationId, queueError, uartOrigin, read ? nullptr : control, &target)) {
        error = queueError; return false;
    }
    if (operationId) {
        for (const auto& existing : s.commands) if (existing.id == operationId) {
            if (mqttReportId != existing.mqttReportId || mqttRequestedValue != existing.mqttRequestedValue) {
                operationId = 0; error = "Motor report identity differs from retained request"; return false;
            }
            break;
        }
        return true;
    }
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
    if (state) {
        state->value.precision = precision;
        state->value.softLimitEnabled = softLimitEnabled;
        state->value.minPosition = minPosition; state->value.maxPosition = maxPosition;
        state->value.estimatedPosition = estimatedPosition;
    }
    if (!s.worker && xTaskCreate(worker, "motor_control", 6144, nullptr, 1, &s.worker) != pdPASS) {
        s.worker = nullptr; error = "Motor worker unavailable"; return false;
    }
    uint64_t workGeneration = 0;
    FastBee::OrdinaryControlResult ordinary;
    if (!read && !PeripheralManager::getInstance().reserveOrdinaryModbusWork(
            target, *control, workGeneration, ordinary, stop)) {
        error = ordinary.reason; return false;
    }
    if (stop) {
        // Queue -> PM is the only nested owner order. Neither code path waits
        // for a bus or calls back into this queue while PM is held.
        for (auto& queued : s.commands) if (ref == queued.ref && queued.phase == Phase::QUEUED) {
            queued.phase = Phase::DONE; queued.finishedMs = millis(); queued.success = false;
            strlcpy(queued.error, "Cancelled by emergency stop", sizeof(queued.error));
            if (queued.reportState == ReportState::Pending) queued.reportState = ReportState::NotProduced;
            if (queued.workGeneration)
                queued.terminalAcknowledged = PeripheralManager::getInstance().finishOrdinaryModbusWork(
                    queued.target, queued.control, queued.workGeneration, true);
        }
    }
    auto& c = s.commands[slot]; c = Command{};
    c.id = s.allocateId(); c.order = ++s.nextOrder;
    // A rule carries its original operation deadline through snapshot/queue
    // waits; accepting a worker must never restart that budget.
    c.deadlineMs = control && control->operationDeadlineAt ? control->operationDeadlineAt :
        budgetMs ? millis() + budgetMs : 0;
    if (budgetMs && !c.deadlineMs) c.deadlineMs = 1;
    c.timeoutMs = timeoutMs; c.legacyValue = legacyValue;
    c.uartOrigin = uartOrigin; c.target = target;
    if (!read) { c.control = *control; c.workGeneration = workGeneration; }
    strlcpy(c.mqttReportId, mqttReportId.c_str(), sizeof(c.mqttReportId));
    c.mqttRequestedValue = mqttRequestedValue;
    c.reportInstanceId = reportInstanceId;
    c.reportState = read ? ReportState::NotApplicable : ReportState::Pending;
    strlcpy(c.ref, ref.c_str(), sizeof(c.ref)); strlcpy(c.action, action.c_str(), sizeof(c.action));
    strlcpy(c.requestId, requestId.c_str(), sizeof(c.requestId)); c.value = value; c.phase = Phase::QUEUED;
    if (state && !read) {
        state->value.operationId = c.id; state->value.commandKnown = false;
        state->value.commandSuccess = false; state->value.error[0] = 0;
        strlcpy(state->value.lastAction, action.c_str(), sizeof(state->value.lastAction));
    }
    operationId = c.id; xTaskNotifyGive(s.worker); return true;
}
bool MotorControlService::operation(uint32_t id, JsonObject out, bool* busy) {
    Lock lock;
    if (busy) *busy = !lock.held;
    if (!lock.held) return false;
    for (const auto& c : lock.state->commands) if (c.id == id && c.phase != Phase::EMPTY) {
        out["operationId"] = id; out["pending"] = c.phase != Phase::DONE || (c.workGeneration && !c.terminalAcknowledged);
        out["success"] = c.phase == Phase::DONE && c.success && (!c.workGeneration || c.terminalAcknowledged); out["action"] = c.action;
        out["error"] = c.error; out["motionKnown"] = false;
        out["reportAdmission"] = MotorCommands::reportStateName(c.reportState);
        out["reportDeliveryVerified"] = false;
        out["physicalOutputVerified"] = false;
        out["outputAttemptKnown"] = c.phase != Phase::RUNNING;
        bool attempted = false;
        for (uint8_t i = 0; i < c.outputStepCount; ++i) attempted |= c.outputSteps[i].attempted;
        if (c.phase != Phase::RUNNING) out["outputAttempted"] = attempted;
        else out["outputAttempted"] = nullptr;
        out["terminalAcknowledged"] = c.workGeneration ? c.terminalAcknowledged : c.phase == Phase::DONE;
        JsonArray steps = out["outputSteps"].to<JsonArray>();
        for (uint8_t i = 0; i < c.outputStepCount; ++i) {
            const auto& step = c.outputSteps[i]; JsonObject item = steps.add<JsonObject>();
            item["function"] = step.function; item["address"] = step.address; item["value"] = step.value;
            item["attempted"] = step.attempted; item["commandAcknowledged"] = step.acknowledged;
            item["error"] = step.error; item["physicalOutputVerified"] = false;
        }
        if (c.phase == Phase::DONE && !strcmp(c.action, "readStatus")) out["parametersKnown"] = c.known;
        if (c.known) { out["speed"] = c.speed; out["pulse"] = c.pulse; }
        return true;
    }
    return false;
}
bool MotorControlService::state(const String& ref, MotorControlState& out) {
    auto owner = lease(); auto* bus = owner.get();
    const int index = bus ? findMotor(*bus, ref) : -1;
    if (index < 0) return false;
    FastBee::MotorTravelAdmission travel; String token;
    const bool snapshot = bus->getMotorTravel(static_cast<uint8_t>(index), travel, token);
    Lock lock; if (!lock.held) return false;
    out = MotorControlState{};
    for (const auto& item : lock.state->states) if (ref == item.ref) {
        out = item.value; out.pending = false;
        out.parametersCached = out.known;
        out.parametersFresh = out.known && millis() - out.updatedMs <= 15000U;
        if (!out.parametersFresh) out.known = false;
        // Display busy state represents a control request. Polling readback
        // has its own operation status and must not disable the controls.
        for (const auto& cmd : lock.state->commands)
            if (ref == cmd.ref && strcmp(cmd.action, "readStatus") &&
                (cmd.phase == Phase::QUEUED || cmd.phase == Phase::RUNNING ||
                 (cmd.phase == Phase::DONE && cmd.workGeneration && !cmd.terminalAcknowledged))) out.pending = true;
        break;
    }
    const auto& dev = bus->getSubDevice(static_cast<uint8_t>(index));
    out.precision = dev.motorDecimals;
    out.softLimitEnabled = dev.hasMotorSoftLimit();
    out.minPosition = dev.motorMinPosition; out.maxPosition = dev.motorMaxPosition;
    out.referenceKnown = snapshot && travel.position.referenced;
    out.stationaryConfirmed = snapshot && travel.stationary;
    // A busy snapshot cannot justify skipping preparation. The queued bus
    // transactions can wait for their turn within the existing command budget.
    out.stopRequired = !snapshot || (travel.mapping.required && !travel.stationary && !travel.stopAcknowledged);
    out.readbackRequired = !snapshot || (travel.mapping.required &&
        (!travel.pulseKnown || static_cast<uint32_t>(millis() - travel.pulseAt) > 15000U));
    out.possiblePositionMin = out.referenceKnown ? travel.position.lower : 0;
    out.possiblePositionMax = out.referenceKnown ? travel.position.upper : 0;
    out.commandPosition = snapshot && travel.mapping.commandPosition;
    out.estimatedPosition = out.referenceKnown && out.commandPosition ? travel.position.lower : 0;
    strlcpy(out.travelToken, snapshot ? token.c_str() : "", sizeof(out.travelToken));
    const char* forwardError = snapshot ? travel.checkStart(true, travel.pulse, millis()) : "TRAVEL_BUS_BUSY";
    const char* reverseError = snapshot ? travel.checkStart(false, travel.pulse, millis()) : "TRAVEL_BUS_BUSY";
    out.forwardReady = !forwardError; out.reverseReady = !reverseError;
    strlcpy(out.travelReason, forwardError && reverseError ? forwardError : "", sizeof(out.travelReason));
    return true;
}

bool MotorControlService::confirmTravel(const String& ref, const String& token, bool reference,
                                       int32_t position, bool stationary, String& error) {
    auto owner = lease(); auto* bus = owner.get();
    const int index = bus ? findMotor(*bus, ref) : -1;
    if (index < 0) { error = "Motor unavailable or configuration changed"; return false; }
    Lock lock;
    if (!lock.held) { error = "Motor service busy"; return false; }
    for (const auto& cmd : lock.state->commands) {
        if (ref == cmd.ref && (cmd.phase == Phase::QUEUED || cmd.phase == Phase::RUNNING)) {
            error = "TRAVEL_COMMAND_PENDING"; return false;
        }
    }
    return bus->confirmMotorTravel(static_cast<uint8_t>(index), token, reference, position, stationary, error);
}
#endif
