#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/NativeOutputDispatch.h"
#if FASTBEE_ENABLE_SEVEN_SEGMENT
#include "peripherals/SevenSegmentDriver.h"
#endif
#if FASTBEE_ENABLE_LCD
#include "peripherals/LCDManager.h"
#endif
#include "core/SensorConfigEpoch.h"
#include <cstring>

// Named synchronous displays share the same PM actor/operation/hold policy as
// other outputs. PM -> driver is the only lock order; no queued display producer.
bool PeripheralManager::submitOrdinaryDisplay(const String& id,
    FastBee::DisplayOutputCommand command, const String& text, int32_t value,
    const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* aggregate) {
    namespace OC = FastBee::OrdinaryControl;
    using Command = FastBee::DisplayOutputCommand;
    FastBee::OrdinaryControlResult result;
    result.evaluated = true;
    if (id.length() <= 40) memcpy(result.targetId, id.c_str(), id.length() + 1);
    auto finish = [&]() {
        FastBee::rememberOrdinaryControlResult(result, aggregate);
        return result.accepted;
    };
    if (!context) { result.reason = "CONTROL_CONTEXT_REQUIRED"; return finish(); }
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return finish(); }
    const auto config = peripherals.find(id);
    if (config == peripherals.end() ||
        config->second.type != PeripheralType::SEVEN_SEGMENT_TM1637 ||
        config->second.pinCount != 2 || config->second.pins[0] == config->second.pins[1] ||
        config->second.isModbusPeripheral() || config->second.isExpanderChannel()) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return finish();
    }
    const bool textual = command == Command::Number || command == Command::Text;
    if (text.length() > 1024 || text.length() != strlen(text.c_str()) ||
        (textual && value != 0) || (!textual && !text.isEmpty()) ||
        (command == Command::Brightness && (value < 0 || value > 7)) ||
        (command == Command::Clear && value != 0) ||
        (!textual && command != Command::Clear && command != Command::Brightness)) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return finish();
    }
#if FASTBEE_ENABLE_SEVEN_SEGMENT
    uint64_t now = 0;
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, OC::Intent::Write,
        result, now, false, permit)) return finish();
    // Config cannot be replaced until this bounded four-digit frame returns.
    const bool retirementAttempted = result.attempted;
    const auto transport = SevenSegmentDriver::instance().write(id,
        config->second.pins[0], config->second.pins[1], command, text, value);
    completeOrdinaryControlLocked(id, now, result, transport.acknowledged, transport.reason);
    result.attempted = retirementAttempted || transport.attempted;
    if (transport.acknowledged) result.reason = "CONTROL_DISPLAY_TRANSPORT_ACKNOWLEDGED";
    return finish();
#else
    result.reason = "CONTROL_TARGET_NOT_COMPILED"; return finish();
#endif
}

bool PeripheralManager::ordinaryDisplayQuiescentLocked(const String& id) const {
    const auto config = peripherals.find(id);
    if (config == peripherals.end()) return false;
#if FASTBEE_ENABLE_SEVEN_SEGMENT
    if (config->second.type == PeripheralType::SEVEN_SEGMENT_TM1637)
        return SevenSegmentDriver::instance().quiescent(id);
#endif
#if FASTBEE_ENABLE_LCD
    if (config->second.type == PeripheralType::LCD)
        return LCDManager::getInstance().quiescent(config->second);
#endif
    return false;
}

bool PeripheralManager::submitOrdinaryLcd(const String& id,
    const FastBee::LcdOutputRequest& output, const FastBee::OrdinaryControlContext* context,
    FastBee::OrdinaryControlResult* aggregate) {
    namespace OC = FastBee::OrdinaryControl;
    using Command = FastBee::LcdOutputCommand;
    FastBee::OrdinaryControlResult result;
    result.evaluated = true;
    if (id.length() <= 40) memcpy(result.targetId, id.c_str(), id.length() + 1);
    auto finish = [&]() { FastBee::rememberOrdinaryControlResult(result, aggregate); return result.accepted; };
    if (!context) { result.reason = "CONTROL_CONTEXT_REQUIRED"; return finish(); }
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return finish(); }
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || config->second.type != PeripheralType::LCD ||
        config->second.pinCount != 2 || config->second.pins[0] == config->second.pins[1] ||
        config->second.params.lcd.interface != 2 || config->second.isModbusPeripheral() ||
        config->second.isExpanderChannel()) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return finish();
    }
    const bool textual = output.command == Command::CustomText || output.command == Command::TextAt ||
        output.command == Command::TextLine;
    if (output.text.length() > 1024 || output.text.length() != strlen(output.text.c_str()) ||
        (!textual && !output.text.isEmpty()) ||
        static_cast<uint8_t>(output.command) > static_cast<uint8_t>(Command::Font) ||
        (output.command == Command::SensorPage && (output.value < -1 || output.value > 127)) ||
        (output.command == Command::TextLine && (output.value < 0 || output.value > 255)) ||
        (output.command == Command::Font && (output.value < 0 || output.value > 2)) ||
        (output.command == Command::TextAt && (output.align < 1 || output.align > 3))) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return finish();
    }
#if FASTBEE_ENABLE_LCD
    uint64_t now = 0;
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, OC::Intent::Write, result, now, false, permit))
        return finish();
    const bool retirementAttempted = result.attempted;
    const auto transport = LCDManager::getInstance().write(config->second, output);
    completeOrdinaryControlLocked(id, now, result, transport.acknowledged, transport.reason);
    result.attempted = retirementAttempted || transport.attempted;
    if (transport.acknowledged) result.reason = transport.attempted
        ? "CONTROL_DISPLAY_TRANSPORT_ACKNOWLEDGED" : "CONTROL_DISPLAY_STATE_UPDATED";
    return finish();
#else
    result.reason = "CONTROL_TARGET_NOT_COMPILED"; return finish();
#endif
}

bool PeripheralManager::getActiveLcdTarget(String& id, uint32_t& configEpoch) const {
    id = ""; configEpoch = 0;
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    for (const auto& item : peripherals) {
        if (item.second.type != PeripheralType::LCD || !item.second.enabled) continue;
        if (!id.isEmpty()) { id = ""; return false; }
        id = item.first;
    }
    if (id.isEmpty()) return false;
    configEpoch = FastBee::SensorConfigEpoch::current();
    return configEpoch != 0;
}

void PeripheralManager::performLcdMaintenance() {
#if FASTBEE_ENABLE_LCD
    // The existing maintenance producer may re-send only an acknowledged
    // foreground buffer, only when its named display has no active grant.
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return;
    for (const auto& item : peripherals) {
        if (item.second.type != PeripheralType::LCD || !item.second.enabled) continue;
        const auto runtime = runtimeStates.find(item.first);
        if (runtime == runtimeStates.end() ||
            runtime->second.ordinaryControl.phase != FastBee::OrdinaryControl::Phase::Idle) continue;
        LCDManager::getInstance().performMaintenance(item.second);
    }
#endif
}
