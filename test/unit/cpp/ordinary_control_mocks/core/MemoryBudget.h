#pragma once
#include <cstdint>
namespace FastBee { namespace MemoryBudget {
inline uint32_t ordinaryFreeMemory = 100000;
constexpr uint32_t ACTION_TICKER_MIN_DRAM = 4096;
inline uint32_t dramFree() { return ordinaryFreeMemory; }
} }
