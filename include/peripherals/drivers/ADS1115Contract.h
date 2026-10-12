#ifndef FASTBEE_ADS1115_CONTRACT_H
#define FASTBEE_ADS1115_CONTRACT_H

#include <cmath>
#include <cstdint>

namespace FastBee {

// Pure H1 preparation, not a registered driver or an electrical safety check.
// TI SBAS444E (December 2024), table 8-3. FSR is ADC scaling, NOT input rating.
struct ADS1115ConversionConfig {
    uint16_t registerValue = 0;
    uint32_t nominalPeriodUs = 0; // Nominal only; never a conversion-ready test.
};

inline int ads1115GainIndex(uint32_t fullScaleMv) {
    constexpr uint16_t ranges[] = {6144, 4096, 2048, 1024, 512, 256};
    for (int index = 0; index < 6; ++index) {
        if (ranges[index] == fullScaleMv) return index;
    }
    return -1;
}

inline bool makeADS1115ConversionConfig(uint32_t mux, uint32_t fullScaleMv,
                                       uint32_t samplesPerSecond,
                                       ADS1115ConversionConfig& output) {
    output = {};
    const int gain = ads1115GainIndex(fullScaleMv);
    if (mux > 7 || gain < 0) return false;
    constexpr uint16_t rates[] = {8, 16, 32, 64, 128, 250, 475, 860};
    int rate = -1;
    for (int index = 0; index < 8; ++index) {
        if (rates[index] == samplesPerSecond) rate = index;
    }
    if (rate < 0) return false;
    // Start one conversion, single-shot mode, comparator disabled. Callers must
    // first establish ownership and idle status; writing OS cannot abort a run.
    output.registerValue = static_cast<uint16_t>(
        0x8103U | (mux << 12) | (static_cast<uint32_t>(gain) << 9) |
        (static_cast<uint32_t>(rate) << 5));
    output.nominalPeriodUs = (1000000UL + samplesPerSecond - 1) / samplesPerSecond;
    return true;
}

// Optional scan mask: zero preserves the original single-MUX/two-value API.
// A scan reuses one physical owner and selects at most four of the eight MUXes.
inline uint8_t ads1115ScanCount(uint32_t mask) {
    if (mask > 255) return 0;
    uint8_t count = 0;
    for (; mask; mask >>= 1) count += mask & 1U;
    return count <= 4 ? count : 0;
}
inline bool ads1115ScanMaskValid(uint32_t mask) {
    return mask == 0 || ads1115ScanCount(mask) != 0;
}
inline uint32_t ads1115ReadBudgetMs(uint32_t rate, uint8_t scanMask = 0) {
    if (!rate || !ads1115ScanMaskValid(scanMask)) return 0;
    const uint32_t count = scanMask ? ads1115ScanCount(scanMask) : 1;
    return count * (2U * ((1000U + rate - 1U) / rate) + 50U);
}

struct ADS1115DecodedSample {
    double volts = NAN;
    int32_t signedCode = 0;
    bool valid = false;
    bool atCodeRail = false; // Does not prove the analog input is safe/unclipped.
};

inline bool decodeADS1115Sample(uint16_t code, uint32_t fullScaleMv,
                                ADS1115DecodedSample& output) {
    output = {};
    if (ads1115GainIndex(fullScaleMv) < 0) return false;
    // Avoid implementation-defined uint16_t -> int16_t narrowing on host/MCU.
    output.signedCode = code >= 0x8000U ? static_cast<int32_t>(code) - 65536 : code;
    output.volts = output.signedCode * (static_cast<double>(fullScaleMv) / 32768000.0);
    output.atCodeRail = code == 0x7fffU || code == 0x8000U;
    output.valid = true; // Decoding only; not bus, freshness, or hardware quality.
    return true;
}

} // namespace FastBee
#endif
