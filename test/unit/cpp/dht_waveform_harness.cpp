#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using std::isinf;
using std::isnan;
static constexpr uint8_t LOW = 0, HIGH = 1, INPUT_PULLUP = 2, OUTPUT = 3;
enum class SensorDriverType { DHT11, DHT22 };
struct Edge { uint64_t at; uint8_t level; };
static std::vector<Edge> edges;
static uint64_t nowUs = 0, releaseUs = 0;
static uint8_t pinModeValue = INPUT_PULLUP;
static bool released = false, heldLow = false, injectPreemption = false;
static uint32_t micros() { return static_cast<uint32_t>(nowUs++); }
static void delay(unsigned long ms) { nowUs += ms * 1000; }
static void delayMicroseconds(unsigned int us) { nowUs += us; }
static void digitalWrite(uint8_t, uint8_t) {}
static void pinMode(uint8_t, uint8_t mode) {
    if (pinModeValue == OUTPUT && mode == INPUT_PULLUP) {
        released = true; releaseUs = nowUs;
    }
    pinModeValue = mode;
}
static uint8_t digitalRead(uint8_t) {
    ++nowUs;
    if (heldLow) return LOW;
    if (!released) return HIGH;
    if (injectPreemption && nowUs - releaseUs >= 300) {
        nowUs += 160000; injectPreemption = false;
    }
    uint8_t level = HIGH;
    for (const auto& edge : edges) {
        if (edge.at > nowUs - releaseUs) break;
        level = edge.level;
    }
    return level;
}
// Match ESP32 Arduino pulseIn: discard an already active pulse first.
// Kept so the unchanged production reader demonstrates the regression.
[[maybe_unused]] static unsigned long pulseIn(uint8_t pin, uint8_t state,
                                             unsigned long timeout) {
    const uint32_t start = micros();
    while (digitalRead(pin) == state)
        if (static_cast<uint32_t>(micros() - start) >= timeout) return 0;
    while (digitalRead(pin) != state)
        if (static_cast<uint32_t>(micros() - start) >= timeout) return 0;
    const uint32_t pulseStart = micros();
    while (digitalRead(pin) == state)
        if (static_cast<uint32_t>(micros() - start) >= timeout) return 0;
    return static_cast<uint32_t>(micros() - pulseStart);
}
#include "dht_reader.inc"

static void waveform(std::array<uint8_t, 5> bytes, const std::string& scenario) {
    if (scenario == "disconnected" || scenario == "stuck-low") return;
    if (scenario == "missing-response-high") { edges = {{20, LOW}, {100, HIGH}}; return; }
    edges = {{20, LOW}, {100, HIGH}, {180, LOW}};
    uint64_t cursor = 180;
    for (unsigned bit = 0; bit < 40; ++bit) {
        if (scenario == "truncated" && bit == 23) break;
        cursor += 50; edges.push_back({cursor, HIGH});
        const bool one = (bytes[bit / 8] >> (7 - bit % 8)) & 1;
        cursor += scenario == "long-pulse" && bit == 15 ? 700 : (one ? 70 : 26);
        edges.push_back({cursor, LOW});
    }
    edges.push_back({cursor + 50, HIGH});
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    auto type = SensorDriverType::DHT11;
    std::array<uint8_t, 5> bytes = {55, 0, 24, 0, 79};
    float expectedHumidity = 55.0f, expectedTemperature = 24.0f;
    const bool valid = scenario == "dht11" || scenario == "dht22" ||
                       scenario == "negative" || scenario == "wrap" || scenario == "no-diagnostic";
    if (scenario == "dht22" || scenario == "negative") {
        type = SensorDriverType::DHT22;
        bytes = {2, 43, 0, 243, 32};
        expectedHumidity = 55.5f; expectedTemperature = 24.3f;
        if (scenario == "negative") {
            bytes = {2, 43, 128, 123, 40}; expectedTemperature = -12.3f;
        }
    }
    if (scenario == "wrap") nowUs = UINT32_MAX - 22000ULL;
    if (scenario == "checksum") bytes[4] ^= 1;
    if (scenario == "range") bytes = {110, 0, 24, 0, 134};
    heldLow = scenario == "stuck-low";
    injectPreemption = scenario == "preempted";
    waveform(bytes, scenario);
    const uint64_t before = nowUs;
    float humidity = NAN, temperature = NAN;
    DhtReadDiagnostic diagnostic;
    const bool actual = readDhtBounded(10, type, humidity, temperature,
                                     scenario == "no-diagnostic" ? nullptr : &diagnostic);
    std::string expectedPhase = "ok";
    if (scenario == "stuck-low") expectedPhase = "idle_low";
    if (scenario == "disconnected") expectedPhase = "response_low";
    if (scenario == "missing-response-high") expectedPhase = "response_high";
    if (scenario == "truncated" || scenario == "long-pulse" || scenario == "preempted") expectedPhase = "bit_pulse";
    if (scenario == "checksum") expectedPhase = "checksum";
    if (scenario == "range") expectedPhase = "range";
    if (diagnostic.phase != expectedPhase || (!valid && diagnostic.elapsedUs == 0) ||
        (expectedPhase == "bit_pulse" && (diagnostic.bit < 0 || diagnostic.bit > 39))) {
        std::cerr << "Unexpected failure evidence: " << diagnostic.phase << " expected=" << expectedPhase << '\n';
        return 1;
    }
    if (actual != valid || (valid && (std::fabs(humidity - expectedHumidity) > .05f ||
                                     std::fabs(temperature - expectedTemperature) > .05f)) ||
        pinModeValue != INPUT_PULLUP || nowUs - before > 185000) {
        std::cerr << scenario << ": result=" << actual << " expected=" << valid
                  << " humidity=" << humidity << " temperature=" << temperature
                  << " elapsedUs=" << nowUs - before << '\n';
        return 1;
    }
    std::cout << scenario << " PASS elapsedUs=" << nowUs - before << '\n';
}
