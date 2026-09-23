#pragma once

#include <stddef.h>
#include <stdint.h>

namespace FastBee {

constexpr uint8_t BOARD_PIN_NONE = 0xFF;

enum class BoardProfileId : uint8_t {
    PUZHONG_PZ_ESP32 = 0,
    ESP32_C6_DEVKITC_1 = 1,
    ESP32_S3_GENERIC = 2
};

// A policy is intentionally separate from electrical capabilities. A pin can
// be input/output capable while still requiring an explicit risk decision.
enum class PinPolicy : uint8_t {
    RECOMMENDED = 0,
    CONDITIONAL = 1,
    HARD_FORBIDDEN = 2
};

enum PinCapability : uint16_t {
    PIN_CAP_NONE = 0,
    PIN_CAP_INPUT = 1U << 0,
    PIN_CAP_OUTPUT = 1U << 1,
    PIN_CAP_PULL_UP = 1U << 2,
    PIN_CAP_PULL_DOWN = 1U << 3,
    PIN_CAP_INTERRUPT = 1U << 4,
    PIN_CAP_ADC = 1U << 5,
    PIN_CAP_DAC = 1U << 6,
    PIN_CAP_TOUCH = 1U << 7
};

enum PinRisk : uint32_t {
    PIN_RISK_NONE = 0,
    PIN_RISK_FLASH = 1UL << 0,
    PIN_RISK_NOT_EXPOSED = 1UL << 1,
    PIN_RISK_STRAPPING = 1UL << 2,
    PIN_RISK_INPUT_ONLY = 1UL << 3,
    PIN_RISK_UART0_CONSOLE = 1UL << 4,
    PIN_RISK_USB_SERIAL_JTAG = 1UL << 5,
    PIN_RISK_JTAG = 1UL << 6,
    PIN_RISK_ONBOARD_RGB = 1UL << 7,
    PIN_RISK_LP_I2C = 1UL << 8,
    PIN_RISK_ADC2_WIFI = 1UL << 9
};

struct BoardPinProfile {
    uint8_t gpio;
    uint16_t capabilities;
    PinPolicy policy;
    uint32_t risks;
    uint8_t adcUnit;
    uint8_t adcChannel;
};

struct BoardResourceLimits {
    uint8_t i2cBusCount;
    uint8_t spiBusCount;
    uint8_t genericUartCount;
    uint8_t firstGenericUartPort;
    uint8_t ledcChannelCount;
    uint8_t ledcTimerCount;
    uint8_t rmtChannelCount;
    uint8_t adcUnitCount;
    uint8_t encoderSlotCount;
    uint8_t pcntUnitCount;
    uint8_t pcntChannelsPerUnit;
    bool sdmmcSupported;
};

struct BoardDefaultPins {
    uint8_t i2cSda;
    uint8_t i2cScl;
    uint8_t spiMiso;
    uint8_t spiMosi;
    uint8_t spiSck;
    uint8_t spiCs;
    uint8_t uart0Rx;
    uint8_t uart0Tx;
    uint8_t onboardRgb;
};

struct BoardProfile {
    BoardProfileId id;
    const char* key;
    const char* displayName;
    const char* version;
    const char* source;
    const BoardPinProfile* pins;
    size_t pinCount;
    BoardResourceLimits resources;
    BoardDefaultPins defaults;
};

const BoardProfile& getPuzhongPzEsp32Profile();
const BoardProfile& getEsp32C6DevKitC1Profile();
const BoardProfile& getEsp32S3Profile(bool octalPsram = true);
const BoardProfile& getCompiledBoardProfile();
const BoardProfile& getBoardProfile(BoardProfileId id);

const BoardPinProfile* findBoardPin(const BoardProfile& profile, uint8_t gpio);
bool boardPinHasCapabilities(const BoardPinProfile& pin, uint16_t required);
const char* pinPolicyCode(PinPolicy policy);

}  // namespace FastBee
