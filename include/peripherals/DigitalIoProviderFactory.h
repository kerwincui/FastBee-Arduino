#pragma once
#include <Arduino.h>
#include <memory>
#include "peripherals/DigitalIoProvider.h"

struct PeripheralConfig;
namespace FastBee {
// Caller holds PM state lock, validates the complete dependency graph and
// initializes the dedicated native RESET configuration before MCP admission.
// This allocates/claims ownership, but does not pulse RESET or start a session.
std::unique_ptr<DigitalIoProvider> createDigitalIoProvider(
    const PeripheralConfig& config, uint8_t sda, uint8_t scl,
    uint32_t frequency, String& error);
}
