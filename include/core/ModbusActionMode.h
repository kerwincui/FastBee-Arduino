#pragma once
#include <cstdint>

namespace FastBee {
// A boot/current-state observation must never replay the action's ctrl array.
// This mode is call-local, not a mutable flag on the shared executor.
enum class ModbusActionMode : uint8_t { ExecuteRule, ReadOnlySnapshot };
} // namespace FastBee
