#ifndef JSON_CONVERTERS_H
#define JSON_CONVERTERS_H

#include <ArduinoJson.h>
#include "network/NetworkManager.h"

// 这些函数使ArduinoJson能够序列化/反序列化自定义枚举类型
namespace ArduinoJson {
    // NetworkMode
    inline void convertToJson(const NetworkMode& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    inline void convertFromJson(JsonVariantConst src, NetworkMode& dst) {
        int v = src.as<int>();
        dst = (v >= 0 && v <= 2) ? static_cast<NetworkMode>(v) : NetworkMode::NETWORK_STA;
    }
    
    // IPConfigType
    inline void convertToJson(const IPConfigType& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    inline void convertFromJson(JsonVariantConst src, IPConfigType& dst) {
        int v = src.as<int>();
        dst = (v == 0 || v == 1) ? static_cast<IPConfigType>(v) : IPConfigType::DHCP;
    }
    
    // IPConflictMode
    inline void convertToJson(const IPConflictMode& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    inline void convertFromJson(JsonVariantConst src, IPConflictMode& dst) {
        int v = src.as<int>();
        dst = (v >= 0 && v <= 2) ? static_cast<IPConflictMode>(v) : IPConflictMode::ARP;
    }
    
    // NetworkStatus
    inline void convertToJson(const NetworkStatus& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    inline void convertFromJson(JsonVariantConst src, NetworkStatus& dst) {
        int v = src.as<int>();
        dst = (v >= 0 && v <= 6) ? static_cast<NetworkStatus>(v) : NetworkStatus::DISCONNECTED;
    }
}

#endif