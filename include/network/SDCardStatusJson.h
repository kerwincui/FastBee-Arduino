#pragma once
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "peripherals/SDCardManager.h"

namespace FastBee {

// Both SD status endpoints use the same ownership/unknown-value contract.
inline void writeSdCardStatusJson(JsonObject data, const SDCardStatus& status, const String& ownerId) {
    for (const char* key : {"configured", "mounted", "cardType", "cardBytes", "totalBytes",
                            "usedBytes", "capacityAgeMs", "driverError", "recoverySuggested",
                            "ioFailures", "recoveryAttempts", "recoveries", "lastFailureCode"}) data.remove(key);
    FastBee::setFixedJsonField(data, "statusAvailable", status.statusAvailable);
    FastBee::setFixedJsonField(data, "configurationAvailable", status.statusAvailable || status.configurationAvailable);
    FastBee::setFixedJsonField(data, "capacityQuality", "unavailable");
    if (!status.statusAvailable && !status.configurationAvailable) {
        if (!status.lastError.isEmpty()) FastBee::setFixedJsonField(data, "driverError", status.lastError.c_str());
        return;
    }
    const bool owns = status.configured && status.ownerId == ownerId;
    FastBee::setFixedJsonField(data, "configured", owns);
    if (status.statusAvailable) FastBee::setFixedJsonField(data, "mounted", owns && status.mounted);
    if (!owns) {
        if (status.configured) FastBee::setFixedJsonField(data, "driverError", "SD_OWNER_MISMATCH: another peripheral owns storage");
        return;
    }
    if (!status.lastError.isEmpty()) FastBee::setFixedJsonField(data, "driverError", status.lastError.c_str());
    FastBee::setFixedJsonField(data, "recoverySuggested", status.recoverySuggested);
    FastBee::setFixedJsonField(data, "ioFailures", status.ioFailures);
    FastBee::setFixedJsonField(data, "recoveryAttempts", status.recoveryAttempts);
    FastBee::setFixedJsonField(data, "recoveries", status.recoveries);
    FastBee::setFixedJsonField(data, "lastFailureCode", status.lastFailureCode.c_str());
    if (!status.statusAvailable || !status.mounted || !status.capacityValid) return;
    FastBee::setFixedJsonField(data, "cardType", status.cardType.c_str());
    FastBee::setFixedJsonField(data, "cardBytes", status.cardBytes);
    FastBee::setFixedJsonField(data, "totalBytes", status.totalBytes);
    FastBee::setFixedJsonField(data, "usedBytes", status.usedBytes);
    FastBee::setFixedJsonField(data, "capacityQuality", "last_operation_snapshot");
    FastBee::setFixedJsonField(data, "capacityAgeMs", status.capacityAgeMs);
}

} // namespace FastBee
