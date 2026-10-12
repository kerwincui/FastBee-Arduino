#pragma once
#include <Arduino.h>
class IPAddress {
public:
    bool fromString(const String& value) {
        unsigned a, b, c, d; char tail;
        if (std::sscanf(value.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
            a > 255 || b > 255 || c > 255 || d > 255) return false;
        text = value; return true;
    }
    String toString() const { return text; }
private:
    String text = "0.0.0.0";
};
