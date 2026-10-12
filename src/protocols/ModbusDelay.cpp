// Runtime delayed commands use the same semaphore as real Modbus traffic.
// This is stale-command retirement, not ordinary-source lease arbitration.
#include "protocols/ModbusHandler.h"
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstdint>

#if FASTBEE_ENABLE_MODBUS
namespace {
struct DelayBusGuard {
    SemaphoreHandle_t semaphore;
    bool held;
    explicit DelayBusGuard(SemaphoreHandle_t value) : semaphore(value),
        held(value && xSemaphoreTake(value, 0) == pdTRUE) {}
    ~DelayBusGuard() { if (held) xSemaphoreGive(semaphore); }
};
const char* delayPhaseName(CoilDelayTask::Phase phase) {
    switch (phase) {
    case CoilDelayTask::Phase::Pending: return "PENDING";
    case CoilDelayTask::Phase::Acknowledged: return "ACKNOWLEDGED";
    case CoilDelayTask::Phase::ResultUnknown: return "RESULT_UNKNOWN";
    case CoilDelayTask::Phase::Retired: return "RETIRED";
    default: return "EMPTY";
    }
}
void delayDecimal(JsonObject object, const char* key, uint64_t value) {
    char text[21];
    snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    object[key] = text; // ArduinoJson copies the local mutable character array.
}
}

void ModbusHandler::invalidateCoilDelayTasksLocked(const char* reason) {
    // Zero is permanently invalid on generation exhaustion, never a wrap.
    if (_delayConfiguration != 0)
        _delayConfiguration = _delayConfiguration == UINT32_MAX ? 0 : _delayConfiguration + 1;
    _delayLastWrite = ModbusDelayReceipt{};
    for (auto& task : coilDelayTasks) {
        if (task.phase != CoilDelayTask::Phase::Pending) continue;
        task.phase = CoilDelayTask::Phase::Retired;
        task.reason = reason;
        task.attempted = false;
    }
    for(auto& task:ordinaryCoilDelayTasks) if(task.phase==CoilDelayTask::Phase::Pending && !task.hardwareOnly) {
        task.phase=CoilDelayTask::Phase::Retired;task.reason=reason;
    }
    _ordinaryDelayAckOperation=0;_ordinaryDelayAckWindowMs=0;
    // Unknown terminal records/cumulative failure evidence remain untouched.
}

bool ModbusHandler::coilDelayTimeLocked(uint64_t& now) {
    const int64_t micros = esp_timer_get_time();
    if (micros < 0 || static_cast<uint64_t>(micros / 1000) < _delayLastTimeMs) {
        invalidateCoilDelayTasksLocked("MONOTONIC_CLOCK_REVERSED");
        return false;
    }
    now = static_cast<uint64_t>(micros / 1000);
    _delayLastTimeMs = now;
    return true;
}

void ModbusHandler::trackCoilDelayWriteLocked(const uint8_t* request, size_t length) {
    if (!request || length < 4) return;
    const uint8_t slave = request[0], function = request[1];
    if (function >= 1 && function <= 4) return;
    _delayLastWrite = ModbusDelayReceipt{};
    _ordinaryDelayAckOperation=0;_ordinaryDelayAckWindowMs=0;
    if (_delayWriteSequence == UINT64_MAX) {
        invalidateCoilDelayTasksLocked("WRITE_SEQUENCE_EXHAUSTED");
        _delayConfiguration = 0;
        return;
    }
    ++_delayWriteSequence;
    const bool single = length == 8 && (function == 5 || function == 6);
    const uint32_t address = length >= 6 ? (request[2] << 8) | request[3] : 0;
    uint32_t quantity = single ? 1 : 0;
    const bool range = (function == 15 || function == 16) && length >= 9;
    if (range) quantity = (request[4] << 8) | request[5];
    const bool coilSpace = function == 5 || function == 15;
    const uint32_t expectedBytes = coilSpace ? (quantity + 7) / 8 : quantity * 2;
    bool precise = single || (range && quantity > 0 && address + quantity <= 65536 &&
        expectedBytes <= 255 && request[6] == expectedBytes && length == expectedBytes + 9);
    // Proprietary FC05 timing values and configured batch-register aliases
    // may affect channels outside the encoded address: retire this slave.
    if (single && function == 5 &&
        !((request[4] == 0 && request[5] == 0) || (request[4] == 0xFF && request[5] == 0)))
        precise = false;
    if (single && function == 6) {
        for (uint8_t i = 0; i < config.master.deviceCount; ++i) {
            const auto& device = config.master.devices[i];
            if (device.slaveAddress == slave && device.batchRegister != 0 &&
                device.batchRegister == address) precise = false;
        }
    }
    for (auto& task : coilDelayTasks) {
        if (task.phase != CoilDelayTask::Phase::Pending) continue;
        if (slave != 0 && task.receipt.slave != slave) continue;
        const bool sameSpace = (task.receipt.function == 5) == coilSpace;
        if (slave == 0 || !precise || (sameSpace && task.receipt.address >= address &&
            static_cast<uint32_t>(task.receipt.address) < address + quantity)) {
            task.phase = CoilDelayTask::Phase::Retired;
            task.attempted = false;
            task.reason = "NEWER_WRITE";
        }
    }
    for(auto& task:ordinaryCoilDelayTasks) {
        if(task.phase!=CoilDelayTask::Phase::Pending || task.running || task.hardwareOnly)continue;
        if(slave && task.receipt.slave!=slave)continue;
        const bool sameSpace=(task.receipt.function==5)==coilSpace;
        if(!slave || !precise || (sameSpace && task.receipt.address>=address &&
            uint32_t(task.receipt.address)<address+quantity)) {
            task.phase=CoilDelayTask::Phase::Retired;task.reason="NEWER_WRITE";
        }
    }
    if (single && slave >= 1 && slave <= 247) {
        _delayLastWrite.instance = _delayInstance;
        _delayLastWrite.configuration = _delayConfiguration;
        _delayLastWrite.writeSequence = _delayWriteSequence;
        _delayLastWrite.slave = slave;
        _delayLastWrite.function = function;
        _delayLastWrite.address = static_cast<uint16_t>(address);
    }
}

bool ModbusHandler::addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr,
    unsigned long delayMs, bool coilValue, bool isRegisterMode,
    const ModbusDelayReceipt& receipt, uint64_t* operationId) {
    if (operationId) *operationId = 0;
    DelayBusGuard lock(_oneShotSemaphore);
    if (!lock.held) return false;
    uint64_t now = 0;
    if (!isInitialized || config.mode != MODBUS_MASTER || !coilDelayTimeLocked(now) ||
        !_delayInstance || !_delayConfiguration || !delayMs || delayMs > 26000 ||
        now > UINT64_MAX - delayMs || _delayNextOperation == UINT64_MAX ||
        _delayWriteSequence == UINT64_MAX ||
        slaveAddr == 0 || slaveAddr > 247 || receipt.instance != _delayInstance ||
        receipt.configuration != _delayConfiguration || receipt.slave != slaveAddr ||
        receipt.address != coilAddr || receipt.function != (isRegisterMode ? 6 : 5) ||
        receipt.writeSequence == 0 || receipt.writeSequence != _delayWriteSequence ||
        receipt.writeSequence <= _delayArmedSequence ||
        receipt.instance != _delayLastWrite.instance ||
        receipt.configuration != _delayLastWrite.configuration ||
        receipt.writeSequence != _delayLastWrite.writeSequence ||
        receipt.slave != _delayLastWrite.slave || receipt.function != _delayLastWrite.function ||
        receipt.address != _delayLastWrite.address) return false;
    for (auto& task : coilDelayTasks) {
        // Unknown outputs retain their own evidence until handler destruction.
        if (task.phase == CoilDelayTask::Phase::Pending ||
            task.phase == CoilDelayTask::Phase::ResultUnknown) continue;
        task = CoilDelayTask{};
        task.receipt = receipt;
        task.operation = ++_delayNextOperation;
        task.dueMs = now + delayMs;
        task.coilValue = coilValue;
        task.phase = CoilDelayTask::Phase::Pending;
        task.error = ONESHOT_SUCCESS;
        task.reason = "SCHEDULED";
        _delayArmedSequence = receipt.writeSequence;
        if (operationId) *operationId = task.operation;
        return true;
    }
    return false;
}

void ModbusHandler::processCoilDelayTasks() {
    DelayBusGuard lock(_oneShotSemaphore);
    if (!lock.held || !isInitialized || config.mode != MODBUS_MASTER) return;
    if (_delayWriteSequence == UINT64_MAX) {
        invalidateCoilDelayTasksLocked("WRITE_SEQUENCE_EXHAUSTED");
        _delayConfiguration = 0;
        return;
    }
    uint64_t now = 0;
    if (!coilDelayTimeLocked(now)) return;
    for (auto& task : coilDelayTasks) {
        if (task.phase != CoilDelayTask::Phase::Pending) continue;
        if (task.receipt.instance != _delayInstance ||
            task.receipt.configuration != _delayConfiguration || !_delayConfiguration) {
            task.phase = CoilDelayTask::Phase::Retired;
            task.reason = "HANDLER_GENERATION_CHANGED";
            continue;
        }
        if (now < task.dueMs) continue;
        const uint16_t value = task.coilValue ? (task.receipt.function == 5 ? 0xFF00 : 1) : 0;
        uint8_t request[8] = { task.receipt.slave, task.receipt.function,
            static_cast<uint8_t>(task.receipt.address >> 8), static_cast<uint8_t>(task.receipt.address),
            static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), 0, 0 };
        const uint16_t crc = calculateCRC(request, 6);
        request[6] = static_cast<uint8_t>(crc);
        request[7] = static_cast<uint8_t>(crc >> 8);
        // Retire from runnable state before entering the synchronous bus call.
        // Do not call writeCoilOnce/writeRegisterOnce under this binary lock.
        task.phase = CoilDelayTask::Phase::Retired;
        const uint64_t beforeWrite = _delayWriteSequence;
        isOneShotReading = true;
        const OneShotResult result = _executeModbusTransaction(
            request, sizeof(request), task.receipt.slave, 0, 0, true);
        isOneShotReading = false;
        task.attempted = _delayWriteSequence != beforeWrite;
        task.error = result.error;
        task.exceptionCode = result.exceptionCode;
        if (result.error == ONESHOT_SUCCESS && result.delayReceipt.writeSequence != 0) {
            task.phase = CoilDelayTask::Phase::Acknowledged;
            task.reason = "FINAL_COMMAND_ACKNOWLEDGED";
        } else {
            if (task.error == ONESHOT_SUCCESS) task.error = ONESHOT_CRC_ERROR;
            task.phase = CoilDelayTask::Phase::ResultUnknown;
            task.reason = task.attempted ? "FINAL_RESULT_UNKNOWN" : "FINAL_NOT_ATTEMPTED";
            if (_delayFailures != UINT32_MAX) ++_delayFailures;
            if (_delayFirstFailureOperation == 0) {
                _delayFirstFailureOperation = task.operation;
                _delayFirstFailure = task.error;
            }
        }
        return; // At most one bounded transaction per handle tick.
    }
}

bool ModbusHandler::getCoilDelayStatus(JsonObject output) {
    DelayBusGuard lock(_oneShotSemaphore);
    output["physicalOutputVerified"] = false;
    if (!lock.held) { output["available"] = false; output["reason"] = "BUS_BUSY"; return false; }
    output["available"] = true;
    output["handlerInstance"] = _delayInstance;
    output["configuration"] = _delayConfiguration;
    output["failureCount"] = _delayFailures;
    delayDecimal(output, "firstFailureOperation", _delayFirstFailureOperation);
    output["firstFailureError"] = static_cast<uint8_t>(_delayFirstFailure);
    JsonArray records = output["records"].to<JsonArray>();
    for (const auto& task : coilDelayTasks) {
        if (task.phase == CoilDelayTask::Phase::Empty) continue;
        JsonObject record = records.add<JsonObject>();
        record["handlerInstance"] = task.receipt.instance;
        record["configuration"] = task.receipt.configuration;
        delayDecimal(record, "operationId", task.operation);
        delayDecimal(record, "initialWriteSequence", task.receipt.writeSequence);
        record["slaveAddress"] = task.receipt.slave;
        record["function"] = task.receipt.function;
        record["address"] = task.receipt.address;
        record["targetValue"] = task.coilValue ? 1 : 0;
        record["phase"] = delayPhaseName(task.phase);
        record["reason"] = task.reason;
        record["initialCommandAcknowledged"] = true;
        record["finalCommandAcknowledged"] = task.phase == CoilDelayTask::Phase::Acknowledged;
        record["attempted"] = task.attempted; // Entered transport; not proof of physical TX/output.
        record["error"] = static_cast<uint8_t>(task.error);
        record["exceptionCode"] = task.exceptionCode;
        record["physicalOutputVerified"] = false;
    }
    appendOrdinaryDelayStatusLocked(records);
    return true; // Owning response document checks allocation/overflow.
}
#endif
