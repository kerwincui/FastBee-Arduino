#pragma once
#include <Arduino.h>

class File {
    std::string content;
    size_t position = 0;
    bool opened;
public:
    File(std::string text, bool available) : content(std::move(text)), opened(available) {}
    explicit operator bool() const { return opened; }
    int read() { return position < content.size() ? static_cast<unsigned char>(content[position++]) : -1; }
    size_t readBytes(char* buffer, size_t length) {
        const size_t count = std::min(length, content.size() - position);
        std::memcpy(buffer, content.data() + position, count);
        position += count;
        return count;
    }
    void close() { opened = false; }
};
struct MockOtaFileSystem {
    std::string content;
    bool present = true, readable = true;
    bool exists(const char*) { return present; }
    File open(const char*, const char*) { return File(content, readable); }
};
inline MockOtaFileSystem LittleFS;
