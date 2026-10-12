#pragma once

#include <ArduinoJson.h>
#include "utils/SharedJsonSerializer.h"
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#if defined(ESP32)
#include "utils/PsramJsonDocument.h"
#endif

namespace FastBee {

// Build a catalog one record at a time. Only serialized fragments survive the
// handler; neither a catalog-sized JSON tree nor a contiguous body is retained.
class JsonFragmentsResponse final : public AsyncAbstractResponse {
public:
    // A single catalog record may stream bounded pieces from an immutable
    // snapshot instead of retaining a record-sized DOM and String together.
    class Reader {
    public:
        static constexpr size_t TryAgain = 0xFFFFFFFFU;
        virtual ~Reader() = default;
        virtual size_t length() const = 0;
        virtual size_t read(uint8_t* destination, size_t capacity) = 0;
    };
    enum class Storage { Automatic, OwnedStrings, CatalogLiterals };

    explicit JsonFragmentsResponse(size_t count, Storage storage = Storage::Automatic)
        : capacity_(count), storage_(storage) {
#if defined(ESP32)
        // Metadata is small; serialized catalogs/envelopes must not occupy
        // scarce internal DRAM while waiting for TCP acknowledgement.
        if (storage_ != Storage::OwnedStrings && psramAvailableForJson()) {
            external_.reset(new (std::nothrow) ExternalFragment[count]);
        } else
#endif
        {
            fragments_.reset(new (std::nothrow) String[count]);
            if (storage_ == Storage::CatalogLiterals) {
                literals_.reset(new (std::nothrow) const char*[count]());
            }
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
        return fragments_ != nullptr &&
            (storage_ != Storage::CatalogLiterals || literals_ != nullptr);
    }

    bool canBorrowCatalogLiteral() const { return literals_ != nullptr; }

    bool setReader(size_t index, std::unique_ptr<Reader> reader) {
        if (!allocated() || reader_ || !reader || !reader->length() || index >= capacity_) return false;
#if defined(ESP32)
        if (external_ && external_[index].data) return false;
#endif
        if (fragments_ && (fragments_[index].length() || (literals_ && literals_[index]))) return false;
        if (reader->length() > std::numeric_limits<size_t>::max() - _contentLength) return false;
        _contentLength += reader->length();
        readerIndex_ = index;
        reader_ = std::move(reader);
        ++count_;
        return true;
    }

    // Only the catalog opts into static program-lifetime JSON. Ordinary and
    // OwnedStrings responses keep their existing allocation and ownership.
    bool setCatalogLiteral(size_t index, const char* literal) {
        if (!allocated() || (reader_ && readerIndex_ == index) || !literals_ || index >= capacity_ || !literal ||
            !*literal || literals_[index] || fragments_[index].length()) return false;
        const size_t expected = strlen(literal);
        if (expected > std::numeric_limits<size_t>::max() - _contentLength) return false;
        literals_[index] = literal;
        _contentLength += expected;
        ++count_;
        return true;
    }

    // Transfer already checked serialized bytes without AsyncBasicResponse's
    // second body allocation. Empty fragments are omitted by the caller, so
    // length remains an unambiguous marker for an occupied slot.
    bool set(size_t index, String&& fragment) {
        if ((reader_ && readerIndex_ == index) || storage_ != Storage::OwnedStrings || !fragments_ ||
            index >= capacity_ || fragment.isEmpty() || fragments_[index].length()) return false;
        const size_t expected = fragment.length();
        if (expected > std::numeric_limits<size_t>::max() - _contentLength) return false;
        String& part = fragments_[index];
        part = std::move(fragment);
        if (part.length() != expected) return false;
        _contentLength += expected;
        ++count_;
        return true;
    }

    bool set(size_t index, const JsonDocument& document, const char* prefix = "",
                const char* suffix = "") {
        if (!allocated() || (reader_ && readerIndex_ == index) || index >= capacity_ || document.overflowed()) return false;
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
        if (part.length() || (literals_ && literals_[index])) return false;
        const size_t expected = strlen(prefix) + measureJson(document) + strlen(suffix);
        if (!part.reserve(expected)) return false;
        part += prefix;
        AppendWriter writer{part};
        serializeJsonShared(document, writer);
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
            if (reader_ && fragment_ == readerIndex_) {
                const size_t copied = reader_->read(destination + written, capacity - written);
                if (copied == Reader::TryAgain || !copied) return written ? written : Reader::TryAgain;
                written += copied;
                readerOffset_ += copied;
                if (readerOffset_ == reader_->length()) {
                    reader_.reset();
                    ++fragment_;
                    offset_ = 0;
                }
                continue;
            }
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
            if (literals_ && literals_[fragment_]) {
                const char* part = literals_[fragment_];
                const size_t length = strlen(part);
                const size_t copy = std::min(capacity - written, length - offset_);
                memcpy(destination + written, part + offset_, copy);
                written += copy;
                offset_ += copy;
                if (offset_ == length) {
                    literals_[fragment_] = nullptr;
                    ++fragment_;
                    offset_ = 0;
                }
                continue;
            }
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
    std::unique_ptr<const char*[]> literals_;
    std::unique_ptr<Reader> reader_;
    size_t readerIndex_ = 0;
    size_t readerOffset_ = 0;
    size_t capacity_;
    Storage storage_;
    size_t count_ = 0;
    size_t fragment_ = 0;
    size_t offset_ = 0;
};

} // namespace FastBee
