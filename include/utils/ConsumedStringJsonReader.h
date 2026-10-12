#pragma once
#include <Arduino.h>
#include <cstddef>

namespace FastBee {
// Keep the parser's actual consumption position: successful first-value
// parsing must not hide a second value or trailing garbage. Both callers use
// this same concrete type so ArduinoJson can share its reader specialization.
struct ConsumedStringJsonReader {
    const String& text;
    size_t offset = 0;
    int read() { return offset < text.length() ? static_cast<unsigned char>(text[offset++]) : -1; }
    size_t readBytes(char* output, size_t count) {
        size_t copied = 0;
        while (copied < count && offset < text.length()) output[copied++] = text[offset++];
        return copied;
    }
    bool complete() const {
        for (size_t i = offset; i < text.length(); ++i)
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') return false;
        return true;
    }
    bool onlyWhitespaceRemaining() const { return complete(); }
};
}  // namespace FastBee
