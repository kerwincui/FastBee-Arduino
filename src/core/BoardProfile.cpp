#include "core/BoardProfile.h"
#include "core/ChipConfig.h"
#include <array>

namespace FastBee {
namespace {

constexpr uint16_t IO = PIN_CAP_INPUT | PIN_CAP_OUTPUT | PIN_CAP_PULL_UP |
                        PIN_CAP_PULL_DOWN | PIN_CAP_INTERRUPT;
constexpr uint16_t INPUT_ONLY = PIN_CAP_INPUT | PIN_CAP_INTERRUPT;

// Chip-level C3 policy, not a claim that every carrier board is verified.
// GPIO11 powers flash on common modules; 12..17 are the flash bus.
// Only ADC1 is advertised: ESP-IDF does not support reliable C3 ADC2 reads.
constexpr std::array<BoardPinProfile, 22> makeC3Pins() {
    std::array<BoardPinProfile, 22> pins{};
    for (uint8_t gpio = 0; gpio < pins.size(); ++gpio) {
        auto& pin = pins[gpio];
        pin = {gpio, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE,
               BOARD_PIN_NONE, BOARD_PIN_NONE};
        if (gpio >= 11 && gpio <= 17) {
            pin.capabilities = PIN_CAP_NONE;
            pin.policy = PinPolicy::HARD_FORBIDDEN;
            pin.risks = PIN_RISK_FLASH;
            continue;
        }
        if (gpio <= 4) {
            pin.capabilities |= PIN_CAP_ADC;
            pin.adcUnit = 1;
            pin.adcChannel = gpio;
        }
        if (gpio == 2 || gpio == 8 || gpio == 9) pin.risks |= PIN_RISK_STRAPPING;
        if (gpio >= 4 && gpio <= 7) pin.risks |= PIN_RISK_JTAG;
        if (gpio == 18 || gpio == 19) pin.risks |= PIN_RISK_USB_SERIAL_JTAG;
        if (gpio == 20 || gpio == 21) pin.risks |= PIN_RISK_UART0_CONSOLE;
        if (pin.risks != PIN_RISK_NONE) pin.policy = PinPolicy::CONDITIONAL;
    }
    return pins;
}
constexpr auto C3_PINS = makeC3Pins();
constexpr BoardProfile C3_PROFILE = {
    BoardProfileId::ESP32_C3_GENERIC, "esp32-c3", "ESP32-C3 (chip policy)",
    "esp32-c3-v1", "Espressif ESP32-C3 datasheet and ESP-IDF GPIO/ADC/RMT capabilities",
    C3_PINS.data(), C3_PINS.size(),
    // The planner's RMT claims currently represent NeoPixel TX channels.
    {1, 1, 1, 1, 6, 4, 2, 1, 4, 0, 0, false},
    {8, 9, 5, 6, 4, 7, 20, 21, BOARD_PIN_NONE}
};

// S3 GPIO46 supports output (unlike ESP32-S2). GPIO22..25 do not exist;
// SPI memory reserves 26..32 and Octal PSRAM additionally reserves 33..37.
// Source: ESP32-S3 datasheet and ESP-IDF esp32s3/soc_caps.h GPIO masks.
constexpr std::array<BoardPinProfile, 49> makeS3Pins(bool octalPsram) {
    std::array<BoardPinProfile, 49> pins{};
    for (uint8_t gpio = 0; gpio < pins.size(); ++gpio) {
        auto& pin = pins[gpio];
        pin = {gpio, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE};
        if ((gpio >= 22 && gpio <= 32) || (octalPsram && gpio >= 33 && gpio <= 37)) {
            pin.capabilities = PIN_CAP_NONE;
            pin.policy = PinPolicy::HARD_FORBIDDEN;
            pin.risks = gpio <= 25 ? PIN_RISK_NOT_EXPOSED : PIN_RISK_FLASH;
            continue;
        }
        if (gpio >= 1 && gpio <= 20) {
            pin.capabilities |= PIN_CAP_ADC;
            pin.adcUnit = gpio <= 10 ? 1 : 2;
            pin.adcChannel = (gpio - 1) % 10;
            if (gpio > 10) pin.risks |= PIN_RISK_ADC2_WIFI;
        }
        if (gpio >= 1 && gpio <= 14) pin.capabilities |= PIN_CAP_TOUCH;
        if (gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46) pin.risks |= PIN_RISK_STRAPPING;
        if (gpio == 19 || gpio == 20) pin.risks |= PIN_RISK_USB_SERIAL_JTAG;
        if (gpio >= 39 && gpio <= 42) pin.risks |= PIN_RISK_JTAG;
        if (gpio == 43 || gpio == 44) pin.risks |= PIN_RISK_UART0_CONSOLE;
        if (pin.risks != PIN_RISK_NONE) pin.policy = PinPolicy::CONDITIONAL;
    }
    return pins;
}
constexpr auto S3_OCTAL_PINS = makeS3Pins(true);
constexpr auto S3_QUAD_PINS = makeS3Pins(false);
constexpr BoardProfile S3_OCTAL_PROFILE = {
    BoardProfileId::ESP32_S3_GENERIC, "esp32-s3-octal", "ESP32-S3 with Octal PSRAM",
    "esp32-s3-v1", "Espressif ESP32-S3 datasheet and ESP-IDF GPIO capability masks",
    S3_OCTAL_PINS.data(), S3_OCTAL_PINS.size(),
    {1, 1, 2, 1, 8, 4, 4, 2, 4, 4, 2, true},
    {8, 9, 13, 11, 12, 10, 44, 43, BOARD_PIN_NONE}
};
constexpr BoardProfile S3_QUAD_PROFILE = {
    BoardProfileId::ESP32_S3_GENERIC, "esp32-s3", "ESP32-S3",
    "esp32-s3-v1", "Espressif ESP32-S3 datasheet and ESP-IDF GPIO capability masks",
    S3_QUAD_PINS.data(), S3_QUAD_PINS.size(),
    {1, 1, 2, 1, 8, 4, 4, 2, 4, 4, 2, true},
    {8, 9, 13, 11, 12, 10, 44, 43, BOARD_PIN_NONE}
};

// Common ESP32-WROOM-32 module policy, also used by the explicit PZ board. Entries for
// package-internal or non-bonded numbers are retained so callers get a precise
// hard-forbidden result instead of a generic out-of-range error.
constexpr BoardPinProfile CLASSIC_WROOM_BASE_PINS[] = {
    {0,  static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_ADC2_WIFI, 2, 1},
    {1,  IO, PinPolicy::CONDITIONAL, PIN_RISK_UART0_CONSOLE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {2,  static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_ADC2_WIFI, 2, 2},
    {3,  IO, PinPolicy::CONDITIONAL, PIN_RISK_UART0_CONSOLE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {4,  static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::RECOMMENDED, PIN_RISK_ADC2_WIFI, 2, 0},
    {5,  IO, PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {6,  PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {7,  PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {8,  PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {9,  PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {10, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {11, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {12, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_ADC2_WIFI, 2, 5},
    {13, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::RECOMMENDED, PIN_RISK_ADC2_WIFI, 2, 4},
    {14, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::RECOMMENDED, PIN_RISK_ADC2_WIFI, 2, 6},
    {15, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_ADC2_WIFI, 2, 3},
    {16, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {17, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {18, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {19, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {20, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {21, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {22, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {23, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {24, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {25, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_DAC), PinPolicy::RECOMMENDED, PIN_RISK_ADC2_WIFI, 2, 8},
    {26, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_DAC), PinPolicy::RECOMMENDED, PIN_RISK_ADC2_WIFI, 2, 9},
    {27, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::RECOMMENDED, PIN_RISK_ADC2_WIFI, 2, 7},
    {28, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {29, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {30, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {31, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {32, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::RECOMMENDED, PIN_RISK_NONE, 1, 4},
    {33, static_cast<uint16_t>(IO | PIN_CAP_ADC | PIN_CAP_TOUCH), PinPolicy::RECOMMENDED, PIN_RISK_NONE, 1, 5},
    {34, static_cast<uint16_t>(INPUT_ONLY | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_INPUT_ONLY, 1, 6},
    {35, static_cast<uint16_t>(INPUT_ONLY | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_INPUT_ONLY, 1, 7},
    {36, static_cast<uint16_t>(INPUT_ONLY | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_INPUT_ONLY, 1, 0},
    {37, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {38, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {39, static_cast<uint16_t>(INPUT_ONLY | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_INPUT_ONLY, 1, 3}
};

// Memory wiring belongs to the module layer, not to an arbitrary carrier board.
// WROVER reserves 16/17 for PSRAM even when PSRAM initialization later fails.
constexpr std::array<BoardPinProfile, 40> makeClassicModulePins(bool psram) {
    std::array<BoardPinProfile, 40> pins{};
    for (size_t index = 0; index < pins.size(); ++index) {
        pins[index] = CLASSIC_WROOM_BASE_PINS[index];
        if (index >= 12 && index <= 15) {
            pins[index].risks |= PIN_RISK_JTAG;
            pins[index].policy = PinPolicy::CONDITIONAL;
        }
        if (psram && (index == 16 || index == 17)) {
            pins[index].capabilities = PIN_CAP_NONE;
            pins[index].policy = PinPolicy::HARD_FORBIDDEN;
            pins[index].risks = PIN_RISK_PSRAM;
        }
    }
    return pins;
}
constexpr auto CLASSIC_WROOM_PINS = makeClassicModulePins(false);
constexpr auto CLASSIC_WROVER_PINS = makeClassicModulePins(true);
constexpr BoardResourceLimits CLASSIC_RESOURCES = {1, 1, 2, 1, 16, 8, 8, 2, 4, 8, 2, true};
constexpr BoardDefaultPins CLASSIC_DEFAULTS = {21, 22, 19, 23, 18, 4, 3, 1, BOARD_PIN_NONE};
constexpr BoardProfile CLASSIC_WROOM_PROFILE = {
    BoardProfileId::ESP32_WROOM_GENERIC, "esp32-wroom", "ESP32 WROOM-compatible module",
    "esp32-wroom-policy-v1", "Espressif ESP32-WROOM-32 datasheet v3.8; carrier wiring unverified",
    CLASSIC_WROOM_PINS.data(), CLASSIC_WROOM_PINS.size(), CLASSIC_RESOURCES, CLASSIC_DEFAULTS
};
constexpr BoardProfile CLASSIC_WROVER_PROFILE = {
    BoardProfileId::ESP32_WROVER_GENERIC, "esp32-wrover", "ESP32 WROVER-compatible PSRAM module",
    "esp32-wrover-policy-v1", "Espressif ESP32-WROVER-E/IE datasheet v2.4; carrier wiring unverified",
    CLASSIC_WROVER_PINS.data(), CLASSIC_WROVER_PINS.size(), CLASSIC_RESOURCES, CLASSIC_DEFAULTS
};

constexpr BoardPinProfile C6_DEVKIT_PINS[] = {
    {0,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_NONE, 1, 0},
    {1,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_NONE, 1, 1},
    {2,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_NONE, 1, 2},
    {3,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::RECOMMENDED, PIN_RISK_NONE, 1, 3},
    {4,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_JTAG, 1, 4},
    {5,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_JTAG, 1, 5},
    {6,  static_cast<uint16_t>(IO | PIN_CAP_ADC), PinPolicy::CONDITIONAL, PIN_RISK_JTAG | PIN_RISK_LP_I2C, 1, 6},
    {7,  IO, PinPolicy::CONDITIONAL, PIN_RISK_JTAG | PIN_RISK_LP_I2C, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {8,  IO, PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_ONBOARD_RGB, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {9,  IO, PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {10, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {11, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {12, IO, PinPolicy::CONDITIONAL, PIN_RISK_USB_SERIAL_JTAG, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {13, IO, PinPolicy::CONDITIONAL, PIN_RISK_USB_SERIAL_JTAG, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {14, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_NOT_EXPOSED, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {15, IO, PinPolicy::CONDITIONAL, PIN_RISK_STRAPPING | PIN_RISK_JTAG, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {16, IO, PinPolicy::CONDITIONAL, PIN_RISK_UART0_CONSOLE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {17, IO, PinPolicy::CONDITIONAL, PIN_RISK_UART0_CONSOLE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {18, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {19, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {20, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {21, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {22, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {23, IO, PinPolicy::RECOMMENDED, PIN_RISK_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {24, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {25, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {26, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {27, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {28, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {29, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE},
    {30, PIN_CAP_NONE, PinPolicy::HARD_FORBIDDEN, PIN_RISK_FLASH, BOARD_PIN_NONE, BOARD_PIN_NONE}
};

constexpr BoardProfile PZ_ESP32_PROFILE = {
    BoardProfileId::PUZHONG_PZ_ESP32,
    "puzhong-pz-esp32",
    "Puzhong PZ-ESP32",
    "pz-esp32-v1.2",
    "Puzhong PZ-ESP32 baseboard schematic and MicroPython guide V1.2",
    CLASSIC_WROOM_BASE_PINS,
    sizeof(CLASSIC_WROOM_BASE_PINS) / sizeof(CLASSIC_WROOM_BASE_PINS[0]),
    {1, 1, 2, 1, 16, 8, 8, 2, 4, 8, 2, true},
    {21, 22, 19, 23, 18, 4, 3, 1, BOARD_PIN_NONE}
};

constexpr BoardProfile C6_DEVKIT_PROFILE = {
    BoardProfileId::ESP32_C6_DEVKITC_1,
    "esp32-c6-devkitc-1",
    "ESP32-C6-DevKitC-1 (WROOM-1)",
    "devkitc-1-v1",
    "Espressif ESP32-C6-DevKitC-1 User Guide and ESP32-C6 datasheet",
    C6_DEVKIT_PINS,
    sizeof(C6_DEVKIT_PINS) / sizeof(C6_DEVKIT_PINS[0]),
    {1, 1, 1, 1, 6, 4, 4, 1, 4, 4, 2, false},
    {23, 22, 20, 19, 21, 18, 17, 16, 8}
};

}  // namespace

const BoardProfile& getPuzhongPzEsp32Profile() {
    return PZ_ESP32_PROFILE;
}

const BoardProfile& getEsp32ModuleProfile(bool psram) {
    return psram ? CLASSIC_WROVER_PROFILE : CLASSIC_WROOM_PROFILE;
}

const BoardProfile& getEsp32C6DevKitC1Profile() {
    return C6_DEVKIT_PROFILE;
}

const BoardProfile& getEsp32C3Profile() {
    return C3_PROFILE;
}

const BoardProfile& getEsp32S3Profile(bool octalPsram) {
    return octalPsram ? S3_OCTAL_PROFILE : S3_QUAD_PROFILE;
}

const BoardProfile& getCompiledBoardProfile() {
#if CHIP_IS_ESP32C6
    return C6_DEVKIT_PROFILE;
#elif CHIP_IS_ESP32C3
    return C3_PROFILE;
#elif CHIP_IS_ESP32S3
#if defined(CONFIG_SPIRAM_MODE_OCT)
    return S3_OCTAL_PROFILE;
#else
    return S3_QUAD_PROFILE;
#endif
#else
#if defined(BOARD_HAS_PSRAM) || (defined(FASTBEE_USE_PSRAM) && FASTBEE_USE_PSRAM)
#if defined(FASTBEE_BOARD_PUZHONG_PZ_ESP32) && FASTBEE_BOARD_PUZHONG_PZ_ESP32
#error "PZ WROOM board policy is incompatible with classic PSRAM memory wiring"
#endif
    return CLASSIC_WROVER_PROFILE;
#elif defined(FASTBEE_BOARD_PUZHONG_PZ_ESP32) && FASTBEE_BOARD_PUZHONG_PZ_ESP32
    return PZ_ESP32_PROFILE;
#else
    return CLASSIC_WROOM_PROFILE;
#endif
#endif
}

const BoardProfile& getBoardProfile(BoardProfileId id) {
    if (id == BoardProfileId::ESP32_C3_GENERIC) return C3_PROFILE;
    if (id == BoardProfileId::ESP32_WROOM_GENERIC) return CLASSIC_WROOM_PROFILE;
    if (id == BoardProfileId::ESP32_WROVER_GENERIC) return CLASSIC_WROVER_PROFILE;
    if (id == BoardProfileId::ESP32_S3_GENERIC) return S3_OCTAL_PROFILE;
    return id == BoardProfileId::ESP32_C6_DEVKITC_1
        ? C6_DEVKIT_PROFILE
        : PZ_ESP32_PROFILE;
}

const BoardPinProfile* findBoardPin(const BoardProfile& profile, uint8_t gpio) {
    for (size_t i = 0; i < profile.pinCount; ++i) {
        if (profile.pins[i].gpio == gpio) return &profile.pins[i];
    }
    return nullptr;
}

bool boardPinHasCapabilities(const BoardPinProfile& pin, uint16_t required) {
    return (pin.capabilities & required) == required;
}

const char* pinPolicyCode(PinPolicy policy) {
    switch (policy) {
        case PinPolicy::RECOMMENDED: return "recommended";
        case PinPolicy::CONDITIONAL: return "conditional";
        case PinPolicy::HARD_FORBIDDEN: return "hard_forbidden";
        default: return "unknown";
    }
}

}  // namespace FastBee
