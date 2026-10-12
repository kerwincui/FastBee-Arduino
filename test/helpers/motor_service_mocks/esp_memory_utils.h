#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace MotorMemoryMock {
struct Region { uintptr_t start; size_t size; bool internal; };
inline std::vector<Region> regions;
}
inline bool esp_ptr_in_dram(const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region : MotorMemoryMock::regions)
        if (address >= region.start && address - region.start < region.size) return region.internal;
    // The harness's unregistered native stack/static storage models internal
    // DRAM; only allocations explicitly requested as PSRAM model external RAM.
    return true;
}
