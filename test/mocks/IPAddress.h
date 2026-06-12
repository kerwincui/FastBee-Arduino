#ifndef FASTBEE_TEST_MOCK_IPADDRESS_H
#define FASTBEE_TEST_MOCK_IPADDRESS_H

#include <Arduino.h>
#include <array>

class IPAddress {
public:
    IPAddress() : _bytes{0, 0, 0, 0} {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : _bytes{a, b, c, d} {}

    bool fromString(const String& value) {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(value.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        _bytes = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d)};
        return true;
    }

    uint8_t operator[](int index) const {
        if (index >= 0 && index < 4) return _bytes[index];
        return 0;
    }

    String toString() const {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", _bytes[0], _bytes[1], _bytes[2], _bytes[3]);
        return String(buffer);
    }

    bool operator==(const IPAddress& other) const {
        return _bytes == other._bytes;
    }

    bool operator!=(const IPAddress& other) const {
        return _bytes != other._bytes;
    }

private:
    std::array<uint8_t, 4> _bytes;
};

#endif
