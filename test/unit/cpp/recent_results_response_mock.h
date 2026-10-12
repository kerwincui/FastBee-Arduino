#pragma once
#include <Arduino.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

class AsyncAbstractResponse {
public:
    virtual ~AsyncAbstractResponse() = default;
    virtual bool _sourceValid() const = 0;
    virtual size_t _fillBuffer(uint8_t*, size_t) = 0;
    int _code = 0;
    String _contentType;
    size_t _contentLength = 0, _sentLength = 0;
    bool _sendContentLength = false;
    void addHeader(const char*, const char*, bool = true) {}
    static void* operator new(size_t size, const std::nothrow_t&) noexcept {
        if (recentResponseFailure) return nullptr;
        void* memory = ::operator new(size, std::nothrow);
        if (memory) ++recentLiveResponses;
        return memory;
    }
    static void operator delete(void* memory) noexcept {
        if (memory) { assert(recentLiveResponses); --recentLiveResponses; }
        ::operator delete(memory);
    }
    static void operator delete(void* memory, const std::nothrow_t&) noexcept { operator delete(memory); }
};
using AsyncWebServerResponse = AsyncAbstractResponse;

// The constructor body is extracted verbatim from the installed dependency.
// Only socket transmission is replaced by deferred, bounded byte reads.
inline const char* T_text_plain = "text/plain";
inline const char* T_Connection = "Connection";
inline const char* T_close = "close";
class AsyncBasicResponse : public AsyncAbstractResponse {
public:
    String _content;
    AsyncBasicResponse(int, const char*, const char*);
    bool _sourceValid() const override { return true; }
    size_t _fillBuffer(uint8_t* output, size_t capacity) override {
        const size_t count = std::min(capacity, _content.length() - _sentLength);
        std::memcpy(output, _content.c_str() + _sentLength, count);
        return count;
    }
};
#include "recent_basic_constructor.inc"
