#pragma once
#include <math.h>
#include <vector>
#include "core/PeripheralConfig.h"
class PeripheralManager {
public:
 static PeripheralManager& getInstance() { static PeripheralManager p; return p; }
 std::vector<PeripheralConfig> getAllPeripherals() { ++reads; return items; }
 std::vector<PeripheralConfig> items;
 unsigned reads=0;
};
