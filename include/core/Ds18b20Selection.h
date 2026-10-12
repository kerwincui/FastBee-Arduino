#pragma once
#include <ArduinoJson.h>
#include "core/Ds18b20Address.h"

namespace FastBee {
// Shared by saved/imported rule admission and the actual sampling entry.
inline const char* parseDs18b20Selection(JsonVariantConst parameters, uint8_t& index,
                                       uint8_t* address, bool& addressed) {
    index = 0;
    addressed = false;
    const auto selectedIndex = parameters["deviceIndex"];
    if (!selectedIndex.isNull()) {
        if (!selectedIndex.is<unsigned>() || selectedIndex.as<unsigned>() > 15)
            return "DS18B20_INDEX_INVALID";
        index = static_cast<uint8_t>(selectedIndex.as<unsigned>());
    }
    const auto selectedAddress = parameters["deviceAddress"];
    if (selectedAddress.isNull()) return nullptr; // Legacy index selection.
    if (!selectedAddress.is<const char*>()) return "DS18B20_ROM_INVALID";
    const auto text = selectedAddress.as<JsonString>();
    if (!text.size()) return nullptr;
    if (!parseDs18b20Address(text.c_str(), text.size(), address)) return "DS18B20_ROM_INVALID";
    addressed = true;
    return nullptr;
}
} // namespace FastBee
