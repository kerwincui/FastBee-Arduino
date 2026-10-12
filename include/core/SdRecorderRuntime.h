#pragma once
#include "core/SdRecorderConfig.h"
#include "core/SdRecorderBuffer.h"
#include "peripherals/SDCardManager.h"

namespace FastBee {
struct SdRecorderRuntime {
    String ruleId, targetId, configurationText, sourceRevision, sourceBinding, unit, lastError;
    SdRecorderConfig config;
    SdRecorderBuffer queue;
    SDRecordingSession storage;
    uint64_t ruleRevision = 0, storageGeneration = 0, recoveryGeneration = 0;
    uint64_t sampledAt = 0, flushedAt = 0, checkedAt = 0;
    uint32_t sourceConfigEpoch = 0;
    bool permanentFault = false;
    bool waitingFresh = true;
    std::array<char, 2048> batch{};
};
} // namespace FastBee
