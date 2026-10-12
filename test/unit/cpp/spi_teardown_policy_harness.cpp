#include <cassert>
#include <math.h>
#include <cstring>
#include "core/SpiBusTeardownPolicy.h"

using FastBee::SpiTeardownProtection;

static PeripheralConfig make(PeripheralType type, const char* id, int cs = 5) {
    PeripheralConfig config;
    config.id = id;
    config.type = type;
    config.enabled = true;
    config.pinCount = 4;
    config.pins[0] = 19; config.pins[1] = 23; config.pins[2] = 18; config.pins[3] = cs;
    if (type == PeripheralType::SDIO) config.params.sdcard.interface = 1;
    if (type == PeripheralType::SCREEN_DISPLAY) {
        std::strcpy(config.params.display.backend, "spi_tft");
        config.params.display.cs = cs;
    }
    return config;
}

int main() {
    const auto a = make(PeripheralType::SPI, "a");
    for (auto type : {PeripheralType::SPI, PeripheralType::RFID_MFRC522,
                      PeripheralType::SDIO, PeripheralType::SCREEN_DISPLAY}) {
        auto peer = make(type, "b", 17);
        SpiTeardownProtection protection;
        protection.include(a, peer);
        assert(protection.retainBus && !protection.retainChipSelect);
        // Even mismatched legacy pins refer to the same global SPI object.
        peer.pins[0] = 12; peer.pins[1] = 13; peer.pins[2] = 14;
        SpiTeardownProtection legacy; legacy.include(a, peer);
        assert(legacy.retainBus);
        peer.pins[3] = 5;
        if (type == PeripheralType::SCREEN_DISPLAY) peer.params.display.cs = 5;
        protection.include(a, peer);
        assert(protection.retainChipSelect);
        peer.enabled = false;
        SpiTeardownProtection disabled; disabled.include(a, peer);
        assert(!disabled.retainBus && !disabled.retainChipSelect);
    }
    SpiTeardownProtection self; self.include(a, a);
    assert(!self.retainBus);
    auto sdmmc = make(PeripheralType::SDIO, "sdmmc");
    sdmmc.params.sdcard.interface = 2;
    auto rgb = make(PeripheralType::SCREEN_DISPLAY, "rgb");
    std::strcpy(rgb.params.display.backend, "rgb_panel");
    for (const auto& peer : {sdmmc, rgb, make(PeripheralType::I2C, "i2c")}) {
        SpiTeardownProtection independent; independent.include(a, peer);
        assert(!independent.retainBus && !independent.retainChipSelect);
    }
    auto invalid = make(PeripheralType::SPI, "invalid");
    invalid.pinCount = 0;
    SpiTeardownProtection unknown; unknown.include(a, invalid);
    assert(unknown.retainBus && !unknown.retainChipSelect);
    // Repeated policy evaluations have no state retained across lifecycles.
    for (int i = 0; i < 1000; ++i) {
        const auto b = make(PeripheralType::SPI, "b", 17);
        SpiTeardownProtection removingFirst; removingFirst.include(a, b);
        assert(removingFirst.retainBus);
        SpiTeardownProtection removingLast; removingLast.include(b, b);
        assert(!removingLast.retainBus);
    }
}
