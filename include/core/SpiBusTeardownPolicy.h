#ifndef FASTBEE_SPI_BUS_TEARDOWN_POLICY_H
#define FASTBEE_SPI_BUS_TEARDOWN_POLICY_H

#include "core/PeripheralConfig.h"
#include <cstring>

namespace FastBee {

// Conservative configuration-level protection until every global SPI user has
// a runtime lease. This is not an active-owner count or a compatibility check.
inline bool usesGlobalSpi(const PeripheralConfig& config) {
    switch (config.type) {
        case PeripheralType::SPI:
        case PeripheralType::RFID_MFRC522:
            return true;
        case PeripheralType::SDIO:
            return config.params.sdcard.interface == 1;
        case PeripheralType::SCREEN_DISPLAY:
            return strcasecmp(config.params.display.backend, "spi_tft") == 0;
        default:
            return false;
    }
}

inline int globalSpiChipSelect(const PeripheralConfig& config) {
    if (!usesGlobalSpi(config)) return -1;
    if (config.type == PeripheralType::SCREEN_DISPLAY)
        return config.params.display.cs;
    return config.pinCount >= 4 && config.pins[3] != 255 ? config.pins[3] : -1;
}

struct SpiTeardownProtection {
    bool retainBus = false;
    bool retainChipSelect = false;

    void include(const PeripheralConfig& retiring, const PeripheralConfig& peer) {
        if (peer.id == retiring.id || !peer.enabled || !usesGlobalSpi(peer)) return;
        // All these drivers use the same global SPI object. Different declared
        // pins (e.g. legacy config or a write-only TFT) do not establish a
        // different hardware bus and must not authorize SPI.end().
        retainBus = true;
        const int cs = globalSpiChipSelect(retiring);
        if (cs >= 0 && cs == globalSpiChipSelect(peer)) retainChipSelect = true;
    }
};

}  // namespace FastBee

#endif
