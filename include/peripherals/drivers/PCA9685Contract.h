#ifndef FASTBEE_PCA9685_CONTRACT_H
#define FASTBEE_PCA9685_CONTRACT_H

#include <cmath>
#include <cstdint>

namespace FastBee {
static constexpr double PCA9685_MAX_CLOCK_HZ = 50000000.0;

// Pure H1 preparation, no I2C writes or registered output capability.
// NXP PCA9685 Rev. 4 (16 April 2015), sections 7.3.1/7.3.3/7.3.5.
struct PCA9685FrequencyConfig {
    uint8_t prescale = 0;
    double actualHz = 0; // Based on supplied clock, not a measured output.
};

inline bool makePCA9685FrequencyConfig(double requestedHz, double oscillatorHz,
                                      PCA9685FrequencyConfig& output) {
    output = {};
    if (!std::isfinite(requestedHz) || requestedHz <= 0 ||
        !std::isfinite(oscillatorHz) || oscillatorHz <= 0 || oscillatorHz > PCA9685_MAX_CLOCK_HZ) {
        return false;
    }
    const double divider = std::round((oscillatorHz / 4096.0) / requestedHz);
    if (!std::isfinite(divider) || divider < 4 || divider > 256) return false;
    output.prescale = static_cast<uint8_t>(divider - 1);
    output.actualHz = oscillatorHz / (4096.0 * divider);
    // ALL channels share this frequency. A future driver must retain output
    // ownership, set SLEEP before PRE_SCALE, and wait >=500us after internal
    // oscillator wake. EXTCLK is sticky; this helper never changes clock mode.
    return true;
}

struct PCA9685ChannelRegisters {
    uint8_t startRegister = 0;
    uint8_t bytes[4] = {}; // ON_L, ON_H, OFF_L, OFF_H; no bus transaction.
    bool valid = false;
};

inline bool makePCA9685DutyTicks(double percent, uint16_t& ticks) {
    ticks = 0;
    if (!std::isfinite(percent) || percent < 0 || percent > 100) return false;
    if (percent == 0) return true;
    if (percent == 100) { ticks = 4096; return true; }
    // Interior duty requests remain PWM even when rounding reaches an endpoint;
    // constant FULL_ON/OFF is reserved for explicit 100/0 percent requests.
    const double count = std::round(percent * 4096.0 / 100.0);
    ticks = static_cast<uint16_t>(count < 1 ? 1 : count > 4095 ? 4095 : count);
    return true;
}

// Pulse/servo requests cannot round to FULL_ON/OFF. Frequency must be the
// shared prescaler-derived value, not the requested or a purported measurement.
inline bool makePCA9685PulseTicks(double pulseUs, double calculatedHz, uint16_t& ticks) {
    ticks = 0;
    if (!std::isfinite(pulseUs) || pulseUs <= 0 || !std::isfinite(calculatedHz) || calculatedHz <= 0)
        return false;
    const double count = std::round(pulseUs * calculatedHz * 4096.0 / 1000000.0);
    if (!std::isfinite(count) || count < 1 || count > 4095) return false;
    ticks = static_cast<uint16_t>(count);
    return true;
}

inline bool makePCA9685ServoTicks(double angle, double minAngle, double maxAngle,
        double minPulseUs, double maxPulseUs, double calculatedHz, uint16_t& ticks) {
    ticks = 0;
    if (!std::isfinite(angle) || !std::isfinite(minAngle) || !std::isfinite(maxAngle) ||
        minAngle >= maxAngle || !std::isfinite(maxAngle - minAngle) || angle < minAngle || angle > maxAngle ||
        !std::isfinite(minPulseUs) || !std::isfinite(maxPulseUs) ||
        minPulseUs <= 0 || maxPulseUs <= minPulseUs) return false;
    uint16_t minimum, maximum;
    if (!makePCA9685PulseTicks(minPulseUs, calculatedHz, minimum) ||
        !makePCA9685PulseTicks(maxPulseUs, calculatedHz, maximum) || minimum >= maximum) return false;
    const double pulse = minPulseUs + (angle - minAngle) / (maxAngle - minAngle) * (maxPulseUs - minPulseUs);
    return makePCA9685PulseTicks(pulse, calculatedHz, ticks);
}

inline bool makePCA9685ChannelRegisters(uint32_t channel, uint32_t phase,
                                       uint32_t widthTicks,
                                       PCA9685ChannelRegisters& output) {
    output = {};
    if (channel >= 16 || phase >= 4096 || widthTicks > 4096) return false;
    uint16_t on = 0, off = 0;
    // Equal ordinary ON/OFF counts are forbidden by the device contract.
    // Exact endpoints use only FULL_OFF or FULL_ON, never both.
    if (widthTicks == 0) off = 0x1000;
    else if (widthTicks == 4096) on = 0x1000;
    else {
        on = static_cast<uint16_t>(phase);
        off = static_cast<uint16_t>((phase + widthTicks) & 0x0fffU);
    }
    output.startRegister = static_cast<uint8_t>(0x06U + 4U * channel);
    output.bytes[0] = static_cast<uint8_t>(on);
    output.bytes[1] = static_cast<uint8_t>(on >> 8);
    output.bytes[2] = static_cast<uint8_t>(off);
    output.bytes[3] = static_cast<uint8_t>(off >> 8);
    output.valid = true;
    return true;
}

} // namespace FastBee
#endif
