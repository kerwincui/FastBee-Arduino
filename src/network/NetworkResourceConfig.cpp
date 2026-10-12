#include "network/NetworkResourceConfig.h"
#include "core/PeripheralManager.h"
#include <memory>
#include <new>

namespace FastBee {
bool validateNetworkResourceConfig(const WiFiConfig& config, String& error) {
    error = "";
    const auto network = ethernetResourceConfig(config);
    const auto cellular = cellularResourceConfig(config);
    if (!network.enabled && !cellular.enabled) return true;
    std::unique_ptr<PreflightResult> result(new (std::nothrow) PreflightResult());
    if (!result) {
        error = "NETWORK_RESOURCE_MEMORY: unable to allocate bounded preflight result";
        return false;
    }
    const auto peripherals = PeripheralManager::getInstance().getAllPeripherals();
    PreflightContext context;
    context.usbSerialJtagActive =
        getCompiledBoardProfile().id == BoardProfileId::ESP32_C3_GENERIC ||
        getCompiledBoardProfile().id == BoardProfileId::ESP32_C6_DEVKITC_1;
    PeripheralResourcePlanner planner(getCompiledBoardProfile(), context, *result,
                                      peripherals.data(), peripherals.size());
    for (const auto& peripheral : peripherals) {
        planner.addPeripheral(peripheral, peripheral.enabled ? ClaimState::ACTIVE : ClaimState::DORMANT);
    }
    result->resetIssuesPreservingClaims();
    planner.addEthernet(network);
    planner.addCellular(cellular);
    if (result->claimsTruncated || result->issuesTruncated) {
        error = "NETWORK_RESOURCE_CAPACITY: resource graph is incomplete";
        return false;
    }
    for (size_t i = 0; i < result->issueCount; ++i) {
        const auto& issue = result->issues[i];
        if (issue.severity != PreflightSeverity::ERROR) continue;
        error = String("NETWORK_RESOURCE_") + preflightIssueCodeName(issue.code);
        if (issue.conflictingOwnerId) error += String(": ") + issue.conflictingOwnerId;
        return false;
    }
    // Existing network API has no per-pin acknowledgement workflow. Preserve
    // its warning policy; never bypass hard conflicts or incomplete graphs.
    return true;
}
}
