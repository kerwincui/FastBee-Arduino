#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include "core/PeripheralExecution.h"
#include "core/SdRecorderConfigMetadata.h"

namespace FastBee {
struct SdRecorderConfig {
    String sourceRef;
    uint32_t sampleMs = 5000, maxAgeMs = 30000, flushMs = 10000;
    uint16_t fileBytes = 16384;
    uint8_t fileCount = 4;
};

inline bool parseSdRecorderConfig(const String& text, SdRecorderConfig& out, String& error) {
    const auto fail = [&](const char* reason) { error = reason; return false; };
    if (text.isEmpty() || text.length() > 512 || text.length() != std::strlen(text.c_str()))
        return fail("SD_RECORD_CONFIG_SIZE");
    JsonDocument doc;
    if (deserializeJson(doc, text.c_str()) || !doc.is<JsonObject>() || doc.size() != 6)
        return fail("SD_RECORD_CONFIG_FIELDS");
    const JsonString source = doc["sourceRef"].as<JsonString>();
    if (!source.c_str() || !source.size() || source.size() > 96 || source.size() != std::strlen(source.c_str()))
        return fail("SD_RECORD_SOURCE_INVALID");
    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source.c_str()[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.' || c == ':')) return fail("SD_RECORD_SOURCE_INVALID");
    }
    for (const auto& bound : kSdRecorderNumbers) {
        const auto value = doc[bound.key];
        if (value.is<bool>() || !value.is<uint32_t>() || value.as<uint32_t>() < bound.min ||
            value.as<uint32_t>() > bound.max) return fail("SD_RECORD_CONFIG_RANGE");
    }
    SdRecorderConfig candidate;
    candidate.sourceRef = source.c_str();
    if (candidate.sourceRef.length() != source.size()) return fail("SD_RECORD_NO_MEMORY");
    candidate.sampleMs = doc["sampleSec"].as<uint32_t>() * 1000U;
    candidate.maxAgeMs = doc["maxAgeSec"].as<uint32_t>() * 1000U;
    candidate.flushMs = doc["flushSec"].as<uint32_t>() * 1000U;
    candidate.fileBytes = doc["fileKiB"].as<uint16_t>() * 1024U;
    candidate.fileCount = doc["fileCount"].as<uint8_t>();
    out = candidate;
    if (out.sourceRef != candidate.sourceRef) return fail("SD_RECORD_NO_MEMORY");
    error = "";
    return true;
}

inline bool hasSdRecorderAction(const PeriphExecRule& rule) {
    for (const auto& action : rule.actions)
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_RECORD)) return true;
    return false;
}

inline bool validateSdRecorderRule(const PeriphExecRule& rule, String& error) {
    if (!hasSdRecorderAction(rule)) return true;
    if (rule.actions.size() != 1 || rule.triggers.size() != 1 ||
        rule.triggers[0].triggerType != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) ||
        rule.triggers[0].timerMode != static_cast<uint8_t>(ExecTimerMode::INTERVAL) ||
        rule.triggers[0].intervalSec != 1 || rule.allowPanelControl || rule.recordHistory ||
        !rule.scriptContent.isEmpty()) {
        error = "SD_RECORD_STANDALONE_TIMER_REQUIRED"; return false;
    }
    const auto& action = rule.actions[0];
    if (action.targetPeriphId.isEmpty() || action.useReceivedValue || action.syncDelayMs) {
        error = "SD_RECORD_LITERAL_TARGET_REQUIRED"; return false;
    }
    SdRecorderConfig config;
    return parseSdRecorderConfig(action.actionValue, config, error);
}
} // namespace FastBee
