#pragma once
#include <cstdint>
#ifndef CHIP_ETHERNET_DEDICATED_SPI
#define CHIP_ETHERNET_DEDICATED_SPI 0
#endif
inline bool chipIsAbsoluteForbiddenPin(uint8_t pin) { return pin == 30; }
