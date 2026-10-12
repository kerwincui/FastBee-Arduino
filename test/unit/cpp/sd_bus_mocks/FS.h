#pragma once
#include <Arduino.h>
#include <atomic>
#include <thread>
#include <vector>
inline std::atomic<bool> blockSdRead{false}, sdReadEntered{false}, allowSdRead{false};
inline size_t sdWriteLimit = static_cast<size_t>(-1);
inline std::string sdFileContent = "abc";
inline size_t sdFileDeclaredSize = 3, sdFileFinalSize = static_cast<size_t>(-1);
inline int sdReadFailAt = -1, sdReadFault = -1;
inline bool sdAvailableInfinite = false;
inline unsigned sdReadCalls = 0, sdWriteCalls = 0;
inline std::vector<std::string> sdDirectoryPaths;
inline unsigned sdDirectoryReads = 0, sdDirectoryChildCloses = 0;
constexpr const char* FILE_READ = "r";
constexpr const char* FILE_WRITE = "w";
constexpr const char* FILE_APPEND = "a";
class File {
public:
    explicit File(bool valid = false, bool directory = false) : _valid(valid), _directory(directory) {}
    explicit operator bool() const { return _valid; }
    bool isDirectory() const { return _directory; }
    size_t size() const { return _position && sdFileFinalSize != static_cast<size_t>(-1) ? sdFileFinalSize : sdFileDeclaredSize; }
    const char* path() const { return _path.c_str(); }
    void close() { if (_valid && _directoryChild) ++sdDirectoryChildCloses; _valid = false; }
    void flush() {}
    File openNextFile() {
        ++sdDirectoryReads;
        if (!_valid || !_directory || _next >= sdDirectoryPaths.size()) return File();
        File child(true); child._path=sdDirectoryPaths[_next++]; child._directoryChild=true;
        return child;
    }
    int available() const { return _valid && (sdAvailableInfinite || _position < sdFileContent.size()); }
    int read() {
        ++sdReadCalls;
        if (blockSdRead.load()) {
            sdReadEntered = true;
            while (!allowSdRead.load()) std::this_thread::yield();
        }
        if (sdReadFailAt >= 0 && _position == static_cast<size_t>(sdReadFailAt)) return sdReadFault;
        return _position < sdFileContent.size() ? static_cast<unsigned char>(sdFileContent[_position++]) : -1;
    }
    size_t write(const uint8_t*, size_t count) { ++sdWriteCalls; return std::min(count, sdWriteLimit); }
private:
    bool _valid, _directory;
    size_t _position = 0;
    size_t _next = 0;
    std::string _path = "/fastbee/test.txt";
    bool _directoryChild = false;
};
