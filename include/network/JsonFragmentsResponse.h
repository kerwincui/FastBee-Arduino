#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#if defined(ESP32)
#include "utils/PsramJsonDocument.h"
#endif

namespace FastBee {

// Build a catalog one record at a time. Only serialized fragments survive the
// handler; neither a catalog-sized JSON tree nor a contiguous body is retained.
class JsonFragmentsResponse final : public AsyncAbstractResponse {
public:
    explicit JsonFragmentsResponse(size_t count)
        : capacity_(count) {
#if defined(ESP32)
        // Metadata is small; serialized catalogs/envelopes must not occupy
        // scarce internal DRAM while waiting for TCP acknowledgement.
        if (psramAvailableForJson()) {
            external_.reset(new (std::nothrow) ExternalFragment[count]);
        } else
#endif
        {
            fragments_.reset(new (std::nothrow) String[count]);
        }
        _code = 200;
        _contentType = "application/json";
        _contentLength = 0;
        _sendContentLength = true;
    }

    bool allocated() const {
#if defined(ESP32)
        if (external_) return true;
#endif
        return fragments_ != nullptr;
    }

    bool set(size_t index, const JsonDocument& document, const char* prefix = "",
                const char* suffix = "") {
        if (!allocated() || index >= capacity_ || document.overflowed()) return false;
#if defined(ESP32)
        if (external_) {
            ExternalFragment& part = external_[index];
            if (part.data) return false;
            const size_t prefixLength = strlen(prefix);
            const size_t jsonLength = measureJson(document);
            const size_t suffixLength = strlen(suffix);
            const size_t expected = prefixLength + jsonLength + suffixLength;
            part.data = static_cast<char*>(heap_caps_malloc(
                expected + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!part.data) return false;
            memcpy(part.data, prefix, prefixLength);
            if (serializeJson(document, part.data + prefixLength, jsonLength + 1) != jsonLength) return false;
            memcpy(part.data + prefixLength + jsonLength, suffix, suffixLength);
            part.data[expected] = '\0';
            part.length = expected;
            _contentLength += expected;
            ++count_;
            return true;
        }
#endif
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
        return allocated() && count_ == capacity_;
    }

    size_t _fillBuffer(uint8_t* destination, size_t capacity) override {
        size_t written = 0;
        while (fragment_ < count_ && written < capacity) {
#if defined(ESP32)
            if (external_) {
                ExternalFragment& part = external_[fragment_];
                const size_t copy = std::min(capacity - written, part.length - offset_);
                memcpy(destination + written, part.data + offset_, copy);
                written += copy;
                offset_ += copy;
                if (offset_ == part.length) {
                    part.clear();
                    ++fragment_;
                    offset_ = 0;
                }
                continue;
            }
#endif
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
#if defined(ESP32)
    struct ExternalFragment {
        char* data = nullptr;
        size_t length = 0;
        ~ExternalFragment() { clear(); }
        void clear() {
            if (data) heap_caps_free(data);
            data = nullptr;
            length = 0;
        }
    };
    std::unique_ptr<ExternalFragment[]> external_;
#endif
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
