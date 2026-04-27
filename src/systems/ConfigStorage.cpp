/**
 * @description: 配置存储系统实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:23
 *
 * 优化说明：
 *  1. 单例由裸指针改为局部静态实例（Meyers' Singleton），线程安全且无内存泄漏
 *  2. 所有 Serial.println 替换为 LoggerSystem 调用
 *  3. isFileSystemOK() 不再重复 begin()，改为检测挂载状态
 *  4. formatFileSystem() / listConfigFiles() 使用 LOG 宏统一输出
 */

#include "systems/ConfigStorage.h"
#include "systems/LoggerSystem.h"
#include <LittleFS.h>
#include <core/SystemConstants.h>

// ── 单例（Meyers' Singleton，无裸指针，线程安全）────────────────────────────
ConfigStorage& ConfigStorage::getInstance() {
    static ConfigStorage instance;
    return instance;
}

bool ConfigStorage::isInitialized() {
    return getInstance()._initialized;
}

// ── 构造 / 内部初始化 ────────────────────────────────────────────────────────

ConfigStorage::ConfigStorage() = default;

bool ConfigStorage::initializeInternal() {
    if (_initialized) return true;

    if (!prefs.begin(Storage::PREFERENCES_NAMESPACE, false)) {
        LOG_ERROR("ConfigStorage: Failed to initialize Preferences");
        return false;
    }

    if (!LittleFS.begin(true)) {
        LOG_ERROR("ConfigStorage: Failed to initialize LittleFS");
        return false;
    }

    // 确保必要目录存在（首次启动或文件系统格式化后自动创建）
    const char* requiredDirs[] = { "/config", "/logs", "/www" };
    for (const char* dir : requiredDirs) {
        if (!LittleFS.exists(dir)) {
            LittleFS.mkdir(dir);
        }
    }

#if FASTBEE_ENABLE_STORAGE_CACHE
    if (!_cacheMutex) {
        _cacheMutex = xSemaphoreCreateMutex();
    }
#endif

    _initialized = true;
    LOG_INFO("ConfigStorage: Initialized");
    return true;
}

// ── NVS 键值接口 ─────────────────────────────────────────────────────────────

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

bool ConfigStorage::removeKey(const String& key) {
    return prefs.remove(key.c_str());
}

// ── JSON 文件接口 ─────────────────────────────────────────────────────────────

bool ConfigStorage::saveJSONConfig(const String& filename, const JsonDocument& config) {
#if FASTBEE_ENABLE_STORAGE_CACHE
    // 判断是否为关键配置（需立即写入）
    bool isCritical = (filename.indexOf("protocol") >= 0 || filename.indexOf("network") >= 0);

    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            // 更新缓存
            updateCacheEntry(filename, config, 0);  // modTime=0 表示待写入

            if (!isCritical) {
                // 非关键配置：标记为 dirty，5秒后通过定时任务写入
                ConfigCacheEntry* entry = findInCache(filename);
                if (entry) {
                    entry->dirty = true;
                    entry->debounceUntil = millis() + 3000;
                }
                return true;  // 延迟写入
            }
        }
    }

    // 关键配置：立即写入（文件 I/O 不需要锁）
    bool result = flushToDisk(filename, config);
    if (result) {
        CacheLock lock(_cacheMutex);
        if (lock) {
            ConfigCacheEntry* entry = findInCache(filename);
            if (entry) {
                entry->dirty = false;
            }
        }
    }
    return result;
#else
    // 未启用缓存：原有逻辑
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for write", filename.c_str());
        LOG_ERROR(buf);
        return false;
    }

    size_t written = serializeJson(config, file);
    file.close();
    return written > 0;
#endif
}

bool ConfigStorage::loadJSONConfig(const String& filename, JsonDocument& config) {
#if FASTBEE_ENABLE_STORAGE_CACHE
    // 1. 查询缓存（加锁）
    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            ConfigCacheEntry* entry = findInCache(filename);
            if (entry && entry->cachedDoc && entry->lastLoadTime > 0) {
                // 检查文件是否被外部修改
                File checkFile = LittleFS.open(filename, FILE_READ);
                if (checkFile) {
                    time_t fileTime = checkFile.getLastWrite();
                    checkFile.close();
                    if (fileTime <= entry->fileModifyTime) {
                        // 缓存命中
                        config = *entry->cachedDoc;
                        entry->accessCount++;
                        return true;
                    }
                }
            }
        }
    }
#endif

    // 文件 I/O（不需要锁）
    File file = LittleFS.open(filename, FILE_READ);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for read", filename.c_str());
        LOG_WARNING(buf);
        return false;
    }

    DeserializationError err = deserializeJson(config, file);
    time_t modTime = file.getLastWrite();
    file.close();

    if (err) {
        char buf[80];
        snprintf(buf, sizeof(buf), "ConfigStorage: JSON parse error in %s: %s",
                 filename.c_str(), err.c_str());
        LOG_ERROR(buf);
        return false;
    }

#if FASTBEE_ENABLE_STORAGE_CACHE
    // 更新缓存（加锁）
    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            updateCacheEntry(filename, config, modTime);
        }
    }
#endif

    return true;
}

// ── 特定配置快捷接口 ─────────────────────────────────────────────────────────

bool ConfigStorage::saveNetworkConfig(const JsonDocument& config)   { return saveJSONConfig(FileSystem::NETWORK_CONFIG_FILE,   config); }
bool ConfigStorage::loadNetworkConfig(JsonDocument& config)         { return loadJSONConfig(FileSystem::NETWORK_CONFIG_FILE,   config); }
bool ConfigStorage::saveUserConfig(const JsonDocument& config)      { return saveJSONConfig(FileSystem::USER_CONFIG_FILE,      config); }
bool ConfigStorage::loadUserConfig(JsonDocument& config)            { return loadJSONConfig(FileSystem::USER_CONFIG_FILE,      config); }
bool ConfigStorage::saveProtocolConfig(const JsonDocument& config)  { return saveJSONConfig(FileSystem::PROTOCOL_CONFIG_FILE,  config); }
bool ConfigStorage::loadProtocolConfig(JsonDocument& config)        { return loadJSONConfig(FileSystem::PROTOCOL_CONFIG_FILE,  config); }

// ── IConfigStorage 接口实现 ──────────────────────────────────────────────────

bool ConfigStorage::initialize() {
    return initializeInternal();
}

bool ConfigStorage::saveConfig(const String& key, const String& value) {
    return saveString(key, value);
}

String ConfigStorage::readConfig(const String& key, const String& defaultValue) {
    return readString(key, defaultValue);
}

bool ConfigStorage::deleteConfig(const String& key) {
    return removeKey(key);
}

bool ConfigStorage::hasConfig(const String& key) {
    return prefs.isKey(key.c_str());
}

// ── 文件系统管理 ─────────────────────────────────────────────────────────────

bool ConfigStorage::isFileSystemOK() {
    // 已挂载时 totalBytes() > 0
    return (LittleFS.totalBytes() > 0);
}

bool ConfigStorage::formatFileSystem() {
    LOG_WARNING("ConfigStorage: Formatting file system...");
    bool result = LittleFS.format();
    if (result) {
        LittleFS.begin(true);
        LOG_INFO("ConfigStorage: File system formatted");
    } else {
        LOG_ERROR("ConfigStorage: Format failed");
    }
    return result;
}

void ConfigStorage::listConfigFiles() {
    File root = LittleFS.open("/");
    if (!root) {
        LOG_ERROR("ConfigStorage: Cannot open root directory");
        return;
    }

    LOG_INFO("ConfigStorage: File list:");
    File file = root.openNextFile();
    while (file) {
        char buf[80];
        snprintf(buf, sizeof(buf), "  %s  (%u bytes)", file.name(), (unsigned)file.size());
        LOG_INFO(buf);
        file = root.openNextFile();
    }
}

bool ConfigStorage::deleteConfigFile(const String& filename) {
#if FASTBEE_ENABLE_STORAGE_CACHE
    // 删除文件时也清除对应缓存（不 flush）
    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            ConfigCacheEntry* entry = findInCache(filename);
            if (entry) {
                entry->dirty = false;  // 不需要 flush，文件将被删除
                size_t idx = entry - _cache;
                if (idx < _cacheSize - 1) {
                    _cache[idx] = std::move(_cache[_cacheSize - 1]);
                }
                _cacheSize--;
            }
        }
    }
#endif
    return LittleFS.remove(filename);
}

// ── 缓存辅助方法 ──────────────────────────────────────────────────────────────

#if FASTBEE_ENABLE_STORAGE_CACHE

ConfigStorage::ConfigCacheEntry* ConfigStorage::findInCache(const String& filename) {
    for (size_t i = 0; i < _cacheSize; i++) {
        if (_cache[i].filename == filename) {
            return &_cache[i];
        }
    }
    return nullptr;
}

bool ConfigStorage::evictLRUEntry() {
    if (_cacheSize == 0) return false;

    // 找到访问次数最少的非 dirty 条目
    size_t minIdx = 0;
    uint8_t minCount = 255;
    bool foundClean = false;
    for (size_t i = 0; i < _cacheSize; i++) {
        if (!_cache[i].dirty && _cache[i].accessCount < minCount) {
            minCount = _cache[i].accessCount;
            minIdx = i;
            foundClean = true;
        }
    }

    // 如果没有干净的条目，找访问次数最少的 dirty 条目
    if (!foundClean) {
        minCount = 255;
        for (size_t i = 0; i < _cacheSize; i++) {
            if (_cache[i].accessCount < minCount) {
                minCount = _cache[i].accessCount;
                minIdx = i;
            }
        }
    }

    // 如果找到的条目是 dirty 的，先 flush
    if (_cache[minIdx].dirty && _cache[minIdx].cachedDoc) {
        flushToDisk(_cache[minIdx].filename, *_cache[minIdx].cachedDoc);
    }

    // 移除：将最后一个条目移到被淘汰的位置
    if (minIdx < _cacheSize - 1) {
        _cache[minIdx] = std::move(_cache[_cacheSize - 1]);
    }
    _cacheSize--;
    return true;
}

void ConfigStorage::updateCacheEntry(const String& filename, const JsonDocument& doc, time_t modTime) {
    ConfigCacheEntry* entry = findInCache(filename);
    if (!entry) {
        // 缓存已满，淘汰一个
        if (_cacheSize >= MAX_CONFIG_CACHE_ENTRIES) {
            evictLRUEntry();
        }
        entry = &_cache[_cacheSize++];
        entry->filename = filename;
        entry->accessCount = 0;
    }
    entry->cachedDoc.reset(new JsonDocument(doc));
    entry->lastLoadTime = millis();
    entry->fileModifyTime = modTime;
    entry->accessCount++;
}

bool ConfigStorage::flushToDisk(const String& filename, const JsonDocument& config) {
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for write", filename.c_str());
        LOG_ERROR(buf);
        return false;
    }
    size_t written = serializeJson(config, file);
    file.close();
    return written > 0;
}

void ConfigStorage::flushDirtyEntries() {
    CacheLock lock(_cacheMutex);
    if (!lock) return;

    unsigned long now = millis();
    for (size_t i = 0; i < _cacheSize; i++) {
        if (_cache[i].dirty && now >= _cache[i].debounceUntil && _cache[i].cachedDoc) {
            if (flushToDisk(_cache[i].filename, *_cache[i].cachedDoc)) {
                _cache[i].dirty = false;
                _cache[i].fileModifyTime = 0; // 下次 load 时会刷新
            }
        }
    }
}

void ConfigStorage::clearCache() {
    CacheLock lock(_cacheMutex);
    if (!lock) return;

    for (size_t i = 0; i < _cacheSize; i++) {
        if (_cache[i].dirty && _cache[i].cachedDoc) {
            flushToDisk(_cache[i].filename, *_cache[i].cachedDoc);
        }
        _cache[i].cachedDoc.reset();
        _cache[i].filename = "";
    }
    _cacheSize = 0;
}

void ConfigStorage::clearCache(const String& filename) {
    CacheLock lock(_cacheMutex);
    if (!lock) return;

    ConfigCacheEntry* entry = findInCache(filename);
    if (entry) {
        if (entry->dirty && entry->cachedDoc) {
            flushToDisk(entry->filename, *entry->cachedDoc);
        }
        // 移除条目
        size_t idx = entry - _cache;
        if (idx < _cacheSize - 1) {
            _cache[idx] = std::move(_cache[_cacheSize - 1]);
        }
        _cacheSize--;
    }
}

#endif // FASTBEE_ENABLE_STORAGE_CACHE
