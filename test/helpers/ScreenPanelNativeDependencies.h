#pragma once

#include <stdexcept>
#include <Arduino.h>
#include <ArduinoJson.h>

class HardwareSerial;
struct portMUX_TYPE;
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// Native builds disable ArduinoJson's Arduino adapter. Match its String
// conversion through the enabled std::string adapter for this production TU.
inline void convertFromJson(JsonVariantConst source, String& destination) {
    destination = source.as<std::string>();
}

#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "systems/SystemRebooter.h"
#include "screen/ScreenRuntime.h"

// The schema validator must remain independent of live peripherals and actions.
// Compile the complete manager against real declarations, but fail immediately
// if a validator test unexpectedly crosses into one of those runtime services.
[[noreturn]] inline void unexpectedPanelRuntimeDependency() {
    throw std::logic_error("Screen-panel validator touched a runtime dependency");
}

inline bool& allowPanelNativeRuntimeDependencies() {
    static bool allowed = false;
    return allowed;
}

inline FastBee::Screen::ScreenSystemActionCapabilities&
panelNativeSystemActionCapabilities() {
    static FastBee::Screen::ScreenSystemActionCapabilities capabilities{};
    return capabilities;
}

// The production manager also resolves system cards through framework/network
// telemetry. Schema validation must not construct these device services.
#define FAST_BEE_FRAMEWORK_H
#define NETWORK_MANAGER_H
enum class NetworkStatus { CONNECTED, DISCONNECTED, CONNECTING, AP_MODE, CONNECTION_FAILED };
struct PanelNativeNetworkStatus { NetworkStatus status; };
class FBNetworkManager {
public:
    PanelNativeNetworkStatus getStatusInfo() const { unexpectedPanelRuntimeDependency(); }
};
class FastBeeFramework {
public:
    static FastBeeFramework* getInstance() { unexpectedPanelRuntimeDependency(); }
    FBNetworkManager* getNetworkManager() const { unexpectedPanelRuntimeDependency(); }
};

inline FastBee::Screen::ScreenRuntime& FastBee::Screen::ScreenRuntime::instance() {
    if (!allowPanelNativeRuntimeDependencies()) {
        unexpectedPanelRuntimeDependency();
    }
    static ScreenRuntime runtime;
    return runtime;
}
inline bool FastBee::Screen::ScreenRuntime::stop(String*) {
    unexpectedPanelRuntimeDependency();
}
inline bool FastBee::Screen::ScreenRuntime::running() const {
    unexpectedPanelRuntimeDependency();
}
inline bool FastBee::Screen::ScreenRuntime::sleeping() const {
    unexpectedPanelRuntimeDependency();
}
inline size_t FastBee::Screen::ScreenRuntime::allocatedBytes() const {
    unexpectedPanelRuntimeDependency();
}
inline void FastBee::Screen::ScreenRuntime::getLastError(String&) const {
    unexpectedPanelRuntimeDependency();
}
inline void FastBee::Screen::ScreenRuntime::requestRefresh() {
    unexpectedPanelRuntimeDependency();
}
inline bool FastBee::Screen::ScreenRuntime::requestSystemAction(
        FastBee::Screen::ScreenSystemAction, uint8_t, String&) {
    unexpectedPanelRuntimeDependency();
}
inline FastBee::Screen::ScreenSystemActionCapabilities
FastBee::Screen::ScreenRuntime::systemActionCapabilities() const {
    if (!allowPanelNativeRuntimeDependencies()) {
        unexpectedPanelRuntimeDependency();
    }
    return panelNativeSystemActionCapabilities();
}

inline PeriphExecManager& PeriphExecManager::getInstance() {
    unexpectedPanelRuntimeDependency();
}
inline bool PeriphExecManager::getRuleSnapshot(const String&, PeriphExecRule&) const {
    unexpectedPanelRuntimeDependency();
}
inline std::vector<PeriphExecRule> PeriphExecManager::getAllRules() const {
    unexpectedPanelRuntimeDependency();
}
inline bool PeriphExecManager::getObservation(const String&, ObservationSnapshot&) const {
    unexpectedPanelRuntimeDependency();
}
inline std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(size_t) const {
    unexpectedPanelRuntimeDependency();
}
inline std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(const std::vector<const char*>&) const {
    unexpectedPanelRuntimeDependency();
}
inline std::vector<AsyncExecResult> PeriphExecManager::getRecentResults() {
    unexpectedPanelRuntimeDependency();
}
inline bool PeriphExecManager::runOnce(const String&, const String&, String*, AsyncExecResult*) {
    unexpectedPanelRuntimeDependency();
}
inline PeripheralManager& PeripheralManager::getInstance() {
    unexpectedPanelRuntimeDependency();
}
inline bool PeripheralManager::getPeripheralSnapshot(const String&, PeripheralConfig&, uint32_t*) const {
    unexpectedPanelRuntimeDependency();
}
inline bool SystemRebooter::isScheduled() {
    unexpectedPanelRuntimeDependency();
}
inline void SystemRebooter::scheduleReboot(const char*, unsigned long, RestartReason) {
    unexpectedPanelRuntimeDependency();
}
