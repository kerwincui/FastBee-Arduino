#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

// Test-only String storage faults. Both paths receive the same operation index,
// partial append and success/failure result. No production String replacement.
struct JsonTextFault {
    int at = -1;
    bool persistent = false, partial = false, lie = false;
    size_t operations = 0;
    bool fail() {
        const size_t call = operations++;
        return at >= 0 && (call == static_cast<size_t>(at) ||
            (persistent && call > static_cast<size_t>(at)));
    }
};
inline JsonTextFault jsonTextFault;

class String {
    std::string text_;
public:
    String() = default;
    String(const char* value) : text_(value ? value : "") {}
    String(std::string value) : text_(std::move(value)) {}
    String(const String&) = default;
    String(String&&) noexcept = default;
    String& operator=(const String&) = default;
    String& operator=(String&&) noexcept = default;
    String& operator=(const char* value) { text_ = value ? value : ""; return *this; }
    bool reserve(size_t size) {
        if (jsonTextFault.fail()) return false;
        text_.reserve(size); return true;
    }
    bool concat(const char* bytes, size_t size) {
        if (jsonTextFault.fail()) {
            if (jsonTextFault.partial && size > 1) text_.append(bytes, size / 2);
            return jsonTextFault.lie;
        }
        if (size) text_.append(bytes, size);
        return true;
    }
    String& operator+=(char value) { concat(&value, 1); return *this; }
    String& operator+=(const char* value) { concat(value, std::strlen(value)); return *this; }
    String& operator+=(const String& value) { concat(value.c_str(), value.length()); return *this; }
    size_t length() const { return text_.size(); }
    const char* c_str() const { return text_.c_str(); }
    bool isEmpty() const { return text_.empty(); }
};
