#pragma once

#include "core/FeatureFlags.h"
#include "core/PeripheralExecution.h"

namespace FastBee {
inline bool isRtcAction(uint8_t type) {
    return type == static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL) ||
           type == static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ) ||
           type == static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET);
}
// Shared by preflight, CRUD/import admission and the actual executor. No
// template substitution or implicit default may turn malformed input into I/O.
inline const char* validateRtcAction(const ExecAction& action) {
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ)) {
        if (action.targetPeriphId.isEmpty() || action.useReceivedValue || !action.actionValue.isEmpty())
            return "DS1302_READ_ACTION_INVALID: select a target and no value";
    } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET)) {
        if (action.targetPeriphId.isEmpty() || action.useReceivedValue || action.actionValue != "system-local")
            return "DS1302_SET_ACTION_INVALID: explicitly select system-local";
    } else {
        if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL))
            return "DS1302_ACTION_TYPE_INVALID";
        if (action.targetPeriphId.isEmpty() || action.useReceivedValue ||
            (action.actionValue != "start" && action.actionValue != "stop")) {
            return "DS1302_ACTION_INVALID: select a target and literal start or stop";
        }
    }
#if !FASTBEE_ENABLE_DS1302
    return "DS1302_NOT_COMPILED";
#else
    return nullptr;
#endif
}
} // namespace FastBee
