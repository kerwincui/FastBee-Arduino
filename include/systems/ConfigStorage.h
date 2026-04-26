#ifndef CONFIGSTORAGE_H
#define CONFIGSTORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include <freertos/semphr.h>
#include "core/SystemConstants.h"
#include "core/FeatureFlags.h"
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
    bool saveNetworkConfig  (const JsonDocument& config);
    bool loadNetworkConfig  (JsonDocument& config);
    bool saveUserConfig     (const JsonDocument& config);
    bool loadUserConfig     (JsonDocument& config);
    bool saveProtocolConfig (const JsonDocument& config);  // 统一协议配置 (protocol.json)
    bool loadProtocolConfig (JsonDocument& config);

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

#if FASTBEE_ENABLE_STORAGE_CACHE
    // RAII 缓存锁辅助类
    class CacheLock {
    public:
        CacheLock(SemaphoreHandle_t mutex) : _mutex(mutex), _locked(false) {
            if (_mutex) {
                _locked = (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
            }
        }
        ~CacheLock() {
            if (_locked && _mutex) {
                xSemaphoreGive(_mutex);
            }
        }
        bool isLocked() const { return _locked; }
        operator bool() const { return _locked; }
        CacheLock(const CacheLock&) = delete;
        CacheLock& operator=(const CacheLock&) = delete;
    private:
        SemaphoreHandle_t _mutex;
        bool _locked;
    };

    SemaphoreHandle_t _cacheMutex = nullptr;

    // 缓存条目结构
    static const size_t MAX_CONFIG_CACHE_ENTRIES = 2;

    struct ConfigCacheEntry {
        String filename;
        std::unique_ptr<JsonDocument> cachedDoc;
        unsigned long lastLoadTime = 0;      // millis()
        time_t fileModifyTime = 0;           // 文件最后修改时间
        bool dirty = false;                  // 有未保存的修改
        unsigned long debounceUntil = 0;     // debounce 截止时间
        uint8_t accessCount = 0;             // LRU 访问计数
    };

    ConfigCacheEntry _cache[MAX_CONFIG_CACHE_ENTRIES];
    size_t _cacheSize = 0;

    // 缓存操作方法
    ConfigCacheEntry* findInCache(const String& filename);
    bool evictLRUEntry();
    void updateCacheEntry(const String& filename, const JsonDocument& doc, time_t modTime);
    bool flushToDisk(const String& filename, const JsonDocument& config);
#endif // FASTBEE_ENABLE_STORAGE_CACHE

public:
#if FASTBEE_ENABLE_STORAGE_CACHE
    void flushDirtyEntries();   // 供定时任务调用
    void clearCache();          // 供 OTA 完成后调用
    void clearCache(const String& filename);  // 清除指定文件的缓存
#endif
};

#endif
