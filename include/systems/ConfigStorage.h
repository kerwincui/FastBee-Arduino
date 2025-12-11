#ifndef CONFIGSTORAGE_H
#define CONFIGSTORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "core/ConfigDefines.h"

class ConfigStorage {
private:
    // 单例实例指针
    static ConfigStorage* instance;
    
    Preferences prefs;
    
    // 私有构造函数
    ConfigStorage();
    
    // 防止拷贝和赋值
    ConfigStorage(const ConfigStorage&) = delete;
    ConfigStorage& operator=(const ConfigStorage&) = delete;
    
    // 内部初始化方法
    bool initializeInternal();

public:
    // 获取单例实例
    static ConfigStorage& getInstance();
    
    // 初始化配置存储（需要在setup中调用）
    static bool initialize();
    
    // 检查是否已初始化
    static bool isInitialized();
    
    // 基本数据类型存储
    bool saveString(const String& key, const String& value);
    String readString(const String& key, const String& defaultValue = "");
    bool saveInt(const String& key, int value);
    int readInt(const String& key, int defaultValue = 0);
    bool saveBool(const String& key, bool value);
    bool readBool(const String& key, bool defaultValue = false);
    
    // JSON配置存储
    bool saveJSONConfig(const String& filename, const JsonDocument& config);
    bool loadJSONConfig(const String& filename, JsonDocument& config);
    
    // 特定配置类型
    bool saveSystemConfig(const JsonDocument& config);
    bool loadSystemConfig(JsonDocument& config);
    bool saveNetworkConfig(const JsonDocument& config);
    bool loadNetworkConfig(JsonDocument& config);
    bool saveUserConfig(const JsonDocument& config);
    bool loadUserConfig(JsonDocument& config);
    bool saveProtocolConfig(const JsonDocument& config);
    bool loadProtocolConfig(JsonDocument& config);
    
    // 清理特定键值
    bool removeKey(const String& key);
    
    // 获取Preferences实例（供高级使用）
    Preferences& getPreferences() { return prefs; }
    
    // 获取文件系统状态
    bool isFileSystemOK();
    
    // 格式化文件系统（谨慎使用）
    bool formatFileSystem();
    
    // 列出配置文件
    void listConfigFiles();
    
    // 删除配置文件
    bool deleteConfigFile(const String& filename);
};

#endif