/**
 * @file BLEProvisionParser.cpp
 * @brief BLE 配网载荷解析（纯 JSON 逻辑，不依赖 BLE 协议栈）
 *
 * 独立于 BLE 库实现，便于 native 单元测试直接包含验证；
 * BLEProvisioner 与测试代码共用同一份解析契约。
 */

#include "network/BLEProvisionTypes.h"
#include <ArduinoJson.h>
#include <cstring>

// 历史蓝牙配网示例协议中，缺失字段会被序列化为字符串 "null"，
// 解析时按空值处理以保持兼容
static bool _isNullToken(const char* value) {
    return value == nullptr || strcmp(value, "null") == 0;
}

bool parseBleProvisionPayload(const String& json, BleProvisionPayload& out) {
    if (json.isEmpty()) {
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json.c_str());
    if (err) {
        return false;
    }
    if (!doc.is<JsonObject>()) {
        return false;
    }

    // ssid 必填：缺失、非字符串或 "null" 均视为无效
    const char* ssid = doc["ssid"] | static_cast<const char*>(nullptr);
    if (_isNullToken(ssid) || strlen(ssid) == 0) {
        return false;
    }
    out.ssid = String(ssid);

    // password 可选：缺失或 "null" 按开放网络（空密码）处理
    const char* password = doc["password"] | static_cast<const char*>(nullptr);
    out.password = _isNullToken(password) ? String("") : String(password);

    // 扩展参数（与 AP 配网 /api/wifi/config 语义一致）
    const char* userId = doc["userId"] | static_cast<const char*>(nullptr);
    if (!_isNullToken(userId) && strlen(userId) > 0) {
        out.userId = String(userId);
        out.hasUserId = true;
    }

    const char* deviceNum = doc["deviceNum"] | static_cast<const char*>(nullptr);
    if (!_isNullToken(deviceNum) && strlen(deviceNum) > 0) {
        out.deviceNum = String(deviceNum);
        out.hasDeviceNum = true;
    }

    const char* extra = doc["extra"] | static_cast<const char*>(nullptr);
    if (!_isNullToken(extra) && strlen(extra) > 0) {
        out.extra = String(extra);
        out.hasExtra = true;
    }

    return true;
}

const char* buildBleProvisionResult(bool success) {
    return success ? "true" : "false";
}
