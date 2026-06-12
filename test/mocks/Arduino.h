#ifndef FASTBEE_TEST_MOCK_ARDUINO_H
#define FASTBEE_TEST_MOCK_ARDUINO_H

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using boolean = bool;
using byte = uint8_t;

constexpr int DEC = 10;
constexpr int HEX = 16;
constexpr int OCT = 8;
constexpr int BIN = 2;
constexpr int HIGH = 1;
constexpr int LOW = 0;

template <typename T>
T constrain(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

class String {
public:
    String() = default;
    String(const char* value) : _value(value ? value : "") {}
    String(const char* value, unsigned int length) : _value(value ? std::string(value, value + length) : "") {}
    String(char* value) : _value(value ? value : "") {}
    String(char value) : _value(1, value) {}
    String(const std::string& value) : _value(value) {}
    String(bool value) : _value(value ? "true" : "false") {}
    String(int value, int base = DEC) : _value(formatInteger(value, base)) {}
    String(unsigned int value, int base = DEC) : _value(formatInteger(value, base)) {}
    String(long value, int base = DEC) : _value(formatInteger(value, base)) {}
    String(unsigned long value, int base = DEC) : _value(formatInteger(value, base)) {}
    String(long long value, int base = DEC) : _value(formatInteger(value, base)) {}
    String(unsigned long long value, int base = DEC) : _value(formatInteger(value, base)) {}
    String(float value, int decimals = 2) : _value(formatFloat(value, decimals)) {}
    String(double value, int decimals = 2) : _value(formatFloat(value, decimals)) {}

    const char* c_str() const { return _value.c_str(); }
    size_t length() const { return _value.length(); }
    bool isEmpty() const { return _value.empty(); }
    void reserve(size_t size) { _value.reserve(size); }
    void clear() { _value.clear(); }
    String substr(size_t pos) const { return _value.substr(pos); }
    String substr(size_t pos, size_t len) const { return _value.substr(pos, len); }

    int indexOf(const char* needle, int fromIndex = 0) const {
        if (!needle || fromIndex < 0 || static_cast<size_t>(fromIndex) > _value.size()) return -1;
        size_t pos = _value.find(needle, static_cast<size_t>(fromIndex));
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    int indexOf(const String& needle, int fromIndex = 0) const {
        return indexOf(needle.c_str(), fromIndex);
    }

    int indexOf(char needle, int fromIndex = 0) const {
        if (fromIndex < 0 || static_cast<size_t>(fromIndex) > _value.size()) return -1;
        size_t pos = _value.find(needle, static_cast<size_t>(fromIndex));
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    String substring(int start) const {
        return substring(start, static_cast<int>(_value.length()));
    }

    String substring(int start, int end) const {
        int len = static_cast<int>(_value.length());
        start = std::max(0, std::min(start, len));
        end = std::max(start, std::min(end, len));
        return _value.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    }

    bool startsWith(const char* prefix) const {
        if (!prefix) return false;
        std::string p(prefix);
        return _value.rfind(p, 0) == 0;
    }

    bool startsWith(const String& prefix) const { return startsWith(prefix.c_str()); }

    bool endsWith(const char* suffix) const {
        if (!suffix) return false;
        std::string s(suffix);
        return s.size() <= _value.size() &&
            _value.compare(_value.size() - s.size(), s.size(), s) == 0;
    }

    bool endsWith(const String& suffix) const { return endsWith(suffix.c_str()); }
    bool equals(const char* other) const { return _value == (other ? other : ""); }
    bool equals(const String& other) const { return _value == other._value; }

    void replace(const char* from, const char* to) {
        std::string f(from ? from : "");
        std::string t(to ? to : "");
        if (f.empty()) return;
        size_t pos = 0;
        while ((pos = _value.find(f, pos)) != std::string::npos) {
            _value.replace(pos, f.length(), t);
            pos += t.length();
        }
    }

    void replace(const String& from, const String& to) { replace(from.c_str(), to.c_str()); }

    void trim() {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch); };
        _value.erase(_value.begin(), std::find_if(_value.begin(), _value.end(), [&](char ch) { return !isSpace(ch); }));
        _value.erase(std::find_if(_value.rbegin(), _value.rend(), [&](char ch) { return !isSpace(ch); }).base(), _value.end());
    }

    void toUpperCase() {
        std::transform(_value.begin(), _value.end(), _value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    }

    void toLowerCase() {
        std::transform(_value.begin(), _value.end(), _value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    }

    int toInt() const {
        try { return std::stoi(_value); } catch (...) { return 0; }
    }

    float toFloat() const {
        try { return std::stof(_value); } catch (...) { return 0.0f; }
    }

    size_t write(uint8_t value) {
        _value.push_back(static_cast<char>(value));
        return 1;
    }

    size_t write(const uint8_t* data, size_t length) {
        if (!data) return 0;
        _value.append(reinterpret_cast<const char*>(data), length);
        return length;
    }

    int read() {
        if (_readPos >= _value.size()) return -1;
        return static_cast<uint8_t>(_value[_readPos++]);
    }

    int read() const {
        if (_readPos >= _value.size()) return -1;
        return static_cast<uint8_t>(_value[_readPos++]);
    }

    char operator[](size_t index) const { return _value[index]; }
    char& operator[](size_t index) { return _value[index]; }
    void setCharAt(size_t index, char c) {
        if (index < _value.length()) _value[index] = c;
    }
    int compareTo(const String& other) const {
        return _value.compare(other._value);
    }
    operator std::string() const { return _value; }

    String& operator=(const char* value) {
        _value = value ? value : "";
        return *this;
    }

    String& operator+=(const String& other) {
        _value += other._value;
        return *this;
    }

    String& operator+=(const char* other) {
        _value += other ? other : "";
        return *this;
    }

    String& operator+=(char other) {
        _value += other;
        return *this;
    }

    friend String operator+(const String& lhs, const String& rhs) { return String(lhs._value + rhs._value); }
    friend String operator+(const String& lhs, const char* rhs) { return String(lhs._value + (rhs ? rhs : "")); }
    friend String operator+(const char* lhs, const String& rhs) { return String(std::string(lhs ? lhs : "") + rhs._value); }
    friend bool operator==(const String& lhs, const String& rhs) { return lhs._value == rhs._value; }
    friend bool operator==(const String& lhs, const char* rhs) { return lhs._value == (rhs ? rhs : ""); }
    friend bool operator!=(const String& lhs, const String& rhs) { return !(lhs == rhs); }
    friend bool operator<(const String& lhs, const String& rhs) { return lhs._value < rhs._value; }
    friend std::ostream& operator<<(std::ostream& os, const String& value) {
        os << value._value;
        return os;
    }

private:
    template <typename T>
    static std::string formatInteger(T value, int base) {
        std::ostringstream out;
        if (base == HEX) out << std::uppercase << std::hex << value;
        else if (base == OCT) out << std::oct << value;
        else out << value;
        return out.str();
    }

    static std::string formatFloat(double value, int decimals) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(std::max(0, decimals)) << value;
        return out.str();
    }

    std::string _value;
    mutable size_t _readPos = 0;
};

class MockSerialClass {
public:
    void begin(unsigned long) {}
    operator bool() const { return true; }

    void print(const char* value) { std::cout << (value ? value : ""); }
    void print(const String& value) { std::cout << value.c_str(); }
    void print(int value) { std::cout << value; }
    void println() { std::cout << std::endl; }
    void println(const char* value) { std::cout << (value ? value : "") << std::endl; }
    void println(const String& value) { std::cout << value.c_str() << std::endl; }
    void println(int value) { std::cout << value << std::endl; }

    int printf(const char* format, ...) {
        va_list args;
        va_start(args, format);
        int result = std::vprintf(format, args);
        va_end(args);
        return result;
    }
};

inline MockSerialClass Serial;

inline auto __fastbeeMockStartTime = std::chrono::steady_clock::now();
inline size_t __fastbeeMockHeapUsed = 0;
inline std::map<void*, size_t> __fastbeeMockAllocations;

inline unsigned long millis() {
    auto elapsed = std::chrono::steady_clock::now() - __fastbeeMockStartTime;
    return static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

inline unsigned long micros() {
    auto elapsed = std::chrono::steady_clock::now() - __fastbeeMockStartTime;
    return static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

inline void delay(unsigned long ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline long random(long max) {
    if (max <= 0) return 0;
    return std::rand() % max;
}

inline long random(long min, long max) {
    if (max <= min) return min;
    return min + (std::rand() % (max - min));
}

inline void* fastbeeMockMalloc(size_t size) {
    void* ptr = std::malloc(size);
    if (ptr) {
        __fastbeeMockAllocations[ptr] = size;
        __fastbeeMockHeapUsed += size;
    }
    return ptr;
}

inline void fastbeeMockFree(void* ptr) {
    if (!ptr) return;
    auto it = __fastbeeMockAllocations.find(ptr);
    if (it != __fastbeeMockAllocations.end()) {
        __fastbeeMockHeapUsed -= it->second;
        __fastbeeMockAllocations.erase(it);
    }
    std::free(ptr);
}

class MockESPClass {
public:
    uint32_t getFreeHeap() const {
        if (_heapOverride > 0) return _heapOverride;
        constexpr uint32_t totalHeap = 150000;
        return totalHeap > __fastbeeMockHeapUsed ? totalHeap - static_cast<uint32_t>(__fastbeeMockHeapUsed) : 0;
    }

    uint32_t getHeapSize() const { return 327680; }
    uint32_t getMinFreeHeap() const { return getFreeHeap(); }
    uint32_t getMaxAllocHeap() const { return getFreeHeap() / 2; }

    // PSRAM support
    uint32_t getPsramSize() const { return _psramSize; }
    uint32_t getFreePsram() const { return _freePsram; }

    // Chip info
    const char* getChipModel() const { return "ESP32-S3"; }
    uint8_t getChipRevision() const { return 1; }
    uint32_t getFlashChipSize() const { return 16 * 1024 * 1024; }

    // Test control methods
    void setFreeHeap(uint32_t heap) { _heapOverride = heap; }
    void resetHeapOverride() { _heapOverride = 0; }
    void setPsramSize(uint32_t size) { _psramSize = size; }
    void setFreePsram(uint32_t free) { _freePsram = free; }

private:
    mutable uint32_t _heapOverride = 0;
    uint32_t _psramSize = 8 * 1024 * 1024;  // 8MB default
    uint32_t _freePsram = 7 * 1024 * 1024;  // 7MB free default
};

inline MockESPClass ESP;

#define malloc(size) fastbeeMockMalloc(size)
#define free(ptr) fastbeeMockFree(ptr)

#endif
