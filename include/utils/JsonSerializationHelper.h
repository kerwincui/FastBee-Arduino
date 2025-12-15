// utils/JsonSerializationHelper.h
#ifndef JSON_SERIALIZATION_HELPER_H
#define JSON_SERIALIZATION_HELPER_H

#include <ArduinoJson.h>
#include "network/NetworkManager.h"

class JsonSerializationHelper {
public:
    /**
     * @brief 将WiFiConfig对象转换为JSON对象
     */
    static bool wifiConfigToJson(const WiFiConfig& config, JsonObject& jsonObj);
    
    /**
     * @brief 从JSON对象解析WiFiConfig
     */
    static bool wifiConfigFromJson(JsonObject& jsonObj, WiFiConfig& config);
    
    /**
     * @brief 将NetworkStatusInfo对象转换为JSON对象
     */
    static void networkStatusToJson(const NetworkStatusInfo& status, JsonObject& jsonObj);
    
    /**
     * @brief 生成完整的网络信息JSON
     */
    static String fullNetworkInfoToJson(const WiFiConfig& config, 
                                        const NetworkStatusInfo& status,
                                        DynamicJsonDocument& doc);
    
    /**
     * @brief 打印WiFiConfig的详细信息（用于调试）
     */
    static void printWiFiConfigDetails(const WiFiConfig& config);
    
    /**
     * @brief 将枚举转换为可读字符串
     */
    static String networkModeToString(NetworkMode mode);
    static String ipConfigTypeToString(IPConfigType type);
    static String ipConflictModeToString(IPConflictMode mode);
    static String networkStatusToString(NetworkStatus status);
};

#endif