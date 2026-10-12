#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"
#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/SensorConfigEpoch.h"
#include "core/MemoryBudget.h"
#include "network/FixedJsonField.h"
#include "utils/SystemTimeQuality.h"
#include <esp_timer.h>
#include <esp_system.h>
#include <cerrno>
#include <cstdlib>

namespace {
uint64_t sdRecordNow() {
    const int64_t us = esp_timer_get_time();
    return us < 0 ? UINT64_MAX : static_cast<uint64_t>(us) / 1000;
}
void sdRecordSession(FastBee::SdRecorderRuntime& runtime, uint64_t now) {
    runtime.storage = SDRecordingSession{};
    runtime.storage.token = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    // A collision with existing data is a hard failure; random IDs are never
    // treated as permission to truncate an unobserved historical file.
    if (!runtime.storage.token) runtime.storage.token = 1;
    runtime.queue.arm(now);
    runtime.waitingFresh = true;
    runtime.sampledAt = runtime.flushedAt = now;
}
void sdRecordFault(FastBee::SdRecorderRuntime& runtime, const char* code, bool permanent = false) {
    runtime.queue.fail(code);
    runtime.permanentFault = runtime.permanentFault || permanent;
}
bool sdRecordNumber(const ObservationSnapshot& observation, uint64_t now,
                    uint32_t maxAge, double& number) {
    if (!observation.hasValue || !observation.bindingValid || !observation.recordable ||
        observation.stale || observation.quality != ObservationQuality::GOOD ||
        observation.valueType != "number" || !observation.sampleIdentity ||
        observation.sourceRevision.isEmpty() || observation.sourceRevision.length() > 64 ||
        observation.unit.length() > 24 || observation.sampledAtMs > now ||
        now - observation.sampledAtMs > maxAge || observation.value.length() > 48 ||
        observation.value.length() != std::strlen(observation.value.c_str()) ||
        observation.sourceRevision.length() != std::strlen(observation.sourceRevision.c_str()) ||
        observation.unit.length() != std::strlen(observation.unit.c_str())) return false;
    const char* start = observation.value.c_str();
    if (!*start) return false;
    char* end = nullptr;
    errno = 0;
    number = std::strtod(start, &end);
    return errno != ERANGE && end != start && end && !*end && std::isfinite(number);
}
size_t sdRecordBatch(FastBee::SdRecorderRuntime& runtime, size_t& count) {
    size_t length = 0;
    count = 0;
    for (size_t i = 0; i < runtime.queue.size(); ++i) {
        const auto& sample = runtime.queue.at(i);
        JsonDocument doc;
        FastBee::setFixedJsonField(doc, "schema", 1);
        FastBee::setFixedJsonField(doc, "sourceRef", runtime.config.sourceRef);
        FastBee::setFixedJsonField(doc, "sourceRevision", runtime.sourceRevision);
        FastBee::setFixedJsonField(doc, "unit", runtime.unit);
        FastBee::setFixedJsonField(doc, "value", sample.value);
        FastBee::setFixedJsonField(doc, "quality", "good");
        FastBee::setFixedJsonField(doc, "sampleId", sample.identity);
        FastBee::setFixedJsonField(doc, "sampledAtMs", sample.sampledAtMs);
        FastBee::setFixedJsonField(doc, "recordedAtMs", sample.recordedAtMs);
        FastBee::setFixedJsonField(doc, "timeSource", sample.timeSource);
        FastBee::setFixedJsonField(doc, "timeQuality", sample.timeQuality);
        FastBee::setFixedJsonField(doc, "timeAuthenticated", sample.timeAuthenticated);
        FastBee::setFixedJsonField(doc, "timeSequence", sample.timeSequence);
        if (sample.wallKnown) FastBee::setFixedJsonField(doc, "wallEpochMs", sample.wallEpochMs);
        else FastBee::setFixedJsonField(doc, "wallEpochMs", nullptr);
        if (doc.overflowed()) return 0;
        const size_t bytes = measureJson(doc);
        if (bytes + 1 > runtime.batch.size() - length) break;
        if (serializeJson(doc, runtime.batch.data() + length, runtime.batch.size() - length) != bytes) return 0;
        length += bytes;
        runtime.batch[length++] = '\n';
        ++count;
    }
    return length;
}
String sdRecordJson(const FastBee::SdRecorderRuntime& runtime) {
    const uint64_t now = sdRecordNow();
    JsonDocument doc;
    FastBee::setFixedJsonField(doc, "ruleId", runtime.ruleId);
    FastBee::setFixedJsonField(doc, "targetId", runtime.targetId);
    FastBee::setFixedJsonField(doc, "sourceRef", runtime.config.sourceRef);
    FastBee::setFixedJsonField(doc, "phase", runtime.queue.fault() ? runtime.queue.fault() :
        runtime.waitingFresh ? "WAITING_FRESH_SAMPLE" : "RECORDING");
    FastBee::setFixedJsonField(doc, "pending", runtime.queue.size());
    FastBee::setFixedJsonField(doc, "accepted", runtime.queue.accepted);
    FastBee::setFixedJsonField(doc, "written", runtime.queue.written);
    FastBee::setFixedJsonField(doc, "dropped", runtime.queue.dropped);
    FastBee::setFixedJsonField(doc, "lastError", runtime.lastError);
    FastBee::setFixedJsonField(doc, "checkAgeMs", now >= runtime.checkedAt ? now - runtime.checkedAt : UINT64_MAX);
    FastBee::setFixedJsonField(doc, "writeQuality", "sdk_write_flush_only");
    FastBee::setFixedJsonField(doc, "fileSlot", runtime.storage.slot);
    FastBee::setFixedJsonField(doc, "fileCount", runtime.config.fileCount);
    if (doc.overflowed()) return "null";
    String json;
    const size_t expected = measureJson(doc);
    if (!json.reserve(expected) || serializeJson(doc, json) != expected || json.length() != expected) return "null";
    return json;
}
}

bool PeriphExecManager::sdRecorderSourceBindingLocked(const String& sourceRef, String& binding) const {
    binding = "";
    bool found = false;
    for (const auto& entry : rules) for (const auto& action : entry.second.actions) {
        if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) continue;
        JsonDocument doc;
        if (deserializeJson(doc, action.actionValue.c_str()) || !doc.is<JsonObject>()) return false;
        String id = doc["periphId"] | "";
        if (id.isEmpty()) id = action.targetPeriphId;
        const String requested = doc["channelField"] | "";
        const String field = requested.isEmpty() ? PeriphExecExecutor::resolveChannelField(
            doc["sensorCategory"] | "analog", id, doc["dataField"] | "") : requested;
        if (id + "_" + field != sourceRef) continue;
        PeripheralConfig source;
        if (found || !entry.second.enabled || !entry.second.recordHistory || action.useReceivedValue ||
            id != action.targetPeriphId ||
            !PeripheralManager::getInstance().getPeripheralSnapshot(id, source) || !source.enabled) return false;
        const size_t expected = entry.first.length() + id.length() + action.actionValue.length() + 2;
        const String fingerprint = entry.first + ":" + id + ":" + action.actionValue;
        if (fingerprint.length() != expected) return false;
        binding = fingerprint;
        if (binding != fingerprint) return false;
        found = true;
    }
    return found;
}

void PeriphExecManager::retireSdRecorderLocked() {
    if (!_sdRecorder) return;
    auto& runtime = *_sdRecorder;
    const auto current = rules.find(runtime.ruleId);
    if (_thermostatShutdown.load(std::memory_order_acquire) || current == rules.end() ||
        !current->second.enabled || !FastBee::hasSdRecorderAction(current->second) ||
        current->second.actions.size() != 1 ||
        current->second.actions[0].targetPeriphId != runtime.targetId ||
        current->second.actions[0].actionValue != runtime.configurationText) {
        // Closing is bounded and never writes a stale queued snapshot. All
        // files were closed after each batch; this owns no separate SPI lease.
        _sdRecorder.reset();
        return;
    }
    runtime.ruleRevision = _ruleRuntimeRevision;
    String binding;
    if (!sdRecorderSourceBindingLocked(runtime.config.sourceRef, binding) || binding != runtime.sourceBinding)
        sdRecordFault(runtime, "SD_RECORD_SOURCE_CHANGED", true);
    if (!runtime.queue.fault()) {
        const uint64_t now = sdRecordNow();
        if (now == UINT64_MAX) sdRecordFault(runtime, "SD_RECORD_CLOCK_INVALID", true);
        else { runtime.queue.arm(now); runtime.waitingFresh = true; }
    }
}

void PeriphExecManager::checkSdRecorder() {
    if (!_rulesMutex) return;
    MutexGuard rulesLock(_rulesMutex, 0);
    if (!rulesLock.isLocked()) return;
    const uint64_t now = sdRecordNow();
    if (now == UINT64_MAX) {
        if (_sdRecorder) sdRecordFault(*_sdRecorder, "SD_RECORD_CLOCK_INVALID", true);
        return;
    }
    if (_thermostatShutdown.load(std::memory_order_acquire)) { _sdRecorder.reset(); return; }
    if (_configurationLoadFailed || _configurationRestartPending || _ruleRuntimeRevision == UINT64_MAX) {
        if (_sdRecorder) sdRecordFault(*_sdRecorder, "SD_RECORD_CONFIGURATION_UNSAFE", true);
        return;
    }
    if (FastBee::MemoryBudget::dram8Free() < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM) {
        if (_sdRecorder) sdRecordFault(*_sdRecorder, "SD_RECORD_MEMORY_SUSPENDED");
        return;
    }
    if (!_sdRecorder) {
        if (FastBee::MemoryBudget::dram8Free() < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM +
            sizeof(FastBee::SdRecorderRuntime) + 4096) return;
        const PeriphExecRule* selected = nullptr;
        for (const auto& entry : rules) if (entry.second.enabled && FastBee::hasSdRecorderAction(entry.second)) {
            // Imported contradictory configurations never pick an arbitrary
            // winner. A single configured SD mount has exactly one recorder.
            if (selected) return;
            selected = &entry.second;
        }
        if (!selected) return;
        String error;
        if (!FastBee::validateSdRecorderRule(*selected, error)) return;
        std::unique_ptr<FastBee::SdRecorderRuntime> candidate(new (std::nothrow) FastBee::SdRecorderRuntime());
        if (!candidate) return;
        candidate->ruleId = selected->id;
        candidate->targetId = selected->actions[0].targetPeriphId;
        candidate->configurationText = selected->actions[0].actionValue;
        candidate->ruleRevision = selected->runtimeRevision;
        if (candidate->ruleId != selected->id || candidate->targetId != selected->actions[0].targetPeriphId ||
            candidate->configurationText != selected->actions[0].actionValue ||
            !FastBee::parseSdRecorderConfig(selected->actions[0].actionValue, candidate->config, error)) return;
        candidate->sourceConfigEpoch = FastBee::SensorConfigEpoch::current();
        sdRecordSession(*candidate, now);
        if (!sdRecorderSourceBindingLocked(candidate->config.sourceRef, candidate->sourceBinding))
            sdRecordFault(*candidate, "SD_RECORD_SOURCE_UNBOUND", true);
        _sdRecorder = std::move(candidate);
    }
    auto& runtime = *_sdRecorder;
    const auto current = rules.find(runtime.ruleId);
    if (current == rules.end() || !current->second.enabled || current->second.runtimeRevision != runtime.ruleRevision) {
        _sdRecorder.reset(); return;
    }
    if (now < runtime.checkedAt) { sdRecordFault(runtime, "SD_RECORD_CLOCK_INVALID", true); return; }
    runtime.checkedAt = now;
    if (!FastBee::SensorConfigEpoch::matches(runtime.sourceConfigEpoch))
        sdRecordFault(runtime, "SD_RECORD_SOURCE_CHANGED", true);

    auto& peripherals = PeripheralManager::getInstance();
    RecursiveMutexGuard peripheralLock(peripherals.getMutex(), 0);
    if (!peripheralLock.isLocked()) return;
    PeripheralConfig config;
    PeripheralRuntimeState state;
    auto& storage = SDCardManager::getInstance();
    const SDCardStatus status = storage.getStatus();
    if (!status.statusAvailable && !status.configurationAvailable) return;
    if (!peripherals.getPeripheralSnapshot(runtime.targetId, config) || config.type != PeripheralType::SDIO ||
        !config.enabled || !peripherals.getRuntimeStateSnapshot(runtime.targetId, state) || state.memorySuspended ||
        !status.configured || status.ownerId != runtime.targetId || config.pinCount != 4 ||
        status.sckPin != config.pins[0] || status.mosiPin != config.pins[1] ||
        status.misoPin != config.pins[2] || status.csPin != config.pins[3] ||
        status.frequency != config.params.sdcard.frequency) {
        sdRecordFault(runtime, "SD_RECORD_TARGET_CHANGED"); return;
    }
    if (!status.statusAvailable) {
        if (status.recoverySuggested) sdRecordFault(runtime, "SD_RECORD_STORAGE_UNAVAILABLE");
        return; // Busy observation never initiates card I/O.
    }
    if (!runtime.storageGeneration) {
        runtime.storageGeneration = status.recordingGeneration;
        runtime.recoveryGeneration = status.recordingRecovery;
    } else if (status.recordingRecovery != runtime.recoveryGeneration && status.mounted) {
        runtime.storageGeneration = status.recordingGeneration;
        runtime.recoveryGeneration = status.recordingRecovery;
        if (!runtime.permanentFault) sdRecordSession(runtime, now);
    } else if (status.recordingGeneration != runtime.storageGeneration) {
        sdRecordFault(runtime, "SD_RECORD_STORAGE_CHANGED");
    }
    if (!status.mounted || status.recoverySuggested) sdRecordFault(runtime, "SD_RECORD_STORAGE_UNAVAILABLE");
    if (runtime.queue.fault()) return;

    if (now - runtime.sampledAt >= runtime.config.sampleMs) {
        runtime.sampledAt = now;
        ObservationSnapshot observation;
        double value = 0;
        if (getObservation(runtime.config.sourceRef, observation) &&
            sdRecordNumber(observation, now, runtime.config.maxAgeMs, value)) {
            if (runtime.sourceRevision.isEmpty()) {
                runtime.sourceRevision = observation.sourceRevision;
                runtime.unit = observation.unit;
                if (runtime.sourceRevision != observation.sourceRevision || runtime.unit != observation.unit) {
                    sdRecordFault(runtime, "SD_RECORD_NO_MEMORY"); return;
                }
            } else if (runtime.sourceRevision != observation.sourceRevision || runtime.unit != observation.unit) {
                sdRecordFault(runtime, "SD_RECORD_SOURCE_CHANGED", true); return;
            }
            FastBee::SdRecordedSample sample;
            sample.value = value; sample.identity = observation.sampleIdentity;
            sample.sampledAtMs = observation.sampledAtMs; sample.recordedAtMs = now;
            const auto time = FastBee::SystemTimeQuality::snapshot();
            sample.wallKnown = time.timeValid && time.synced;
            sample.wallEpochMs = sample.wallKnown ? time.epochMs : 0;
            sample.timeSequence = time.sequence; sample.timeSource = time.source;
            sample.timeQuality = time.quality; sample.timeAuthenticated = time.transportAuthenticated;
            if (runtime.queue.enqueue(sample)) runtime.waitingFresh = false;
        }
    }
    if (!runtime.queue.size() || (runtime.queue.size() < FastBee::SdRecorderBuffer::CAPACITY &&
        now - runtime.flushedAt < runtime.config.flushMs)) return;
    size_t count = 0;
    const size_t length = sdRecordBatch(runtime, count);
    if (!length || !count) { sdRecordFault(runtime, "SD_RECORD_SERIALIZATION_FAILED"); return; }
    String error;
    bool ioAttempted = false;
    if (!storage.appendRecording(runtime.targetId, runtime.storageGeneration, runtime.storage,
            reinterpret_cast<const uint8_t*>(runtime.batch.data()), length,
            runtime.config.fileBytes, runtime.config.fileCount, error, &ioAttempted)) {
        // These two codes are produced only before any SD mutation. Keeping
        // pending rows after failed admission is not a write retry/replay.
        if (!ioAttempted && (error == "SD_BUSY" || error == "SPI_BUSY")) return;
        runtime.lastError = error;
        sdRecordFault(runtime, "SD_RECORD_WRITE_FAILED"); return;
    }
    runtime.queue.acknowledge(count);
    runtime.flushedAt = now;
}

bool PeriphExecManager::getSdRecorderStatus(const PeriphExecRule& rule, ActionExecResult& result) {
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(20));
    if (!lock.isLocked()) { result.remark = "SD_RECORD_STATUS_BUSY"; return false; }
    const auto current = rules.find(rule.id);
    if (current == rules.end() || !current->second.enabled || current->second.runtimeRevision != rule.runtimeRevision ||
        !_sdRecorder || _sdRecorder->ruleId != rule.id || _sdRecorder->ruleRevision != rule.runtimeRevision) {
        result.remark = "SD_RECORD_NOT_ADMITTED"; return false;
    }
    result.targetPeriphId = _sdRecorder->targetId;
    result.actualValue = sdRecordJson(*_sdRecorder);
    result.remark = _sdRecorder->queue.fault() ? _sdRecorder->queue.fault() : "sd_record_status_only";
    result.success = result.actualValue != "null" && !_sdRecorder->queue.fault();
    return result.success;
}

String PeriphExecManager::getSdRecorderDiagnosticsJson() const {
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(20));
    if (!lock.isLocked() || !_sdRecorder) return "null";
    return sdRecordJson(*_sdRecorder);
}
#endif
