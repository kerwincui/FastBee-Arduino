/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:32:23
 */

#include "systems/ConfigStorage.h"
#include <LittleFS.h>
#include <core/SystemConstants.h>

// 静态成员初始化
ConfigStorage* ConfigStorage::instance = nullptr;

ConfigStorage::ConfigStorage() {
    // 构造函数逻辑（如果需要）
}

bool ConfigStorage::initializeInternal() {
    bool success = prefs.begin(Storage::PREFERENCES_NAMESPACE, false);
    if (!success) {
        Serial.println("Failed to initialize Preferences");
        return false;
    }
    
    // 检查文件系统
    if (!LittleFS.begin(true)) {
        Serial.println("Failed to initialize LittleFS");
        return false;
    }
    
    // 创建配置目录
    if (!LittleFS.exists("/config")) {
        LittleFS.mkdir("/config");
    }
    
    return true;
}

ConfigStorage& ConfigStorage::getInstance() {
    if (instance == nullptr) {
        instance = new ConfigStorage();
    }
    return *instance;
}

bool ConfigStorage::initialize() {
    ConfigStorage& storage = getInstance();
    return storage.initializeInternal();
}

bool ConfigStorage::isInitialized() {
    return instance != nullptr;
}

bool ConfigStorage::saveString(const String& key, const String& value) {
    return prefs.putString(key.c_str(), value) > 0;
}

String ConfigStorage::readString(const String& key, const String& defaultValue) {
    return prefs.getString(key.c_str(), defaultValue);
}

bool ConfigStorage::saveInt(const String& key, int value) {
    return prefs.putInt(key.c_str(), value) > 0;
}

int ConfigStorage::readInt(const String& key, int defaultValue) {
    return prefs.getInt(key.c_str(), defaultValue);
}

bool ConfigStorage::saveBool(const String& key, bool value) {
    return prefs.putBool(key.c_str(), value) > 0;
}

bool ConfigStorage::readBool(const String& key, bool defaultValue) {
    return prefs.getBool(key.c_str(), defaultValue);
}

bool ConfigStorage::saveJSONConfig(const String& filename, const JsonDocument& config) {
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
        return false;
    }
    
    size_t bytesWritten = serializeJson(config, file);
    file.close();
    
    return bytesWritten > 0;
}

bool ConfigStorage::loadJSONConfig(const String& filename, JsonDocument& config) {
    File file = LittleFS.open(filename, FILE_READ);
    if (!file) {
        return false;
    }
    
    DeserializationError error = deserializeJson(config, file);
    file.close();
    
    return error == DeserializationError::Ok;
}

bool ConfigStorage::saveSystemConfig(const JsonDocument& config) {
    return saveJSONConfig(CONFIG_FILE_SYSTEM, config);
}

bool ConfigStorage::loadSystemConfig(JsonDocument& config) {
    return loadJSONConfig(CONFIG_FILE_SYSTEM, config);
}

bool ConfigStorage::saveNetworkConfig(const JsonDocument& config) {
    return saveJSONConfig(CONFIG_FILE_NETWORK, config);
}

bool ConfigStorage::loadNetworkConfig(JsonDocument& config) {
    return loadJSONConfig(CONFIG_FILE_NETWORK, config);
}

bool ConfigStorage::saveUserConfig(const JsonDocument& config) {
    return saveJSONConfig(CONFIG_FILE_USERS, config);
}

bool ConfigStorage::loadUserConfig(JsonDocument& config) {
    return loadJSONConfig(CONFIG_FILE_USERS, config);
}

bool ConfigStorage::saveProtocolConfig(const JsonDocument& config) {
    return saveJSONConfig(CONFIG_FILE_MQTT, config);
}

bool ConfigStorage::loadProtocolConfig(JsonDocument& config) {
    return loadJSONConfig(CONFIG_FILE_MQTT, config);
}

bool ConfigStorage::removeKey(const String& key) {
    return prefs.remove(key.c_str());
}

bool ConfigStorage::isFileSystemOK() {
    return LittleFS.begin(true);
}

bool ConfigStorage::formatFileSystem() {
    Serial.println("格式化文件系统...");
    bool result = LittleFS.format();
    LittleFS.begin(true);
    return result;
}

void ConfigStorage::listConfigFiles() {
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    
    Serial.println("配置文件列表:");
    while(file){
        Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }
}

bool ConfigStorage::deleteConfigFile(const String& filename) {
    return LittleFS.remove(filename);
}