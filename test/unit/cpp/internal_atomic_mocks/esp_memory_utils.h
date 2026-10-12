#pragma once
#include <cstdint>
#include <map>
namespace AtomicSdkMock {
inline bool allInternal = true;
inline bool failAllocation = false;
inline bool wrongRegion = false;
inline unsigned allocations = 0, frees = 0;
inline std::map<uintptr_t, size_t> internalAllocations;
}
inline bool esp_ptr_in_dram(const void* pointer) {
    if (AtomicSdkMock::allInternal) return true;
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& item : AtomicSdkMock::internalAllocations)
        if (address >= item.first && address - item.first < item.second) return true;
    return false;
}
