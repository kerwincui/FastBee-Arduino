#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecExecutor.h"
#include "core/SdStatusAction.h"
#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "peripherals/SDCardManager.h"
#include <ArduinoJson.h>

bool PeriphExecExecutor::executeSdStatusAction(const ExecAction& action, ActionExecResult& result) {
    result = ActionExecResult{};
    const auto fail = [&](const char* reason) { result.remark = reason; return false; };
    if (!FastBee::captureLiteralReportIdentity(result, action.targetPeriphId.c_str()))
        return fail("SD_REPORT_IDENTITY_FAILED");
    if (const char* reason = FastBee::validateSdStatusAction(action)) return fail(reason);
    auto& manager = PeripheralManager::getInstance();
    // Keep target identity/lifecycle stable through the passive snapshot. No
    // card mount, FAT query, filesystem transaction or waiting for the bus.
    RecursiveMutexGuard lock(manager.getMutex(), 0);
    if (!lock.isLocked()) return fail("SD_STATUS_BUSY");
    PeripheralConfig config;
    PeripheralRuntimeState runtime;
    if (!manager.getPeripheralSnapshot(action.targetPeriphId, config) || config.type != PeripheralType::SDIO)
        return fail("SD_STATUS_TARGET_INVALID");
    if (!config.enabled || !manager.getRuntimeStateSnapshot(action.targetPeriphId, runtime) || runtime.memorySuspended)
        return fail("SD_STATUS_NOT_READY");
    const auto status = SDCardManager::getInstance().getStatus();
    if (!status.statusAvailable) return fail("SD_STATUS_UNAVAILABLE");
    if (!status.configured || status.ownerId != action.targetPeriphId) return fail("SD_STATUS_OWNER_MISMATCH");
    if (config.pinCount != 4 || status.sckPin != config.pins[0] || status.mosiPin != config.pins[1] ||
        status.misoPin != config.pins[2] || status.csPin != config.pins[3] ||
        status.frequency != config.params.sdcard.frequency) return fail("SD_STATUS_CONFIG_CHANGED");
    JsonDocument document;
    document["schema"] = 1;
    document["mounted"] = status.mounted;
    document["quality"] = "lifecycle_snapshot";
    document["recoverySuggested"] = status.recoverySuggested;
    document["ioFailures"] = status.ioFailures;
    document["recoveries"] = status.recoveries;
    document["capacityQuality"] = "unavailable";
    if (status.mounted && status.capacityValid && status.totalBytes > 0 && status.usedBytes <= status.totalBytes) {
        document["capacityQuality"] = "last_operation_snapshot";
        document["totalBytes"] = status.totalBytes;
        document["usedBytes"] = status.usedBytes;
        document["freeBytes"] = status.totalBytes - status.usedBytes;
        document["capacityAgeMs"] = status.capacityAgeMs;
        document["capacitySampledAtMs"] = status.capacitySampledAtMs;
    }
    if (document.overflowed()) return fail("SD_STATUS_SERIALIZATION_FAILED");
    const size_t bytes = measureJson(document);
    if (!result.actualValue.reserve(bytes)) return fail("SD_STATUS_SERIALIZATION_FAILED");
    if (serializeJson(document, result.actualValue) != bytes) {
        result.actualValue = ""; return fail("SD_STATUS_SERIALIZATION_FAILED");
    }
    result.remark = "sd_passive_snapshot";
    result.success = true;
    return true;
}
#endif
