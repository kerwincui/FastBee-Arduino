#include "core/PeripheralManager.h"
#include "core/ResourceProfile.h"
#include "core/SpiBusInitializer.h"
#include "core/SpiBusTeardownPolicy.h"
#include "systems/LoggerSystem.h"

static_assert(FastBee::SpiBusInitializer::MAX_NAME_LENGTH ==
              FastBee::ResourceProfile::MAX_PERIPHERAL_ID_LEN,
              "SPI logical ID capacity must match peripheral validation");

// Caller holds PeripheralManager's mutex, then the SPI operation lock.
bool PeripheralManager::setupSpiHardware(const PeripheralConfig& config) {
    const auto fail = [&](const String& error) {
        auto state = runtimeStates.find(config.id);
        if (state != runtimeStates.end()) state->second.lastError = error;
        LOG_WARNINGF("Peripheral Manager: SPI '%s' init failed: %s", config.id.c_str(), error.c_str());
        return false;
    };
    if (config.pinCount < 4 || config.pins[3] == 255 || !isValidPin(config.pins[3]) ||
        isReservedPin(config.pins[3]) || isInputOnlyPin(config.pins[3])) {
        return fail("SPI_CONFIG_INVALID: four pins and a usable CS are required");
    }
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) return fail("SPI_BUSY: initialization deferred");
    String error;
    if (!FastBee::SpiBusInitializer::instance().acquireNamed(
            config.id, config.pins[2], config.pins[0], config.pins[1], config.pins[3], error)) {
        return fail(error);
    }
    pinMode(config.pins[3], OUTPUT);
    digitalWrite(config.pins[3], HIGH);
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) state->second.lastError = "";
    return true;
}

bool PeripheralManager::teardownSpiHardware(const PeripheralConfig& config) {
    const auto fail = [&](const String& error) {
        auto state = runtimeStates.find(config.id);
        if (state != runtimeStates.end()) state->second.lastError = error;
        LOG_WARNINGF("Peripheral Manager: SPI '%s' stop deferred: %s", config.id.c_str(), error.c_str());
        return false;
    };
    if (config.pinCount < 4 || config.pins[3] == 255) {
        return fail("SPI_CONFIG_INVALID: cannot identify the owned chip select");
    }
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) return fail("SPI_BUSY: peripheral and pins retained");
    FastBee::SpiTeardownProtection protection;
    for (const auto& pair : peripherals) protection.include(config, pair.second);
    auto& bus = FastBee::SpiBusInitializer::instance();
    String error;
    bool released = false;
    if (!bus.releaseNamed(config.id, error, config.pins[3], &released)) return fail(error);
    const uint8_t cs = config.pins[3];
    if (released && !protection.retainChipSelect && !bus.isChipSelectAcquired(cs) &&
        !isReservedPin(cs) && !isInputOnlyPin(cs)) {
        pinMode(cs, INPUT);
    }
    // Global host shutdown awaits W5500/legacy coordination; never infer that
    // an empty config or lease table proves exclusive hardware ownership.
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) state->second.lastError = "";
    return true;
}
