#pragma once
#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace FastBee {
// Identities belong to the observation, never to configuration found later
// during serialization. Legacy is retained only to reject old/unbound callers.
enum class ActionReportIdentity : uint8_t { Legacy, Invalid, Peripheral, Field };

template<class Result>
bool captureLiteralReportIdentity(Result& result, const char* peripheral, const char* field = "") {
    result.reportIdentity = ActionReportIdentity::Invalid;
    result.reportIdBytes = 0;
    if (!peripheral || !field) return false;
    const size_t peripheralBytes = strlen(peripheral), fieldBytes = strlen(field);
    if (!peripheralBytes || peripheralBytes > 512 || fieldBytes > 512) return false;
    result.targetPeriphId = peripheral;
    result.dataField = field;
    if (result.targetPeriphId.length() != peripheralBytes || result.dataField.length() != fieldBytes)
        return false;
    result.reportIdentity = fieldBytes ? ActionReportIdentity::Field : ActionReportIdentity::Peripheral;
    result.reportIdBytes = static_cast<uint16_t>(fieldBytes ? fieldBytes : peripheralBytes);
    return true;
}

// Use the same sensorId[_chN] convention as ModbusHandler, from the device
// snapshot held BEFORE I/O. Never read the current handler during reporting.
template<class Result, size_t N>
bool captureModbusReportIdentity(Result& result, const char* peripheral,
                                 const char (&sensorId)[N], uint8_t channels, uint16_t channel) {
    result.reportIdentity = ActionReportIdentity::Invalid;
    result.reportIdBytes = 0;
    const char* end = static_cast<const char*>(std::memchr(sensorId, '\0', N));
    if (!end) return false;
    if (end == sensorId) return captureLiteralReportIdentity(result, peripheral);
    char mapped[N + 9];
    const int size = channels > 1
        ? std::snprintf(mapped, sizeof(mapped), "%s_ch%u", sensorId, static_cast<unsigned>(channel))
        : std::snprintf(mapped, sizeof(mapped), "%s", sensorId);
    return size > 0 && static_cast<size_t>(size) < sizeof(mapped) &&
        captureLiteralReportIdentity(result, peripheral, mapped);
}

// Parse only a complete unsigned channel prefix. This is report formatting,
// not permission to execute a command or write a register.
inline bool splitModbusReportValue(const String& value, uint16_t& channel, size_t& offset) {
    channel = 0;
    offset = 0;
    if (std::strlen(value.c_str()) != value.length()) return false;
    const char* colon = std::strchr(value.c_str(), ':');
    if (!colon) return true;
    if (colon == value.c_str() || !colon[1]) return false;
    uint32_t number = 0;
    for (const char* at = value.c_str(); at < colon; ++at) {
        if (*at < '0' || *at > '9') return false;
        number = number * 10 + static_cast<unsigned>(*at - '0');
        if (number > UINT16_MAX) return false;
    }
    channel = static_cast<uint16_t>(number);
    offset = static_cast<size_t>(colon - value.c_str()) + 1;
    return true;
}

template<class Result>
void completeCapturedReport(Result& result, const String& value, bool success, const String& remark) {
    result.actualValue = value;
    result.remark = remark;
    result.success = success;
    if (result.actualValue.length() != value.length() || result.actualValue != value ||
        result.remark.length() != remark.length() || result.remark != remark ||
        std::strlen(value.c_str()) != value.length() || std::strlen(remark.c_str()) != remark.length()) {
        result.reportIdentity = ActionReportIdentity::Invalid;
        result.reportIdBytes = 0;
    }
}

// Keep the selected identity kind even if a later String copy loses its data;
// never silently fall back from a field ID to a different peripheral ID.
template<class Result>
const String* capturedReportId(const Result& result) {
    const String* id = result.reportIdentity == ActionReportIdentity::Field ? &result.dataField :
        result.reportIdentity == ActionReportIdentity::Peripheral ? &result.targetPeriphId : nullptr;
    return id && result.reportIdBytes && id->length() == result.reportIdBytes &&
        strlen(id->c_str()) == result.reportIdBytes ? id : nullptr;
}
} // namespace FastBee
