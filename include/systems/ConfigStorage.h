#ifndef CONFIGSTORAGE_H
#define CONFIGSTORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "core/SystemConstants.h"
#include "core/interfaces/IConfigStorage.h"
#include "core/ErrorHandler.h"

// ============================================================================
// ConfigStorage - 配置存储管理器（Meyers' Singleton）
//
// 提供两种存储后端：
//  - NVS (Preferences)：适合频繁读写的小型键值对
//  - LittleFS JSON 文件：适合结构化配置对象
// ============================================================================
class ConfigStorage : public IConfigStorage {
public:
    // 删除拷贝构造和赋值
    ConfigStorage(const ConfigStorage&)            = delete;
    ConfigStorage& operator=(const ConfigStorage&) = delete;

    // 获取单例实例（Meyers' Singleton，线程安全）
    static ConfigStorage& getInstance();

    // 简单判断是否可用
    static bool isInitialized();
    
    // IConfigStorage 接口实现
    bool initialize() override;
    bool saveConfig(const String& key, const String& value) override;
    String readConfig(const String& key, const String& defaultValue = "") override;
    bool deleteConfig(const String& key) override;
    bool hasConfig(const String& key) override;

    // ── NVS 键值接口 ────────────────────────────────────────────────────────
    bool   saveString(const String& key, const String& value);
    String readString(const String& key, const String& defaultValue = "");
    bool   saveInt   (const String& key, int value);
    int    readInt   (const String& key, int defaultValue = 0);
    bool   saveBool  (const String& key, bool value);
    bool   readBool  (const String& key, bool defaultValue = false);
    bool   removeKey (const String& key);

    // 获取底层 Preferences 实例（供高级使用）
    Preferences& getPreferences() { return prefs; }

    // ── JSON 文件接口 ────────────────────────────────────────────────────────
    bool saveJSONConfig(const String& filename, const JsonDocument& config);
    bool loadJSONConfig(const String& filename, JsonDocument& config);

    // 特定配置类型快捷接口
    bool saveSystemConfig  (const JsonDocument& config);
    bool loadSystemConfig  (JsonDocument& config);
    bool saveNetworkConfig (const JsonDocument& config);
    bool loadNetworkConfig (JsonDocument& config);
    bool saveUserConfig    (const JsonDocument& config);
    bool loadUserConfig    (JsonDocument& config);
    // 各协议独立存储接口（原 saveProtocolConfig 只能存 MQTT，已拆分为以下接口）
    bool saveMQTTConfig    (const JsonDocument& config);
    bool loadMQTTConfig    (JsonDocument& config);
    bool saveModbusConfig  (const JsonDocument& config);
    bool loadModbusConfig  (JsonDocument& config);
    bool saveTCPConfig     (const JsonDocument& config);
    bool loadTCPConfig     (JsonDocument& config);
    bool saveHTTPConfig    (const JsonDocument& config);
    bool loadHTTPConfig    (JsonDocument& config);
    bool saveCoAPConfig    (const JsonDocument& config);
    bool loadCoAPConfig    (JsonDocument& config);

    // ── 文件系统管理 ────────────────────────────────────────────────────────
    bool isFileSystemOK();
    bool formatFileSystem();       // 谨慎使用：会清除所有数据
    void listConfigFiles();
    bool deleteConfigFile(const String& filename);

private:
    ConfigStorage();  // 私有构造（Meyers' Singleton）
    bool initializeInternal();

    Preferences prefs;
    bool _initialized = false;  // 真实初始化标志，不再造假
};

#endif
