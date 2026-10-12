#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
// Production must never reintroduce the observed newlib float call chain.
#define snprintf FASTBEE_FORBIDDEN_snprintf
#include "protocols/ModbusRegisterCodec.h"
#undef snprintf

static size_t checks = 0;
static void compare(double value, uint8_t decimals) {
    char expected[128], actual[128];
    const int length = std::snprintf(expected, sizeof(expected), "%.*f", int(decimals), value);
    assert(length > 0 && size_t(length) < sizeof(expected));
    assert(FastBee::formatModbusFixed(value, decimals, actual, sizeof(actual)));
    if (std::strcmp(expected, actual)) {
        std::fprintf(stderr, "wire mismatch %.17g decimals=%u expected=%s actual=%s\n",
                     value, unsigned(decimals), expected, actual);
        std::exit(2);
    }
    assert(FastBee::formatModbusFixed(value, decimals, actual, size_t(length) + 1));
    actual[length] = '!';
    assert(!FastBee::formatModbusFixed(value, decimals, actual, size_t(length)));
    assert(actual[length] == '!');
    ++checks;
}
int main() {
    const double extremes[] = {0.0, -0.0, 0.5, -0.5, 1.5, 2.5, -2.5, 1.25, 1.75,
        9.9999995, -9.9999995, 1e-100, -1e-100,
        double(std::numeric_limits<float>::max()) * std::numeric_limits<float>::max(),
        -double(std::numeric_limits<float>::max()) * std::numeric_limits<float>::max(),
        double(std::numeric_limits<float>::denorm_min()) * std::numeric_limits<float>::denorm_min(),
        4294967295.0, -2147483648.0};
    for (double value : extremes) for (uint8_t decimals = 0; decimals <= 6; ++decimals) compare(value, decimals);
    std::mt19937 generator(0x14c19fc1);
    for (unsigned sample = 0; sample < 20000; ++sample) {
        const uint32_t rawBits = generator(), scaleBits = generator();
        float raw, scale;
        std::memcpy(&raw, &rawBits, sizeof(raw));
        std::memcpy(&scale, &scaleBits, sizeof(scale));
        if (!std::isfinite(raw) || !std::isfinite(scale)) continue;
        const double value = double(raw) * double(scale);
        for (uint8_t decimals = 0; decimals <= 6; ++decimals) compare(value, decimals);
    }
    char output[96];
    assert(!FastBee::formatModbusFixed(1, 7, output, sizeof(output)));
    assert(!FastBee::formatModbusFixed(1, 0, nullptr, sizeof(output)));
    assert(!FastBee::formatModbusFixed(1, 0, output, 0));
    assert(!FastBee::formatModbusFixed(std::numeric_limits<double>::infinity(), 0, output, sizeof(output)));
    assert(!FastBee::formatModbusFixed(std::numeric_limits<double>::quiet_NaN(), 0, output, sizeof(output)));
    assert(!FastBee::formatModbusFixed(std::numeric_limits<double>::max(), 0, output, sizeof(output)));
    std::printf("fixed mapping wire oracle: %zu comparisons passed\n", checks);
}
