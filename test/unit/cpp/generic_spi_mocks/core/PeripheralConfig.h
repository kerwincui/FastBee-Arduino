#pragma once
#include <Arduino.h>
#include <strings.h>
enum class PeripheralType { SPI, RFID_MFRC522, SDIO, SCREEN_DISPLAY, GPIO };
struct PeripheralConfig {
    String id = "spi-a";
    PeripheralType type = PeripheralType::SPI;
    bool enabled = true;
    uint8_t pinCount = 4, pins[4] = {MISO, MOSI, SCK, 5};
    struct {
        struct { uint32_t frequency = 0; bool msbFirst = true; uint8_t mode = 0; } spi;
        struct { uint8_t interface = 1; } sdcard;
        struct { char backend[16] = "spi_tft"; int cs = 12; } display;
    } params;
};
