#pragma once
#include <Arduino.h>
#include <functional>
#include <map>
#include <set>
#include <string>

// Byte-backed filesystem, not a JSON mock: production ArduinoJson serializes
// and reparses every byte through the same File API used by LittleFS.
struct HistoryFileSystem {
    std::map<std::string, std::string> files;
    std::set<std::string> directories;
    std::string failOpenWrite;
    std::string failRemove;
    std::string failRenameFrom;
    std::string failRenameTo;
    std::string corruptClose;
    size_t writeLimit = static_cast<size_t>(-1);
    size_t written = 0;
    size_t writeOpens = 0;
    std::function<void()> duringWrite;
    bool exists(const String& path) const {
        const std::string key(path);
        return files.count(key) || directories.count(key);
    }
    bool mkdir(const String& path) { directories.insert(std::string(path)); return true; }
    bool remove(const String& path) {
        if (std::string(path) == failRemove) return false;
        return files.erase(std::string(path)) != 0;
    }
    bool rename(const String& from, const String& to) {
        const std::string source(from), target(to);
        if (source == failRenameFrom && target == failRenameTo) return false;
        auto found = files.find(source);
        if (found == files.end() || files.count(target)) return false;
        files.emplace(target, found->second);
        files.erase(found);
        return true;
    }
    struct File open(const String& path, const char* mode);
};
inline HistoryFileSystem LittleFS;

struct File {
    std::string path;
    size_t pos = 0;
    bool valid = false;
    bool writable = false;
    explicit operator bool() const { return valid; }
    size_t size() const { return valid ? LittleFS.files.at(path).size() : 0; }
    int read() {
        if (!valid || pos >= size()) return -1;
        return static_cast<unsigned char>(LittleFS.files.at(path)[pos++]);
    }
    size_t readBytes(char* out, size_t count) {
        size_t copied = 0;
        for (; copied < count; ++copied) {
            const int value = read();
            if (value < 0) break;
            out[copied] = static_cast<char>(value);
        }
        return copied;
    }
    size_t write(uint8_t value) { return write(&value, 1); }
    size_t write(const uint8_t* bytes, size_t count) {
        if (!valid || !writable) return 0;
        if (LittleFS.duringWrite) {
            auto callback = std::move(LittleFS.duringWrite);
            LittleFS.duringWrite = {};
            callback();
        }
        const size_t remaining = LittleFS.writeLimit > LittleFS.written
            ? LittleFS.writeLimit - LittleFS.written : 0;
        const size_t accepted = std::min(count, remaining);
        LittleFS.files[path].append(reinterpret_cast<const char*>(bytes), accepted);
        LittleFS.written += accepted;
        return accepted;
    }
    void flush() {}
    void close() {
        if (valid && writable && path == LittleFS.corruptClose) LittleFS.files[path] = "{";
        valid = false;
    }
};

inline File HistoryFileSystem::open(const String& path, const char* mode) {
    const std::string key(path);
    const bool write = mode[0] == 'w';
    if (write) {
        if (key == failOpenWrite) return {};
        files[key].clear();
        ++writeOpens;
    } else if (!files.count(key)) return {};
    return {key, 0, true, write};
}
#define FILE_READ "r"
#define FILE_WRITE "w"
