#pragma once
#include "core/PeripheralResourcePlanner.h"
#include "network/WiFiManager.h"

namespace FastBee {
inline EthernetResourceConfig ethernetResourceConfig(const WiFiConfig& config) {
    const auto& pins = config.ethernet;
    return {config.networkType == NetworkType::NET_ETHERNET,
            pins.spiMiso, pins.spiMosi, pins.spiSck, pins.csPin, pins.rstPin, pins.intPin};
}
inline CellularResourceConfig cellularResourceConfig(const WiFiConfig& config) {
    const auto& pins = config.cellular;
    return {config.networkType == NetworkType::NET_4G, pins.rxPin, pins.txPin, pins.pwrPin};
}
// Caller holds PeripheralManager's recursive mutex until config commit/rollback.
// This checks configured resources, not live driver cleanup or physical wiring.
bool validateNetworkResourceConfig(const WiFiConfig& config, String& error);
}
