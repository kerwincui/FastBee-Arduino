/**
 * @file MD5Builder.h
 * @brief Mock MD5Builder for native unit tests
 */

#ifndef MOCK_MD5_BUILDER_H
#define MOCK_MD5_BUILDER_H

#include <Arduino.h>

class MD5Builder {
public:
    void begin() {}
    void add(const String& str) { m_data = str; }
    void calculate() {}
    String toString() {
        // Return a fake 32-char hex MD5 hash for testing
        // Format: 8 + 8 + 8 + 8 = 32 hex chars
        char buf[33];
        snprintf(buf, sizeof(buf), "aabbccdd%08x00112233aabbccdd", (unsigned)m_data.length());
        return String(buf);
    }
private:
    String m_data;
};

#endif // MOCK_MD5_BUILDER_H
