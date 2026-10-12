#pragma once
#include <cstdint>
enum peripheral_bus_type_t { ESP32_BUS_TYPE_INIT, ESP32_BUS_TYPE_GPIO, ESP32_BUS_TYPE_LEDC, ESP32_BUS_TYPE_DAC };
peripheral_bus_type_t perimanGetPinBusType(uint8_t);
