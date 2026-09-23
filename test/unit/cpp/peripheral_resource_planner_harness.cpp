#include <assert.h>
#include <math.h>
#include <string.h>

#include <ArduinoJson.h>

#include "core/PeripheralConfig.h"
#include "core/BoardProfile.h"
#include "core/PeripheralResourcePlanner.h"

using namespace FastBee;

namespace {

PeripheralConfig makeGpio(const char* id, uint8_t pin, bool enabled = true) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.enabled = enabled;
    config.pinCount = 1;
    config.pins[0] = pin;
    return config;
}

PeripheralConfig makeI2c(const char* id, uint8_t sda, uint8_t scl,
                         uint8_t address) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::I2C;
    config.enabled = true;
    config.pinCount = 2;
    config.pins[0] = sda;
    config.pins[1] = scl;
    config.params.i2c.frequency = 400000;
    config.params.i2c.address = address;
    config.params.i2c.isMaster = true;
    return config;
}

PeripheralConfig makeSpi(const char* id, uint8_t miso, uint8_t mosi,
                         uint8_t sck, uint8_t cs) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::SPI;
    config.enabled = true;
    config.pinCount = 4;
    config.pins[0] = miso;
    config.pins[1] = mosi;
    config.pins[2] = sck;
    config.pins[3] = cs;
    config.params.spi.frequency = 1000000;
    return config;
}

PeripheralConfig makeUart(const char* id, uint8_t rx, uint8_t tx) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::UART;
    config.enabled = true;
    config.pinCount = 2;
    config.pins[0] = rx;
    config.pins[1] = tx;
    config.params.uart.baudRate = 115200;
    config.params.uart.dataBits = 8;
    config.params.uart.stopBits = 1;
    return config;
}

PeripheralConfig makePwm(const char* id, uint8_t pin, uint8_t channel,
                         uint32_t frequency) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::GPIO_PWM_OUTPUT;
    config.enabled = true;
    config.pinCount = 1;
    config.pins[0] = pin;
    config.params.gpio.pwmChannel = channel;
    config.params.gpio.pwmFrequency = frequency;
    config.params.gpio.pwmResolution = 8;
    return config;
}

PeripheralConfig makeNeoPixel(const char* id, uint8_t pin) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::NEO_PIXEL;
    config.enabled = true;
    config.pinCount = 1;
    config.pins[0] = pin;
    config.params.neopixel.count = 1;
    return config;
}

PeripheralConfig makeEncoder(const char* id, uint8_t pinA, uint8_t pinB) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::ENCODER;
    config.enabled = true;
    config.pinCount = 2;
    config.pins[0] = pinA;
    config.pins[1] = pinB;
    config.params.encoder.resolution = 20;
    config.params.encoder.useInterrupt = true;
    return config;
}

PeripheralConfig makeAdc(const char* id, uint8_t pin) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::ADC;
    config.enabled = true;
    config.pinCount = 1;
    config.pins[0] = pin;
    config.params.adc.resolution = 12;
    return config;
}

PeripheralConfig makeGenericSensor(const char* id, uint8_t sensorType,
                                   uint8_t firstPin,
                                   uint8_t secondPin = BOARD_PIN_NONE) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::SENSOR_GENERIC;
    config.enabled = true;
    config.params.sensor.sensorType = sensorType;
    config.params.sensor.sampleInterval = 1000;
    config.pinCount = secondPin == BOARD_PIN_NONE ? 1 : 2;
    config.pins[0] = firstPin;
    if (secondPin != BOARD_PIN_NONE) config.pins[1] = secondPin;
    return config;
}

PeripheralConfig makeI2cSensor(const char* id, uint8_t sda, uint8_t scl,
                               uint8_t address) {
    PeripheralConfig config = makeGenericSensor(id, 0, sda, scl);
    strcpy(config.params.sensor.driver, "BMP280");
    config.params.sensor.address = address;
    config.params.sensor.frequency = 400000;
    return config;
}

PeripheralConfig makeRfid(const char* id, uint8_t sck, uint8_t mosi,
                          uint8_t miso, uint8_t cs, uint8_t rst) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::RFID_MFRC522;
    config.enabled = true;
    config.pinCount = 5;
    config.pins[0] = sck;
    config.pins[1] = mosi;
    config.pins[2] = miso;
    config.pins[3] = cs;
    config.pins[4] = rst;
    config.params.rfid.frequency = RFID_DEFAULT_FREQUENCY;
    return config;
}

PeripheralConfig makeLcd1602(const char* id, uint8_t sda, uint8_t scl,
                             uint8_t address) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::LCD1602;
    config.enabled = true;
    config.pinCount = 2;
    config.pins[0] = sda;
    config.pins[1] = scl;
    config.params.lcd1602.cols = 16;
    config.params.lcd1602.rows = 2;
    config.params.lcd1602.address = address;
    config.params.lcd1602.backlight = true;
    return config;
}

PeripheralConfig makeDisplay(const char* id, bool touch = true) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::SCREEN_DISPLAY;
    config.enabled = true;
    initializeDisplayDefaults(config);
    strcpy(config.params.display.driver, "st7789");
    strcpy(config.params.display.touchBackend, touch ? "gt911" : "none");
    config.params.display.mosi = 23;
    config.params.display.sclk = 18;
    config.params.display.cs = 19;
    config.params.display.dc = 21;
    config.params.display.rst = 22;
    config.params.display.backlight = 17;
    config.params.display.backlightChannel = 4;
    config.params.display.touchSda = touch ? 32 : -1;
    config.params.display.touchScl = touch ? 33 : -1;
    config.params.display.touchInt = touch ? 34 : -1;
    config.params.display.touchRst = touch ? 16 : -1;
    return config;
}

PreflightResult run(const BoardProfile& profile,
                    const PeripheralConfig* existing, size_t count,
                    const PeripheralConfig& candidate,
                    PreflightOperation operation = PreflightOperation::ADD,
                    PreflightContext context = {}) {
    PreflightResult result;
    preflightPeripheralChange(profile, existing, count, candidate, operation,
                              "", context, result);
    return result;
}

void testBoardProfiles() {
    const BoardProfile& pz = getPuzhongPzEsp32Profile();
    assert(strcmp(pz.key, "puzhong-pz-esp32") == 0);
    assert(strstr(pz.version, "1.2") != nullptr);
    assert(pz.source && pz.source[0]);
    assert(pz.defaults.spiMiso == 19);
    assert(pz.defaults.spiMosi == 23);
    assert(pz.defaults.spiSck == 18);
    assert(pz.defaults.spiCs == 4);
    assert(findBoardPin(pz, 6)->policy == PinPolicy::HARD_FORBIDDEN);
    assert(findBoardPin(pz, 0)->policy == PinPolicy::CONDITIONAL);
    assert(findBoardPin(pz, 34)->policy == PinPolicy::RECOMMENDED);
    assert(!boardPinHasCapabilities(*findBoardPin(pz, 34), PIN_CAP_OUTPUT));

    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    assert(strcmp(c6.key, "esp32-c6-devkitc-1") == 0);
    assert(c6.resources.genericUartCount == 1);
    assert(c6.resources.firstGenericUartPort == 1);
    assert(c6.resources.pcntUnitCount == 4);
    assert(c6.resources.pcntChannelsPerUnit == 2);
    assert(!c6.resources.sdmmcSupported);
    assert(findBoardPin(c6, 14)->policy == PinPolicy::HARD_FORBIDDEN);
    assert(findBoardPin(c6, 24)->policy == PinPolicy::HARD_FORBIDDEN);
    assert(findBoardPin(c6, 18)->policy == PinPolicy::RECOMMENDED);
    assert(findBoardPin(c6, 8)->policy == PinPolicy::CONDITIONAL);
    assert(c6.defaults.spiMiso == 20);
    assert(c6.defaults.spiMosi == 19);
    assert(c6.defaults.spiSck == 21);
    assert(c6.defaults.spiCs == 18);
}

void testPinPoliciesAndCapabilities() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig strap = makeGpio("strap", 9);
    PreflightResult result = run(c6, nullptr, 0, strap);
    assert(!result.allowed);
    assert(result.requiresConfirmation);
    assert(result.hasIssue(PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED));

    PreflightContext acknowledged;
    acknowledged.acknowledgements = ACK_STRAPPING_PIN;
    result = run(c6, nullptr, 0, strap, PreflightOperation::ADD, acknowledged);
    assert(result.allowed);

    PeripheralConfig consolePin = makeGpio("console", 16);
    result = run(c6, nullptr, 0, consolePin);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_UART0_ACTIVE));

    PeripheralConfig usbPin = makeGpio("usb", 12);
    result = run(c6, nullptr, 0, usbPin);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_USB_SERIAL_JTAG_ACTIVE));

    const BoardProfile& pz = getPuzhongPzEsp32Profile();
    PeripheralConfig flashPin = makeGpio("flash", 6);
    result = run(pz, nullptr, 0, flashPin);
    assert(result.hasIssue(PreflightIssueCode::PIN_HARD_FORBIDDEN));

    PeripheralConfig inputOnly = makeGpio("input-only", 34);
    result = run(pz, nullptr, 0, inputOnly);
    assert(result.hasIssue(PreflightIssueCode::PIN_OUTPUT_UNSUPPORTED));

    PeripheralConfig adc2 = makeAdc("adc2", 25);
    result = run(pz, nullptr, 0, adc2);
    assert(result.hasIssue(PreflightIssueCode::ADC2_WIFI_ACK_REQUIRED));
    acknowledged.acknowledgements = ACK_ADC2_WITH_WIFI;
    result = run(pz, nullptr, 0, adc2, PreflightOperation::ADD, acknowledged);
    assert(result.allowed);
}

void testExistingRiskDiagnosticsDoNotLeakIntoCandidatePreflight() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig rgb = makeGpio("rgb_led", 8);

    PreflightResult diagnostics;
    PeripheralResourcePlanner planner(c6, PreflightContext{}, diagnostics);
    planner.addPeripheral(rgb, ClaimState::ACTIVE);
    assert(diagnostics.hasIssue(
        PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED));
    assert(diagnostics.hasIssue(
        PreflightIssueCode::PIN_BOARD_RESOURCE_ACK_REQUIRED));
    for (size_t i = 0; i < diagnostics.issueCount; ++i) {
        assert(strcmp(diagnostics.issues[i].ownerId, "rgb_led") == 0);
        assert(diagnostics.issues[i].index == 8);
    }

    PeripheralConfig safeCandidate = makeGpio("gpio10", 10, false);
    PreflightResult result = run(c6, &rgb, 1, safeCandidate);
    assert(result.allowed);
    assert(!result.requiresConfirmation);
    assert(result.issueCount == 0);
    assert(result.claimCount == 2);

    PeripheralConfig riskyCandidate = makeGpio("gpio9", 9, false);
    result = run(c6, &rgb, 1, riskyCandidate);
    assert(!result.allowed);
    assert(result.requiresConfirmation);
    assert(result.issueCount == 1);
    assert(result.issues[0].code ==
        PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED);
    assert(strcmp(result.issues[0].ownerId, "gpio9") == 0);
    assert(result.issues[0].index == 9);
}

void testI2cSharingAndAddressExclusion() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig existing[] = {makeI2c("sensor-a", 23, 22, 0x68)};

    PeripheralConfig shared = makeI2c("sensor-b", 23, 22, 0x76);
    PreflightResult result = run(c6, existing, 1, shared);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig duplicateAddress = makeI2c("sensor-b", 23, 22, 0x68);
    result = run(c6, existing, 1, duplicateAddress);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));

    PeripheralConfig wrongPins = makeI2c("sensor-b", 10, 11, 0x76);
    result = run(c6, existing, 1, wrongPins);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::I2C_BUS_PINS_MISMATCH));
}

void testSpiSharingAndCsExclusion() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig existing[] = {makeSpi("spi-a", 20, 19, 21, 18)};

    PeripheralConfig shared = makeSpi("spi-b", 20, 19, 21, 10);
    PreflightResult result = run(c6, existing, 1, shared);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig duplicateCs = makeSpi("spi-b", 20, 19, 21, 18);
    result = run(c6, existing, 1, duplicateCs);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::SPI_CS_CONFLICT));

    PeripheralConfig wrongPins = makeSpi("spi-b", 3, 2, 1, 10);
    result = run(c6, existing, 1, wrongPins);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::SPI_BUS_PINS_MISMATCH));
}

void testC6ExclusiveResourcesAndSdspiOnly() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig uartExisting[] = {makeUart("uart-a", 2, 3)};
    PeripheralConfig uartCandidate = makeUart("uart-b", 10, 11);
    PreflightResult result = run(c6, uartExisting, 1, uartCandidate);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::UART_PORT_EXHAUSTED));

    PeripheralConfig sdmmc;
    sdmmc.id = "sdmmc";
    sdmmc.type = PeripheralType::SDIO;
    sdmmc.enabled = true;
    sdmmc.pinCount = 6;
    for (uint8_t i = 0; i < 6; ++i) sdmmc.pins[i] = i;
    sdmmc.params.sdcard.interface = 0;
    PreflightContext acknowledged;
    acknowledged.acknowledgements = ACK_STRAPPING_PIN | ACK_BOARD_SHARED_PIN;
    result = run(c6, nullptr, 0, sdmmc, PreflightOperation::ADD, acknowledged);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::SDMMC_UNSUPPORTED));
}

void testLedcTimerAndChannelClaims() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig existing[] = {makePwm("pwm-a", 0, 0, 1000)};

    PeripheralConfig sameTimer = makePwm("pwm-b", 1, 1, 1000);
    PreflightResult result = run(c6, existing, 1, sameTimer);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig timerMismatch = makePwm("pwm-b", 1, 1, 5000);
    result = run(c6, existing, 1, timerMismatch);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::LEDC_TIMER_CONFLICT));

    PeripheralConfig duplicateChannel = makePwm("pwm-b", 1, 0, 1000);
    result = run(c6, existing, 1, duplicateChannel);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::LEDC_CHANNEL_CONFLICT));

    PeripheralConfig invalidChannel = makePwm("pwm-b", 1, 6, 1000);
    result = run(c6, nullptr, 0, invalidChannel);
    assert(result.hasIssue(PreflightIssueCode::LEDC_CHANNEL_INVALID));
}

void testRmtAdcAndEncoderClaims() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig pixels[] = {
        makeNeoPixel("pixel-0", 0), makeNeoPixel("pixel-1", 1),
        makeNeoPixel("pixel-2", 2), makeNeoPixel("pixel-3", 3)
    };
    PeripheralConfig fifthPixel = makeNeoPixel("pixel-4", 10);
    PreflightResult result = run(c6, pixels, 4, fifthPixel);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RMT_CHANNEL_EXHAUSTED));

    PeripheralConfig adcExisting[] = {makeAdc("adc-a", 0)};
    PeripheralConfig duplicateAdc = makeAdc("adc-b", 0);
    result = run(c6, adcExisting, 1, duplicateAdc);
    assert(result.hasIssue(PreflightIssueCode::ADC_CHANNEL_CONFLICT));

    PeripheralConfig invalidAdc = makeAdc("adc-b", 10);
    result = run(c6, nullptr, 0, invalidAdc);
    assert(result.hasIssue(PreflightIssueCode::PIN_ADC_UNSUPPORTED));

    PeripheralConfig encoders[] = {
        makeEncoder("enc-0", 0, 1), makeEncoder("enc-1", 2, 3),
        makeEncoder("enc-2", 10, 11), makeEncoder("enc-3", 18, 19)
    };
    PeripheralConfig fifthEncoder = makeEncoder("enc-4", 20, 21);
    result = run(c6, encoders, 4, fifthEncoder);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::ENCODER_SLOT_EXHAUSTED));
}

void testDormantClaimsAreVisibleButDoNotBlock() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig existing[] = {makeGpio("active", 10)};
    PeripheralConfig dormant = makeGpio("dormant", 10, false);

    PreflightResult result = run(c6, existing, 1, dormant);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::DORMANT_GPIO_CONFLICT));

    result = run(c6, existing, 1, dormant, PreflightOperation::ENABLE);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
}

void testGenericSensorPinRolesAndI2cSharing() {
    const BoardProfile& pz = getPuzhongPzEsp32Profile();

    PeripheralConfig ultrasonic = makeGenericSensor("sonar", 3, 16, 34);
    PreflightResult result = run(pz, nullptr, 0, ultrasonic);
    assert(result.allowed);

    PeripheralConfig missingEcho = makeGenericSensor("sonar", 3, 16);
    result = run(pz, nullptr, 0, missingEcho);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_REQUIRED));

    PeripheralConfig reversed = makeGenericSensor("sonar", 3, 34, 16);
    result = run(pz, nullptr, 0, reversed);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_OUTPUT_UNSUPPORTED));

    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig existing[] = {makeI2c("i2c", 10, 11, 0x68)};
    PeripheralConfig bmp = makeI2cSensor("bmp", 10, 11, 0x76);
    result = run(c6, existing, 1, bmp);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig duplicate = makeI2cSensor("bmp", 10, 11, 0x68);
    result = run(c6, existing, 1, duplicate);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));

    PeripheralConfig lcdExisting[] = {makeLcd1602("lcd", 10, 11, 0x27)};
    result = run(c6, lcdExisting, 1, bmp);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));
}

void testRfidUsesSharedSpiBusAndAllFivePins() {
    const BoardProfile& c6 = getEsp32C6DevKitC1Profile();
    PeripheralConfig existing[] = {makeSpi("spi", 20, 19, 21, 18)};
    PeripheralConfig rfid = makeRfid("rfid", 21, 19, 20, 10, 11);
    PreflightResult result = run(c6, existing, 1, rfid);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig missingRst = rfid;
    missingRst.pinCount = 4;
    result = run(c6, nullptr, 0, missingRst);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_REQUIRED));

    PeripheralConfig duplicateCs = makeRfid("rfid", 21, 19, 20, 18, 11);
    result = run(c6, existing, 1, duplicateCs);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::SPI_CS_CONFLICT));
}

void testDisplayClaimsSpiTouchAndBacklightResources() {
    const BoardProfile& pz = getPuzhongPzEsp32Profile();
    PeripheralConfig display = makeDisplay("panel");
    PreflightResult result = run(pz, nullptr, 0, display);
    assert(result.allowed);

    bool hasSpiBus = false;
    bool hasSpiCs = false;
    bool hasI2cBus = false;
    bool hasPrimaryTouchAddress = false;
    bool hasSecondaryTouchAddress = false;
    bool hasBacklightChannel = false;
    for (size_t i = 0; i < result.claimCount; ++i) {
        const ResourceClaim& claim = result.claims[i];
        hasSpiBus = hasSpiBus || claim.kind == ResourceKind::SPI_BUS;
        hasSpiCs = hasSpiCs ||
            (claim.kind == ResourceKind::SPI_CS && claim.index == 19);
        hasI2cBus = hasI2cBus || claim.kind == ResourceKind::I2C_BUS;
        hasPrimaryTouchAddress = hasPrimaryTouchAddress ||
            (claim.kind == ResourceKind::I2C_ADDRESS && claim.index == 0x5D);
        hasSecondaryTouchAddress = hasSecondaryTouchAddress ||
            (claim.kind == ResourceKind::I2C_ADDRESS && claim.index == 0x14);
        hasBacklightChannel = hasBacklightChannel ||
            (claim.kind == ResourceKind::LEDC_CHANNEL && claim.unit == 4);
    }
    assert(hasSpiBus && hasSpiCs && hasI2cBus);
    assert(hasPrimaryTouchAddress && hasSecondaryTouchAddress);
    assert(hasBacklightChannel);

    PeripheralConfig sharedI2c[] = {makeI2c("sensor", 32, 33, 0x76)};
    result = run(pz, sharedI2c, 1, display);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig primaryConflict[] = {makeI2c("touch-owner", 32, 33, 0x5D)};
    result = run(pz, primaryConflict, 1, display);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));

    PeripheralConfig secondaryConflict[] = {makeI2c("touch-owner", 32, 33, 0x14)};
    result = run(pz, secondaryConflict, 1, display);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));

    PeripheralConfig wrongI2cPins[] = {makeI2c("sensor", 25, 26, 0x76)};
    result = run(pz, wrongI2cPins, 1, display);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::I2C_BUS_PINS_MISMATCH));

    PeripheralConfig sharedSpi[] = {
        makeSpi("spi", BOARD_PIN_NONE, 23, 18, 5)
    };
    result = run(pz, sharedSpi, 1, display);
    assert(result.allowed);
    assert(result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));

    PeripheralConfig duplicateCs[] = {
        makeSpi("spi", BOARD_PIN_NONE, 23, 18, 19)
    };
    result = run(pz, duplicateCs, 1, display);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::SPI_CS_CONFLICT));

    PeripheralConfig backlightConflict[] = {makePwm("pwm", 27, 4, 5000)};
    result = run(pz, backlightConflict, 1, display);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::LEDC_CHANNEL_CONFLICT));

    PeripheralConfig missingTouchClock = display;
    missingTouchClock.params.display.touchScl = -1;
    result = run(pz, nullptr, 0, missingTouchClock);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_REQUIRED));

    PeripheralConfig duplicateNamedPin = display;
    duplicateNamedPin.params.display.dc = duplicateNamedPin.params.display.cs;
    result = run(pz, nullptr, 0, duplicateNamedPin);
    assert(!result.allowed);
    assert(result.hasIssue(PreflightIssueCode::PIN_DUPLICATED));

    PeripheralConfig displayOnly = makeDisplay("display-only", false);
    result = run(pz, nullptr, 0, displayOnly);
    assert(result.allowed);
    for (size_t i = 0; i < result.claimCount; ++i) {
        assert(result.claims[i].kind != ResourceKind::I2C_BUS);
        assert(result.claims[i].kind != ResourceKind::I2C_ADDRESS);
    }
}

void testRgbDisplaySharesI2cButOwnsAllDataAndCommandPins() {
    const BoardProfile& s3 = getEsp32S3Profile(true);
    assert(findBoardPin(s3, 46)->capabilities & PIN_CAP_OUTPUT);
    assert(findBoardPin(s3, 19)->policy == PinPolicy::CONDITIONAL);
    assert(findBoardPin(s3, 20)->policy == PinPolicy::CONDITIONAL);
    for (uint8_t pin = 22; pin <= 37; ++pin)
        assert(findBoardPin(s3, pin)->policy == PinPolicy::HARD_FORBIDDEN);
    assert(findBoardPin(getEsp32S3Profile(false), 35)->policy != PinPolicy::HARD_FORBIDDEN);

    PeripheralConfig display;
    display.id = "rgb-panel";
    display.type = PeripheralType::SCREEN_DISPLAY;
    display.enabled = true;
    initializeDisplayDefaults(display);
    auto& cfg = display.params.display;
    assert(cfg.controlBuzzerMask == 0 && cfg.controlInactiveMask == 0 &&
           cfg.controlTouchIntMask == 0 &&
           cfg.controlMasksDisjoint() && cfg.controlTouchSyncValid());
    strcpy(cfg.backend, "rgb"); strcpy(cfg.driver, "st7701");
    strcpy(cfg.controlBackend, "ch32v003"); strcpy(cfg.touchBackend, "gt911");
    cfg.width = cfg.height = 480;
    cfg.cs = 42; cfg.sclk = 2; cfg.mosi = 1;
    cfg.rgbDe = 40; cfg.rgbVsync = 39; cfg.rgbHsync = 38; cfg.rgbPclk = 41;
    const int8_t dataPins[16] = {5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17};
    memcpy(cfg.rgbDataPins, dataPins, sizeof(dataPins));
    cfg.touchSda = cfg.controlSda = 15;
    cfg.touchScl = cfg.controlScl = 7;
    cfg.controlAddress = 0x24;
    cfg.i2cFrequency = 100000;
    cfg.controlResetMask = 0x0a; cfg.controlEnableMask = 0x20;
    cfg.controlInactiveMask = 0x40;
    auto result = run(s3, nullptr, 0, display);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED));
    const auto runRgb = [&s3](const PeripheralConfig* existing, size_t count, const PeripheralConfig& candidate) {
        PreflightContext context;
        context.acknowledgements = ACK_STRAPPING_PIN;
        return run(s3, existing, count, candidate, PreflightOperation::ADD, context);
    };
    result = runRgb(nullptr, 0, display);
    assert(result.allowed);
    cfg.controlTouchIntMask=0x04;
    assert(runRgb(nullptr,0,display).allowed);
    for(uint8_t invalidMask : {uint8_t(0x06),uint8_t(0x20),uint8_t(0x40)}) {
        cfg.controlTouchIntMask=invalidMask;
        const auto invalid=runRgb(nullptr,0,display);
        assert(!invalid.allowed && invalid.hasIssue(PreflightIssueCode::DISPLAY_CONTROL_INVALID));
    }
    cfg.controlTouchIntMask=0x04;cfg.touchInt=6;
    assert(!runRgb(nullptr,0,display).allowed);
    cfg.touchInt=-1;cfg.controlTouchIntMask=0;
    for (uint8_t overlap : {uint8_t(0x02), uint8_t(0x20)}) {
        cfg.controlInactiveMask = overlap;
        result = runRgb(nullptr, 0, display);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::DISPLAY_CONTROL_INVALID));
    }
    cfg.controlBacklightMask = 0x80;
    cfg.controlInactiveMask = 0x80;
    result = runRgb(nullptr, 0, display);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::DISPLAY_CONTROL_INVALID));
    cfg.controlBacklightMask = 0;
    cfg.controlInactiveMask = 0x40;
    result = runRgb(nullptr, 0, display);
    assert(result.allowed);
    cfg.controlBuzzerMask = 0x40;
    assert(!runRgb(nullptr, 0, display).allowed);
    cfg.controlInactiveMask = 0;
    assert(runRgb(nullptr, 0, display).allowed);
    cfg.controlTouchIntMask = 0x40;
    assert(!runRgb(nullptr, 0, display).allowed);
    cfg.controlTouchIntMask = 0;
    unsigned rgbExclusive = 0, addresses = 0;
    for (size_t i = 0; i < result.claimCount; ++i) {
        const auto& claim = result.claims[i];
        if (claim.kind == ResourceKind::GPIO && claim.shareMode == ResourceShareMode::EXCLUSIVE) ++rgbExclusive;
        if (claim.kind == ResourceKind::I2C_ADDRESS) ++addresses;
        assert(claim.kind != ResourceKind::SPI_BUS); // 9-bit serial initialization is exclusive bit-banging.
    }
    assert(rgbExclusive == 23 && addresses == 3); // Sixteen RGB, four timing, three command pins.
    for (uint8_t index = 0; index < 32; ++index) {
        const int8_t pin = cfg.pinAt(index);
        if (pin < 0 || index == 6 || index == 7) continue;
        PeripheralConfig existing[] = {makeGpio("other-output", pin)};
        result = runRgb(existing, 1, display);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
        result = runRgb(&display, 1, existing[0]);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
    }
    PeripheralConfig sht30 = makeI2cSensor("sht30", 15, 7, 0x44);
    strcpy(sht30.params.sensor.driver, "SHT30"); sht30.params.sensor.frequency = 100000;
    result = runRgb(&sht30, 1, display);
    assert(result.allowed && result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));
    result = runRgb(&display, 1, sht30);
    assert(result.allowed && result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));
    {
        PeripheralConfig referencedSensor = sht30;
        referencedSensor.pinCount = 0;
        memset(referencedSensor.pins, BOARD_PIN_NONE, sizeof(referencedSensor.pins));
        strcpy(referencedSensor.params.sensor.busPeripheralId, "panel_i2c");
        // The local frequency is ignored in reference mode at runtime.
        referencedSensor.params.sensor.frequency = 400000;
        PeripheralConfig parent = makeI2c("panel_i2c", 15, 7, 0);
        parent.params.i2c.frequency = 100000;
        // The child precedes its bus in the persisted snapshot.
        PeripheralConfig existing[] = {referencedSensor, parent, display};
        PreflightContext context;
        context.acknowledgements = ACK_STRAPPING_PIN;
        PeripheralConfig clockChange = display;
        clockChange.params.display.rgbFrequency = 6000000;
        result = run(s3, existing, 3, clockChange, PreflightOperation::UPDATE, context);
        assert(result.allowed && !result.hasIssue(PreflightIssueCode::I2C_BUS_PINS_MISMATCH));
        bool inherited = false;
        for(size_t i=0;i<result.claimCount;++i) {
            const auto& claim=result.claims[i];
            if(claim.kind==ResourceKind::I2C_BUS && strcmp(claim.ownerId,"sht30")==0) {
                assert(claim.pins[0]==15 && claim.pins[1]==7 && claim.parameterA==100000);
                inherited=true;
            }
        }
        assert(inherited);
        result = run(s3, existing, 3, referencedSensor, PreflightOperation::UPDATE, context);
        assert(result.allowed);
        // Snapshot resolution also powers the resources overview endpoint.
        PreflightResult overview;
        PeripheralResourcePlanner planner(s3, context, overview, existing, 3);
        for(const auto& peripheral:existing)planner.addPeripheral(peripheral,ClaimState::ACTIVE);
        assert(overview.allowed);
        referencedSensor.params.sensor.address=0x24;
        result=run(s3,existing,3,referencedSensor,PreflightOperation::UPDATE,context);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));
        referencedSensor.params.sensor.address=0x44;
        for(int invalid=0;invalid<6;++invalid) {
            PeripheralConfig badParent=parent;
            if(invalid==0)badParent.id="missing";
            if(invalid==1)badParent.enabled=false;
            if(invalid==2)badParent.params.i2c.isMaster=false;
            if(invalid==3)badParent.params.i2c.address=0x55;
            if(invalid==4)badParent.params.i2c.frequency=1000000;
            if(invalid==5)badParent.type=PeripheralType::UART;
            result=runRgb(&badParent,1,referencedSensor);
            assert(!result.allowed && result.hasIssue(PreflightIssueCode::I2C_REFERENCE_INVALID));
        }
        referencedSensor.pinCount=2;
        result=runRgb(&parent,1,referencedSensor);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::I2C_REFERENCE_INVALID));
        // Resolving a child never mutates the persisted reference-only config.
        assert(existing[0].pinCount==0 && existing[0].pins[0]==BOARD_PIN_NONE);
    }
    for (uint8_t address : {uint8_t(0x24), uint8_t(0x5d), uint8_t(0x14)}) {
        sht30.params.sensor.address = address;
        result = runRgb(&sht30, 1, display);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));
        result = runRgb(&display, 1, sht30);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::I2C_ADDRESS_CONFLICT));
    }
    cfg.rgbDataPins[15] = cfg.rgbDataPins[0];
    result = runRgb(nullptr, 0, display);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::PIN_DUPLICATED));
}

}  // namespace

int main() {
    testBoardProfiles();
    testPinPoliciesAndCapabilities();
    testExistingRiskDiagnosticsDoNotLeakIntoCandidatePreflight();
    testI2cSharingAndAddressExclusion();
    testSpiSharingAndCsExclusion();
    testC6ExclusiveResourcesAndSdspiOnly();
    testLedcTimerAndChannelClaims();
    testRmtAdcAndEncoderClaims();
    testDormantClaimsAreVisibleButDoNotBlock();
    testGenericSensorPinRolesAndI2cSharing();
    testRfidUsesSharedSpiBusAndAllFivePins();
    testDisplayClaimsSpiTouchAndBacklightResources();
    testRgbDisplaySharesI2cButOwnsAllDataAndCommandPins();
    return 0;
}
