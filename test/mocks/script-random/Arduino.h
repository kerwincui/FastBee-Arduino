#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Only the String operations used by the actual expansion wrapper. Inject
// allocation failures without changing the shared mock used by other suites.
class String {
    std::string data;
public:
    static inline bool rejectReserve = false;
    static inline int appendBudget = -1;
    String() = default;
    String(const char* value): data(value) {}
    String(const std::string& value): data(value) {}
    size_t length() const { return data.size(); }
    const char* c_str() const { return data.c_str(); }
    void clear() { data.clear(); }
    bool reserve(size_t size) {
        if (rejectReserve) return false;
        data.reserve(size); return true;
    }
    bool concat(char c) {
        if (appendBudget == 0) return false;
        if (appendBudget > 0) --appendBudget;
        data += c; return true;
    }
};
