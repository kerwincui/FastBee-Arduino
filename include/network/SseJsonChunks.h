#pragma once
#include <cstddef>
#include <cstring>

namespace FastBee {
enum class SseChunkResult { Ok, InvalidArray, RecordTooLarge, SendFailed };

// Boundaries of objects in a server-generated JSON array. No heap allocation
// or cutting inside a string/UTF-8 sequence. Nested containers are supported.
inline bool nextSseObject(const char* data, size_t length, size_t& pos,
                          size_t& begin, size_t& end, bool& done) {
    auto space = [&]() {
        while (pos < length && (data[pos] == ' ' || data[pos] == '\n' ||
               data[pos] == '\r' || data[pos] == '\t')) ++pos;
    };
    space();
    if (pos >= length || data[pos] != '{') return false;
    begin = pos;
    char stack[32];
    size_t depth = 0;
    bool quoted = false, escaped = false;
    for (; pos < length; ++pos) {
        const char c = data[pos];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (depth == sizeof(stack)) return false;
            stack[depth++] = c;
        } else if (c == '}' || c == ']') {
            if (!depth || stack[depth - 1] != (c == '}' ? '{' : '[')) return false;
            if (--depth == 0) { ++pos; break; }
        }
    }
    if (depth || quoted) return false;
    end = pos;
    space();
    if (pos >= length) return false;
    if (data[pos] == ',') { ++pos; done = false; return true; }
    if (data[pos++] != ']') return false;
    space();
    done = true;
    return pos == length;
}

template<size_t Capacity, class Emit>
SseChunkResult sendSseObjectArray(const char* data, size_t length, Emit emit) {
    static_assert(Capacity >= 4, "SSE buffer must fit an object array");
    size_t start = 0;
    while (start < length && (data[start] == ' ' || data[start] == '\n' ||
           data[start] == '\r' || data[start] == '\t')) ++start;
    if (start == length || data[start++] != '[') return SseChunkResult::InvalidArray;
    size_t pos = start, begin = 0, end = 0;
    bool done = false;
    // Validate all record boundaries and sizes before publishing a partial set.
    do {
        if (!nextSseObject(data, length, pos, begin, end, done)) return SseChunkResult::InvalidArray;
        if (end - begin + 2 > Capacity) return SseChunkResult::RecordTooLarge;
    } while (!done);
    char buffer[Capacity + 1];
    size_t used = 1;
    buffer[0] = '[';
    pos = start;
    done = false;
    do {
        if (!nextSseObject(data, length, pos, begin, end, done)) return SseChunkResult::InvalidArray;
        const size_t count = end - begin;
        if (used + (used > 1 ? 1 : 0) + count + 1 > Capacity) {
            buffer[used++] = ']'; buffer[used] = '\0';
            if (!emit(buffer)) return SseChunkResult::SendFailed;
            used = 1;
        }
        if (used > 1) buffer[used++] = ',';
        std::memcpy(buffer + used, data + begin, count);
        used += count;
    } while (!done);
    buffer[used++] = ']'; buffer[used] = '\0';
    return emit(buffer) ? SseChunkResult::Ok : SseChunkResult::SendFailed;
}
}
