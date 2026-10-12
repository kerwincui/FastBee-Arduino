#pragma once
#include <cstdint>
class String;
class PeripheralManager;

namespace FastBee {
// Final write admission only. SDK acceptance is not physical voltage feedback.
bool writeDacAtVersion(PeripheralManager& manager, const String& id,
                       uint32_t expectedVersion, uint8_t value);
}
