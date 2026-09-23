#pragma once
#include <Arduino.h>
#include <functional>
#include <limits>
#include <map>
#include <string>

struct ConfigFileSystem {
    std::map<std::string, std::string> files;
    std::string failOpen, failRename, failRemove;
    size_t writeLimit = std::numeric_limits<size_t>::max(), written = 0;
    std::function<void(size_t)> onRead;
    bool exists(const String& path) const { return files.count(path.c_str()) != 0; }
    bool mkdir(const String&) { return true; }
    bool remove(const String& path) {
        if (path == failRemove.c_str()) return false;
        return files.erase(path.c_str()) != 0;
    }
    bool rename(const String& from, const String& to) {
        if (from == failRename.c_str() || !exists(from)) return false;
        auto node = files.extract(from.c_str());
        files.erase(to.c_str());
        node.key() = to.c_str();
        files.insert(std::move(node));
        return true;
    }
    size_t totalBytes() const { return 1024 * 1024; }
    size_t usedBytes() const { size_t n = 0; for (const auto& f : files) n += f.second.size(); return n; }
    struct File open(const String& path, const char* mode);
};
inline ConfigFileSystem LittleFS;

struct File {
    std::string path;
    size_t pos = 0;
    bool valid = false, writable = false;
    explicit operator bool() const { return valid; }
    size_t size() const { return valid ? LittleFS.files.at(path).size() : 0; }
    size_t position() const { return pos; }
    bool seek(size_t offset, int = 0) { if (!valid || offset > size()) return false; pos = offset; return true; }
    int available() const { return valid ? static_cast<int>(size() - pos) : 0; }
    int peek() const { return valid && pos < size() ? static_cast<unsigned char>(LittleFS.files.at(path)[pos]) : -1; }
    int read() { int c = peek(); if (c >= 0) { ++pos; if (LittleFS.onRead) LittleFS.onRead(pos); } return c; }
    size_t read(uint8_t* out, size_t count) { size_t n = 0; while (n < count) { int c = read(); if (c < 0) break; out[n++] = c; } return n; }
    size_t readBytes(char* out, size_t count) { return read(reinterpret_cast<uint8_t*>(out), count); }
    size_t write(uint8_t value) { return write(&value, 1); }
    size_t write(const uint8_t* bytes, size_t count) {
        if (!valid || !writable) return 0;
        size_t accepted = std::min(count, LittleFS.writeLimit - std::min(LittleFS.writeLimit, LittleFS.written));
        LittleFS.files[path].append(reinterpret_cast<const char*>(bytes), accepted);
        LittleFS.written += accepted;
        pos += accepted;
        return accepted;
    }
    size_t print(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), strlen(text)); }
    size_t print(const String& text) { return print(text.c_str()); }
    void flush() {}
    void close() { valid = false; }
};

inline File ConfigFileSystem::open(const String& path, const char* mode) {
    const std::string key(path.c_str());
    if (key == failOpen) return {};
    const bool write = mode[0] == 'w' || mode[0] == 'a';
    if (mode[0] == 'w') files[key].clear();
    if (!write && !files.count(key)) return {};
    const size_t initial = mode[0] == 'a' ? files[key].size() : 0;
    return {key, initial, true, write};
}
#define FILE_READ "r"
#define FILE_WRITE "w"


