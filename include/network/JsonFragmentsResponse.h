#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace FastBee {

// Build a catalog one record at a time. Only serialized fragments survive the
// handler; neither a catalog-sized JSON tree nor a contiguous body is retained.
class JsonFragmentsResponse final : public AsyncAbstractResponse {
public:
    explicit JsonFragmentsResponse(size_t count)
        : fragments_(new (std::nothrow) String[count]), capacity_(count) {
        _code = 200;
        _contentType = "application/json";
        _contentLength = 0;
        _sendContentLength = true;
    }

    bool allocated() const { return fragments_ != nullptr; }

    bool set(size_t index, const JsonDocument& document, const char* prefix = "",
                const char* suffix = "") {
        if (!fragments_ || index >= capacity_ || document.overflowed()) return false;
        String& part = fragments_[index];
        if (part.length()) return false;
        const size_t expected = strlen(prefix) + measureJson(document) + strlen(suffix);
        if (!part.reserve(expected)) return false;
        part += prefix;
        AppendWriter writer{part};
        serializeJson(document, writer);
        part += suffix;
        // ArduinoJson's String writer can report the intended byte count even
        // when String::concat fails. The resulting length is authoritative.
        if (part.length() != expected) return false;
        _contentLength += expected;
        ++count_;
        return true;
    }

    bool _sourceValid() const override {
        return fragments_ && count_ == capacity_;
    }

    size_t _fillBuffer(uint8_t* destination, size_t capacity) override {
        size_t written = 0;
        while (fragment_ < count_ && written < capacity) {
            String& part = fragments_[fragment_];
            const size_t copy = std::min(capacity - written, part.length() - offset_);
            memcpy(destination + written, part.c_str() + offset_, copy);
            written += copy;
            offset_ += copy;
            if (offset_ == part.length()) {
                // The response staging buffer owns these bytes until ACK.
                part = String();
                ++fragment_;
                offset_ = 0;
            }
        }
        return written;
    }

private:
    struct AppendWriter {
        String& value;
        size_t write(uint8_t byte) { return write(&byte, 1); }
        size_t write(const uint8_t* bytes, size_t count) {
            return value.concat(reinterpret_cast<const char*>(bytes), count) ? count : 0;
        }
    };
    std::unique_ptr<String[]> fragments_;
    size_t capacity_;
    size_t count_ = 0;
    size_t fragment_ = 0;
    size_t offset_ = 0;
};

} // namespace FastBee
