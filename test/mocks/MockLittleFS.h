#ifndef MOCK_LITTLEFS_H
#define MOCK_LITTLEFS_H

#include <Arduino.h>
#include <ctime>
#include <map>

inline std::map<String, String> g_mockFiles;
inline std::map<String, time_t> g_mockFileTimes;

class MockFile {
public:
    MockFile() = default;

    MockFile(const String& path, const String& mode = "r")
        : _path(path), _open(true), _write(mode.indexOf('w') >= 0 || mode.indexOf('a') >= 0),
          _directory(isDirectoryPath(path)) {
        if (_directory) {
            buildDirectoryEntries(path);
            return;
        }
        auto it = g_mockFiles.find(path);
        if (mode.indexOf('w') >= 0) {
            // Write mode: truncate existing content (matches real FILE_WRITE behavior)
            _content = "";
        } else if (it != g_mockFiles.end()) {
            _content = it->second;
        }
        if (mode.indexOf('a') >= 0) {
            // Append mode: load existing and position at end
            if (it != g_mockFiles.end()) _content = it->second;
            _pos = _content.length();
        }
    }

    size_t write(const uint8_t* buffer, size_t size) {
        if (!_open || !_write || !buffer) return 0;
        for (size_t i = 0; i < size; ++i) _content += static_cast<char>(buffer[i]);
        return size;
    }

    size_t write(uint8_t value) {
        return write(&value, 1);
    }

    size_t print(const char* value) {
        if (!value) return 0;
        return write(reinterpret_cast<const uint8_t*>(value), std::strlen(value));
    }

    size_t print(const String& value) {
        return print(value.c_str());
    }

    int read() {
        if (!_open || _pos >= _content.length()) return -1;
        return static_cast<uint8_t>(_content[_pos++]);
    }

    size_t read(uint8_t* buffer, size_t size) {
        if (!_open || !buffer) return 0;
        size_t availableBytes = available();
        size_t toRead = std::min(size, availableBytes);
        for (size_t i = 0; i < toRead; ++i) buffer[i] = static_cast<uint8_t>(_content[_pos++]);
        return toRead;
    }

    int available() const {
        if (!_open || _pos >= _content.length()) return 0;
        return static_cast<int>(_content.length() - _pos);
    }

    String readString() {
        if (!_open || _pos >= _content.length()) return "";
        String result = _content.substr(_pos);
        _pos = _content.length();
        return result;
    }

    void close() {
        if (_open && _write && !_directory) {
            g_mockFiles[_path] = _content;
            g_mockFileTimes[_path] = std::time(nullptr);
        }
        _open = false;
    }

    operator bool() const { return _open; }
    bool isDirectory() const { return _directory; }
    size_t size() const { return _content.length(); }
    const char* name() const { return _path.c_str(); }

    MockFile openNextFile() {
        if (!_directory || _dirIndex >= _dirEntries.size()) return MockFile();
        return MockFile(_dirEntries[_dirIndex++], "r");
    }

private:
    static bool isDirectoryPath(const String& path) {
        if (path == "/" || path == "/test" || path == "/config") return true;
        String prefix = path;
        if (!prefix.endsWith("/")) prefix += "/";
        for (const auto& item : g_mockFiles) {
            if (item.first.startsWith(prefix)) return true;
        }
        return false;
    }

    void buildDirectoryEntries(const String& path) {
        String prefix = path;
        if (!prefix.endsWith("/")) prefix += "/";
        for (const auto& item : g_mockFiles) {
            if (path == "/" || item.first.startsWith(prefix)) {
                _dirEntries.push_back(item.first);
            }
        }
    }

    String _path;
    String _content;
    bool _open = false;
    bool _write = false;
    bool _directory = false;
    size_t _pos = 0;
    size_t _dirIndex = 0;
    std::vector<String> _dirEntries;
};

class MockLittleFSClass {
public:
    bool begin(bool = false) { return true; }
    void end() {}
    bool format() {
        g_mockFiles.clear();
        g_mockFileTimes.clear();
        return true;
    }
    bool exists(const char* path) { return g_mockFiles.find(String(path)) != g_mockFiles.end(); }
    bool exists(const String& path) { return exists(path.c_str()); }
    MockFile open(const char* path, const char* mode = "r") { return MockFile(String(path), String(mode)); }
    MockFile open(const String& path, const char* mode = "r") { return open(path.c_str(), mode); }
    bool remove(const char* path) {
        g_mockFiles.erase(String(path));
        return true;
    }
    bool remove(const String& path) { return remove(path.c_str()); }
    bool rename(const String& oldPath, const String& newPath) {
        auto it = g_mockFiles.find(oldPath);
        if (it == g_mockFiles.end()) return false;
        g_mockFiles[newPath] = it->second;
        g_mockFiles.erase(it);
        return true;
    }
    bool mkdir(const String&) { return true; }
    bool rmdir(const String&) { return true; }
    size_t totalBytes() { return 1024 * 1024; }
    size_t usedBytes() { return g_mockFiles.size() * 128; }
    void clearAll() { format(); }
};

inline MockLittleFSClass MockLittleFS;

#define LittleFS MockLittleFS

#endif
