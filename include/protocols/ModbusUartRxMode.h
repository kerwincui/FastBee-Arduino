#pragma once

#if defined(ESP32)
#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#endif

namespace FastBee {

inline bool configureModbusReceiveOnlyPin(int rxPin, int txPin) {
    if (rxPin < 0 || txPin < 0 || rxPin == txPin) return false;
#if defined(ESP32)
    if (!GPIO_IS_VALID_GPIO(rxPin) || !GPIO_IS_VALID_OUTPUT_GPIO(txPin)) return false;
    // A boot-console TX pin can retain output enable after UART RX attachment.
    // Clear its drive without changing the selected IOMUX or UART input matrix.
    gpio_ll_output_disable(&GPIO, static_cast<uint32_t>(rxPin));
    gpio_ll_set_output_enable_ctrl(&GPIO, static_cast<uint8_t>(rxPin), false, false);
#endif
    return true;
}

} // namespace FastBee
