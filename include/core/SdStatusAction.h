#pragma once
#include "core/PeripheralExecution.h"

namespace FastBee {
inline const char* validateSdStatusAction(const ExecAction& action) {
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS))
        return "SD_STATUS_ACTION_TYPE_INVALID";
    if (action.targetPeriphId.isEmpty() || !action.actionValue.isEmpty() || action.useReceivedValue)
        return "SD_STATUS_ACTION_INVALID: select an SD target and no value";
    return nullptr;
}
} // namespace FastBee
