#pragma once
#include <cstdint>
#include "core/UartPortPolicy.h"

namespace FastBee {
// Same fixed controller as CellularAdapter; UART0 remains reserved.
constexpr uint8_t cellularUartPort(uint8_t uartCount) {
    return serviceUartPort(uartCount);
}
}
