#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecExecutor.h"
#include "core/PeripheralManager.h"
#include "core/UartAction.h"

bool PeriphExecExecutor::resolveUartActionTarget(const ExecAction& action,
    uint32_t expectedConfigEpoch, bool& isUart, uint32_t& configEpoch, ActionExecResult* detail) {
    isUart = false; configEpoch = 0;
    if (detail) *detail = ActionExecResult{};
    const auto fail = [&](const char* reason) {
        if (detail) detail->remark = reason;
        return false;
    };
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_UART_SEND) &&
        action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL))
        return fail("UART_ACTION_INVALID");
    auto& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard guard(manager.getMutex(), 0);
    if (!guard.isLocked()) return fail("UART_OR_CALL_BUSY");
    PeripheralConfig declared;
    uint32_t version = 0;
    if (!manager.getPeripheralSnapshot(action.targetPeriphId, declared, &version))
        return fail("UART_OR_CALL_TARGET_INVALID");
    // Check BEFORE generic-call routing: changing a queued UART target into
    // GPIO must not turn an old send into a newly configured physical action.
    if (expectedConfigEpoch && version != expectedConfigEpoch)
        return fail("UART_OR_CALL_CONFIG_CHANGED");
    isUart = action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_UART_SEND) ||
             declared.type == PeripheralType::UART;
    configEpoch = version;
    return true;
}

bool PeriphExecExecutor::executeUartSendAction(const ExecAction& action,
    const String& effectiveValue, const String& receivedValue,
    uint32_t expectedConfigVersion, ActionExecResult& result) {
    result = ActionExecResult{};
    result.targetPeriphId = action.targetPeriphId;
    const auto fail = [&](const char* reason) { result.remark = reason; return false; };
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_UART_SEND) &&
        action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL))
        return fail("UART_ACTION_INVALID");
    if (action.targetPeriphId.isEmpty()) return fail("UART_TARGET_INVALID");
    String payload;
    if (const char* error = FastBee::buildUartActionPayload(action, effectiveValue, receivedValue, payload))
        return fail(error);
    auto& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard guard(manager.getMutex(), 0);
    if (!guard.isLocked()) return fail("UART_BUSY");
    PeripheralConfig current;
    uint32_t currentVersion = 0;
    if (!manager.getPeripheralSnapshot(action.targetPeriphId, current, &currentVersion) ||
        currentVersion != expectedConfigVersion ||
        current.type != PeripheralType::UART) return fail("UART_CONFIG_CHANGED");
    PeripheralRuntimeState runtime;
    if (!current.enabled || !manager.getRuntimeStateSnapshot(action.targetPeriphId, runtime) ||
        runtime.memorySuspended || !runtime.state.comm.isConnected)
        return fail("UART_NOT_READY");
    // Exactly one local write. A false return may already have sent bytes; no
    // retry, RX consumption, peer verification or payload in the result/trace.
    if (!manager.writeString(action.targetPeriphId, payload)) return fail("UART_WRITE_UNCONFIRMED");
    result.success = true;
    result.remark = "UART_LOCAL_ACCEPTED_PEER_UNVERIFIED";
    return true;
}
#endif
