#ifndef JSON_CONVERTERS_H
#define JSON_CONVERTERS_H

#include <ArduinoJson.h>
#include "network/NetworkManager.h"

// 这些函数使ArduinoJson能够序列化您的自定义枚举类型
namespace ArduinoJson {
    // 为NetworkMode添加序列化支持
    inline void convertToJson(const NetworkMode& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    
    // 为IPConfigType添加序列化支持
    inline void convertToJson(const IPConfigType& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    
    // 为IPConflictMode添加序列化支持
    inline void convertToJson(const IPConflictMode& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
    
    // 为NetworkStatus添加序列化支持
    inline void convertToJson(const NetworkStatus& src, JsonVariant dst) {
        dst.set(static_cast<uint8_t>(src));
    }
}

#endif