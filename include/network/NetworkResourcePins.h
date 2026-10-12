#pragma once
#include "network/WiFiManager.h"
#include <ArduinoJson.h>
#include <cstdlib>

namespace FastBee {
inline bool parseNetworkResourcePin(JsonVariantConst value, bool optional, int8_t& pin) {
    long parsed = 0;
    if (value.is<int>()) {
        parsed = value.as<int>();
    } else if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        if (!text || !*text) return false;
        const char* digit = text;
        if (*digit == '-') ++digit;
        if (!*digit) return false;
        // Reject whitespace, decimals, exponents, bool/null and overflow before narrowing.
        size_t length = 0;
        for (; digit[length]; ++length) {
            if (length >= 3 || digit[length] < '0' || digit[length] > '9') return false;
        }
        parsed = std::strtol(text, nullptr, 10);
    } else {
        return false;
    }
    if (parsed < (optional ? -1 : 0) || parsed > 127) return false;
    pin = static_cast<int8_t>(parsed);
    return true;
}
inline bool parseEthernetResourcePins(JsonObjectConst object, EthernetConfig& destination, String& error) {
    EthernetConfig candidate = destination;
    struct Field { const char* name; int8_t* pin; bool optional; };
    const Field fields[] = {{"spiMiso", &candidate.spiMiso, false},
        {"spiMosi", &candidate.spiMosi, false}, {"spiSck", &candidate.spiSck, false},
        {"csPin", &candidate.csPin, false}, {"rstPin", &candidate.rstPin, true},
        {"intPin", &candidate.intPin, true}};
    for (const auto& field : fields) {
        if (!object.containsKey(field.name)) continue;
        if (!parseNetworkResourcePin(object[field.name], field.optional, *field.pin)) {
            error = String("NETWORK_PIN_INVALID: ") + field.name;
            return false;
        }
    }
    destination = candidate;
    error = "";
    return true;
}
inline bool parseCellularResourcePins(JsonObjectConst object, CellularConfig& destination, String& error) {
    CellularConfig candidate = destination;
    struct Field { const char* name; int8_t* pin; bool optional; };
    const Field fields[] = {{"rxPin", &candidate.rxPin, false},
        {"txPin", &candidate.txPin, false}, {"pwrPin", &candidate.pwrPin, true}};
    for (const auto& field : fields) {
        if (!object.containsKey(field.name)) continue;
        if (!parseNetworkResourcePin(object[field.name], field.optional, *field.pin)) {
            error = String("NETWORK_PIN_INVALID: cellular.") + field.name;
            return false;
        }
    }
    destination = candidate;
    error = "";
    return true;
}
}
