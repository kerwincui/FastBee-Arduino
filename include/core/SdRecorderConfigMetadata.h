#pragma once
#include <cstdint>
namespace FastBee {
struct SdRecorderNumber { const char* key; uint16_t min, max, defaultValue; };
inline constexpr SdRecorderNumber kSdRecorderNumbers[] = {
    {"sampleSec", 1, 3600, 5}, {"maxAgeSec", 1, 3600, 30},
    {"flushSec", 1, 300, 10}, {"fileKiB", 4, 16, 16}, {"fileCount", 2, 8, 4}
};
} // namespace FastBee
