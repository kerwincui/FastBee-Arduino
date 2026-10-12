#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cctype>
#include <utility>

// Isolated parser String with deterministic allocation-failure injection.
class String {
    std::string data;
    bool assign(const std::string& value) {
        data.clear();
        if (!value.empty() && !allocation()) return false;
        data = value; return true;
    }
public:
    static inline int failAt = -1, allocations = 0;
    static bool allocation() { return allocations++ != failAt; }
    String() = default;
    String(const char* value) { assign(value); }
    String(const std::string& value) { assign(value); }
    template<class T> explicit String(T value) { assign(std::to_string(value)); }
    String(const String& value) { assign(value.data); }
    String(String&&) noexcept = default;
    String& operator=(const String& value) { if(this != &value) assign(value.data); return *this; }
    String& operator=(String&&) noexcept = default;
    const char* c_str() const { return data.c_str(); }
    size_t length() const { return data.size(); }
    bool isEmpty() const { return data.empty(); }
    void clear() { data.clear(); }
    char charAt(size_t pos) const { return pos < data.size() ? data[pos] : 0; }
    int indexOf(char ch,int start) const {
        const auto pos=data.find(ch,static_cast<size_t>(start));
        return pos==std::string::npos ? -1 : static_cast<int>(pos);
    }
    String substring(int start,int end) const { return String(data.substr(start,end-start)); }
    void trim() {
        while(!data.empty() && std::isspace(static_cast<unsigned char>(data.back()))) data.pop_back();
        size_t i=0;
        while(i<data.size() && std::isspace(static_cast<unsigned char>(data[i]))) ++i;
        data.erase(0,i);
    }
    void toUpperCase() { for(char& c:data) c=static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    bool concat(char c) { if(!allocation()) return false; data+=c; return true; }
    bool concat(const String& value) { if(!allocation()) return false; data+=value.data; return true; }
    bool operator==(const char* value) const { return data==value; }
    bool operator!=(const char* value) const { return data!=value; }
    friend String operator+(const String& a,const String& b) { return String(a.data+b.data); }
};
