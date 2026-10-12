#pragma once
#include <Arduino.h>

namespace FastBee { namespace PeripheralUart {
// Internal hardware lookup shared by lifecycle and passive status paths.
HardwareSerial* serialForPort(uint8_t port);
uint16_t boundedUartAvailable(HardwareSerial* serial);
} }
