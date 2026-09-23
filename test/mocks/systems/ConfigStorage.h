#ifndef FASTBEE_TEST_CONFIG_STORAGE_H
#define FASTBEE_TEST_CONFIG_STORAGE_H

#include <ArduinoJson.h>
#include <LittleFS.h>

class ConfigStorage {
public:
    static ConfigStorage& getInstance() {
        static ConfigStorage instance;
        return instance;
    }

    void setSaveFailureCount(unsigned int count) {
        _saveFailureCount = count;
    }

    bool saveJSONConfig(const String& filename, const JsonDocument& config) {
        if (_saveFailureCount > 0) {
            --_saveFailureCount;
            return false;
        }
        const String tmpPath = filename + ".tmp";
        const String backupPath = filename + ".bak";
        LittleFS.remove(tmpPath);
        File tmp = LittleFS.open(tmpPath, "w");
        if (!tmp) return false;
        const size_t expected = measureJson(config);
        const size_t written = serializeJson(config, tmp);
        tmp.close();
        if (written != expected) {
            LittleFS.remove(tmpPath);
            return false;
        }

        File verify = LittleFS.open(tmpPath, "r");
        JsonDocument parsed;
        const DeserializationError error = deserializeJson(parsed, verify);
        verify.close();
        if (error) {
            LittleFS.remove(tmpPath);
            return false;
        }

        LittleFS.remove(backupPath);
        if (LittleFS.exists(filename) && !LittleFS.rename(filename, backupPath)) return false;
        return LittleFS.rename(tmpPath, filename);
    }

    bool saveFactoryJSONConfig(const String& filename, const char* jsonContent) {
        JsonDocument parsed;
        if (!jsonContent || deserializeJson(parsed, jsonContent)) return false;
        if (!saveJSONConfig(filename, parsed)) return false;
        LittleFS.remove(filename + ".bak");
        LittleFS.remove(filename + ".tmp");
        return true;
    }

    bool deleteJSONConfigGenerations(const String& filename) {
        LittleFS.remove(filename);
        LittleFS.remove(filename + ".bak");
        LittleFS.remove(filename + ".tmp");
        LittleFS.remove(filename + ".factory.tmp");
        return true;
    }

    bool loadJSONConfig(const String& filename, JsonDocument& config) {
        if (loadOne(filename, config)) return true;
        const String backupPath = filename + ".bak";
        if (!loadOne(backupPath, config)) return false;
        LittleFS.remove(filename);
        LittleFS.rename(backupPath, filename);
        return true;
    }

private:
    unsigned int _saveFailureCount = 0;

    static bool loadOne(const String& path, JsonDocument& config) {
        File file = LittleFS.open(path, "r");
        if (!file) return false;
        config.clear();
        const DeserializationError error = deserializeJson(config, file);
        file.close();
        return !error;
    }
};

#endif
