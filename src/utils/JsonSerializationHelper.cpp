// utils/JsonSerializationHelper.cpp
#include "utils/JsonSerializationHelper.h"
#include <ArduinoJson.h>
#include "network/NetworkManager.h"
#include <WiFi.h>
#include "utils/JsonConverters.h"

bool JsonSerializationHelper::wifiConfigToJson(const WiFiConfig& config, JsonObject& jsonObj) {
    try {
        // 基本配置
        jsonObj["deviceName"] = config.deviceName;
        jsonObj["mode"] = static_cast<uint8_t>(config.mode);
        jsonObj["apSSID"] = config.apSSID;
        jsonObj["apPassword"] = config.apPassword;
        jsonObj["apChannel"] = config.apChannel;
        jsonObj["apHidden"] = config.apHidden;
        jsonObj["staSSID"] = config.staSSID;
        jsonObj["staPassword"] = config.staPassword;
        
        // IP配置
        jsonObj["ipConfigType"] = static_cast<uint8_t>(config.ipConfigType);
        jsonObj["staticIP"] = config.staticIP;
        jsonObj["gateway"] = config.gateway;
        jsonObj["subnet"] = config.subnet;
        jsonObj["dns1"] = config.dns1;
        jsonObj["dns2"] = config.dns2;
        
        // 高级配置
        jsonObj["enableMDNS"] = config.enableMDNS;
        jsonObj["customDomain"] = config.customDomain;
        jsonObj["enableOTA"] = config.enableOTA;
        jsonObj["enableWebServer"] = config.enableWebServer;
        
        // 容错配置 - 根据错误信息调整：
        // conflictDetection 是 IPConflictMode 类型
        // conflictMode 是 bool 类型
        jsonObj["conflictDetection"] = static_cast<uint8_t>(config.conflictDetection); // IPConflictMode枚举
        jsonObj["conflictMode"] = config.conflictMode; // bool类型
        
        // 备用IP列表
        JsonArray backupIPs = jsonObj.createNestedArray("backupIPs");
        for (const String& ip : config.backupIPs) {
            backupIPs.add(ip);
        }
        
        return true;
    } catch (const std::exception& e) {
        Serial.printf("[JsonSerializationHelper] wifiConfigToJson异常: %s\n", e.what());
        return false;
    }
}

bool JsonSerializationHelper::wifiConfigFromJson(JsonObject& jsonObj, WiFiConfig& config) {
    try {
        // 基本配置
        if (jsonObj.containsKey("deviceName")) config.deviceName = jsonObj["deviceName"].as<String>();
        if (jsonObj.containsKey("mode")) config.mode = static_cast<NetworkMode>(jsonObj["mode"].as<uint8_t>());
        if (jsonObj.containsKey("apSSID")) config.apSSID = jsonObj["apSSID"].as<String>();
        if (jsonObj.containsKey("apPassword")) config.apPassword = jsonObj["apPassword"].as<String>();
        if (jsonObj.containsKey("apChannel")) config.apChannel = jsonObj["apChannel"].as<int>();
        if (jsonObj.containsKey("apHidden")) config.apHidden = jsonObj["apHidden"].as<bool>();
        if (jsonObj.containsKey("staSSID")) config.staSSID = jsonObj["staSSID"].as<String>();
        if (jsonObj.containsKey("staPassword")) config.staPassword = jsonObj["staPassword"].as<String>();
        
        // IP配置
        if (jsonObj.containsKey("ipConfigType")) {
            config.ipConfigType = static_cast<IPConfigType>(jsonObj["ipConfigType"].as<uint8_t>());
        }
        if (jsonObj.containsKey("staticIP")) config.staticIP = jsonObj["staticIP"].as<String>();
        if (jsonObj.containsKey("gateway")) config.gateway = jsonObj["gateway"].as<String>();
        if (jsonObj.containsKey("subnet")) config.subnet = jsonObj["subnet"].as<String>();
        if (jsonObj.containsKey("dns1")) config.dns1 = jsonObj["dns1"].as<String>();
        if (jsonObj.containsKey("dns2")) config.dns2 = jsonObj["dns2"].as<String>();
        
        // 高级配置
        if (jsonObj.containsKey("enableMDNS")) config.enableMDNS = jsonObj["enableMDNS"].as<bool>();
        if (jsonObj.containsKey("customDomain")) config.customDomain = jsonObj["customDomain"].as<String>();
        if (jsonObj.containsKey("enableOTA")) config.enableOTA = jsonObj["enableOTA"].as<bool>();
        if (jsonObj.containsKey("enableWebServer")) config.enableWebServer = jsonObj["enableWebServer"].as<bool>();
        
        // 容错配置 - 根据错误信息调整：
        if (jsonObj.containsKey("conflictDetection")) {
            config.conflictDetection = static_cast<IPConflictMode>(jsonObj["conflictDetection"].as<uint8_t>()); // IPConflictMode枚举
        }
        if (jsonObj.containsKey("conflictMode")) {
            config.conflictMode = jsonObj["conflictMode"].as<bool>(); // bool类型
        }
        if (jsonObj.containsKey("autoFailover")) config.autoFailover = jsonObj["autoFailover"].as<bool>();
        
        // 备用IP列表
        if (jsonObj.containsKey("backupIPs") && jsonObj["backupIPs"].is<JsonArray>()) {
            config.backupIPs.clear();
            JsonArray backupIPs = jsonObj["backupIPs"].as<JsonArray>();
            for (JsonVariant ip : backupIPs) {
                config.backupIPs.push_back(ip.as<String>());
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        Serial.printf("[JsonSerializationHelper] wifiConfigFromJson异常: %s\n", e.what());
        return false;
    }
}

String JsonSerializationHelper::networkModeToString(NetworkMode mode) {
    // 使用整数值进行比较，避免宏冲突
    uint8_t modeValue = static_cast<uint8_t>(mode);
    
    switch(modeValue) {
        case 0: return "关闭";          // WIFI_OFF / WIFI_MODE_NULL
        case 1: return "站点模式";      // WIFI_STA / WIFI_MODE_STA
        case 2: return "热点模式";      // WIFI_AP / WIFI_MODE_AP
        case 3: return "混合模式";      // WIFI_AP_STA / WIFI_MODE_APSTA
        default: return "未知";
    }
}

String JsonSerializationHelper::ipConfigTypeToString(IPConfigType type) {
    switch(type) {
        case IPConfigType::DHCP: return "DHCP";
        case IPConfigType::STATIC: return "静态IP";
        default: return "未知";
    }
}

String JsonSerializationHelper::networkStatusToString(NetworkStatus status) {
    switch(status) {
        case NetworkStatus::DISCONNECTED: return "未连接";
        case NetworkStatus::CONNECTING: return "连接中";
        case NetworkStatus::CONNECTED: return "已连接";
        case NetworkStatus::AP_MODE: return "热点模式";
        case NetworkStatus::CONNECTION_FAILED: return "连接失败";
        case NetworkStatus::IP_CONFLICT: return "IP冲突";
        case NetworkStatus::FAILOVER_IN_PROGRESS: return "故障转移中";
        default: return "未知";
    }
}

void JsonSerializationHelper::networkStatusToJson(const NetworkStatusInfo& status, JsonObject& jsonObj) {
    jsonObj["status"] = static_cast<uint8_t>(status.status);
    jsonObj["statusText"] = networkStatusToString(status.status);
    jsonObj["ipAddress"] = status.ipAddress;
    jsonObj["macAddress"] = status.macAddress;
    jsonObj["ssid"] = status.ssid;
    jsonObj["rssi"] = status.rssi;
    jsonObj["signalStrength"] = status.signalStrength;
    jsonObj["gateway"] = status.currentGateway;
    jsonObj["subnet"] = status.currentSubnet;
    jsonObj["dnsServer"] = status.dnsServer;
    jsonObj["internetAvailable"] = status.internetAvailable;
    jsonObj["failoverCount"] = status.failoverCount;
    jsonObj["uptime"] = status.uptime;
    jsonObj["lastConnectionTime"] = status.lastConnectionTime;
}

String JsonSerializationHelper::fullNetworkInfoToJson(const WiFiConfig& config, 
                                                      const NetworkStatusInfo& status,
                                                      DynamicJsonDocument& doc) {
    JsonObject root = doc.to<JsonObject>();
    
    // 配置部分
    JsonObject configObj = root.createNestedObject("config");
    wifiConfigToJson(config, configObj);
    
    // 状态部分
    JsonObject statusObj = root.createNestedObject("status");
    networkStatusToJson(status, statusObj);
    
    // 可读字符串表示
    root["modeText"] = networkModeToString(config.mode);
    root["ipConfigTypeText"] = ipConfigTypeToString(config.ipConfigType);
    root["statusText"] = networkStatusToString(status.status);
    
    // 系统信息
    root["timestamp"] = millis();
    root["freeHeap"] = ESP.getFreeHeap();
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}