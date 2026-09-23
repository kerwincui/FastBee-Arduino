#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace FastBee {

// Own the existing document until the response is destroyed. Large JSON no
// longer needs a contiguous serialized String plus AsyncBasicResponse's copy.
// Each TCP fill traverses the document but writes only its requested byte range
// into the small buffer, matching the upstream AsyncJsonResponse strategy.
class JsonDocumentResponse final : public AsyncAbstractResponse {
public:
    explicit JsonDocumentResponse(JsonDocument&& document)
        : document_(std::move(document)) {
        _code = 200;
        _contentType = "application/json";
        _contentLength = measureJson(document_);
        _sendContentLength = true;
    }

    bool _sourceValid() const override { return !document_.overflowed(); }

    size_t _fillBuffer(uint8_t* destination, size_t capacity) override {
        RangeWriter writer(destination, _sentLength, capacity);
        serializeJson(document_, writer);
        const size_t written = writer.written();
        // Once the last bytes are staged, retries use the response's staging
        // buffer. Release the much larger JSON pool before TCP ACK/FIN and
        // before the browser starts its next request.
        if (_sentLength + written >= _contentLength) document_.clear();
        return written;
    }

private:
    class RangeWriter {
    public:
        RangeWriter(uint8_t* destination, size_t skip, size_t capacity)
            : destination_(destination), skip_(skip), capacity_(capacity) {}
        size_t write(uint8_t byte) { return write(&byte, 1); }
        size_t write(const uint8_t* bytes, size_t length) {
            const size_t skipped = std::min(skip_, length);
            skip_ -= skipped;
            const size_t copied = std::min(length - skipped, capacity_ - written_);
            if (copied) {
                std::memcpy(destination_ + written_, bytes + skipped, copied);
                written_ += copied;
            }
            // Serializer must keep traversing after a skipped/full range.
            return length;
        }
        size_t written() const { return written_; }
    private:
        uint8_t* destination_;
        size_t skip_;
        size_t capacity_;
        size_t written_ = 0;
    };
    JsonDocument document_;
};

} // namespace FastBee
