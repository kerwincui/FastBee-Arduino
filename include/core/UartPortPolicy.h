#pragma once
#include <cstdint>
#include <cstddef>
#include <initializer_list>
namespace FastBee {
constexpr uint8_t serviceUartPort(uint8_t uartCount) {
    return uartCount >= 3 ? 2 : (uartCount >= 2 ? 1 : UINT8_MAX);
}
// Existing Modbus/RS485 identifier convention, without temporary Strings.
inline bool isProtocolUartId(const char* id) {
    if (!id) return false;
    for (const char* prefix : {"modbus", "rs485"}) {
        size_t i=0;
        for (; prefix[i]; ++i) {
            char c=id[i];
            if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
            if (c!=prefix[i]) break;
        }
        if (!prefix[i]) return true;
    }
    return false;
}
}
