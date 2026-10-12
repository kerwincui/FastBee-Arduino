// Real ArduinoJson and extracted production entry bodies.
// Host semantics only; no device access.
#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "utils/ConsumedStringJsonReader.h"
#include "utils/SharedJsonDeserializer.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::abort(); } } while (false)
#define LOG_WARNINGF(...) do {} while (false)
static_assert(ARDUINOJSON_VERSION_MAJOR == 7 && ARDUINOJSON_VERSION_MINOR == 4 &&
              ARDUINOJSON_VERSION_REVISION == 2, "Review reader equivalence after library upgrade");
static_assert(std::is_base_of<ArduinoJson::detail::IteratorReader<const char*>,
              ArduinoJson::detail::BoundedReader<const uint8_t*>>::value, "Byte reader changed");
static_assert(std::is_base_of<ArduinoJson::detail::IteratorReader<const char*>,
              ArduinoJson::detail::BoundedReader<const char*>>::value, "Char reader changed");

// Verbatim reader algorithms from the pre-dedup production sources. Their
// containing namespace alone is changed for simultaneous differential tests.
namespace Original {
struct JsonReader {
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
};
struct RfJsonReader {
    const String& raw; size_t offset = 0;
    int read() { return offset < raw.length() ? static_cast<unsigned char>(raw[offset++]) : -1; }
    size_t readBytes(char* bytes, size_t length) {
        size_t copied = 0;
        while (copied < length && offset < raw.length()) bytes[copied++] = raw[offset++];
        return copied;
    }
    bool onlyWhitespaceRemaining() const {
        for (size_t i = offset; i < raw.length(); ++i)
            if (raw[i] != ' ' && raw[i] != '\t' && raw[i] != '\r' && raw[i] != '\n') return false;
        return true;
    }
};
}
struct RouteResult {
    bool accepted;
    std::string payload;
    bool operator==(const RouteResult& other) const {
        return accepted == other.accepted && payload == other.payload;
    }
};
#include "production_entries.inc"
static_assert(std::is_same<Current::JsonReader, Current::RfJsonReader>::value,
              "Both actual aliases must instantiate the identical parser type");

struct TraceAllocator final : ArduinoJson::Allocator {
    std::map<void*, size_t> sizes;
    std::vector<std::string> trace;
    unsigned attempts = 0, failAt = 0;
    bool persistent = false, growthFailure = false;
    bool fail(bool growth) {
        ++attempts;
        const bool failure = failAt && (persistent ? attempts >= failAt : attempts == failAt);
        if (failure) {
            trace.push_back(growth ? "failure:grow" : "failure:allocate");
            growthFailure = growthFailure || growth;
        }
        return failure;
    }
    void* allocate(size_t size) override {
        trace.push_back("allocate:" + std::to_string(size));
        if (fail(false)) return nullptr;
        void* pointer = std::malloc(size);
        CHECK(pointer);
        sizes[pointer] = size;
        return pointer;
    }
    void* reallocate(void* pointer, size_t size) override {
        const size_t old = pointer ? sizes.at(pointer) : 0;
        const bool growth = size > old;
        trace.push_back(std::string(growth ? "grow:" : "shrink:") +
                        std::to_string(old) + ":" + std::to_string(size));
        // AJ 7.4.2 StringBuilder requires shrinking reallocations to succeed.
        // Inject only allocations/growth. Retaining a shrink block is valid.
        if (pointer && !growth) { sizes[pointer] = size; return pointer; }
        if (fail(growth)) return nullptr;
        void* replacement = std::realloc(pointer, size);
        CHECK(replacement);
        sizes.erase(pointer);
        sizes[replacement] = size;
        return replacement;
    }
    void deallocate(void* pointer) override {
        trace.push_back(pointer ? "free:" + std::to_string(sizes.at(pointer)) : "free:null");
        sizes.erase(pointer);
        std::free(pointer);
    }
};

struct ParseResult {
    std::string error, json;
    std::vector<std::string> shape, allocations;
    bool overflowed = false, remainingWhitespace = true, growthFailure = false;
    size_t offset = 0;
    unsigned attempts = 0;
    bool operator==(const ParseResult& other) const {
        return error == other.error && json == other.json && shape == other.shape &&
               allocations == other.allocations && overflowed == other.overflowed &&
               remainingWhitespace == other.remainingWhitespace && offset == other.offset &&
               attempts == other.attempts && growthFailure == other.growthFailure;
    }
};
static void shapeOf(JsonVariantConst value, std::vector<std::string>& shape) {
    if (value.is<JsonObjectConst>()) {
        shape.push_back("object");
        for (JsonPairConst pair : value.as<JsonObjectConst>()) {
            const auto key = pair.key();
            shape.push_back("key:" + std::to_string(key.size()) + ":" + std::to_string(key.isStatic()));
            shapeOf(pair.value(), shape);
        }
    } else if (value.is<JsonArrayConst>()) {
        shape.push_back("array");
        for (JsonVariantConst item : value.as<JsonArrayConst>()) shapeOf(item, shape);
    } else if (value.is<const char*>()) {
        const auto text = value.as<JsonString>();
        shape.push_back("text:" + std::to_string(text.size()) + ":" + std::to_string(text.isStatic()));
    } else {
        shape.push_back(std::to_string(value.isNull()) + ":" + std::to_string(value.is<bool>()) +
                        ":" + std::to_string(value.is<int64_t>()) + ":" +
                        std::to_string(value.is<uint64_t>()) + ":" + std::to_string(value.is<double>()));
    }
}
static ParseResult parse(const std::string& bytes, unsigned mode, unsigned failAt = 0,
                         bool persistent = false, uint8_t nesting = ARDUINOJSON_DEFAULT_NESTING_LIMIT) {
    TraceAllocator allocator;
    ParseResult result;
    {
        JsonDocument doc(&allocator);
        doc["previous"] = std::string("previous owned value that must be cleared on failure too");
        CHECK(!doc.overflowed());
        allocator.trace.clear(); allocator.attempts = 0;
        allocator.failAt = failAt; allocator.persistent = persistent;
        // Vector has exactly size() input bytes, no terminating byte supplied.
        // Empty bounded parses use a valid pointer with explicit zero length.
        std::vector<uint8_t> raw(bytes.begin(), bytes.end());
        const uint8_t empty = 0;
        const uint8_t* pointer = raw.empty() ? &empty : raw.data();
        String text(bytes.data(), static_cast<unsigned int>(bytes.size()));
        DeserializationError error;
        const DeserializationOption::NestingLimit limit(nesting);
        if (mode == 0) error = deserializeJson(doc, pointer, raw.size(), limit);
        else if (mode == 1) error = deserializeJson(doc, reinterpret_cast<const char*>(pointer), raw.size(), limit);
        else if (mode == 2) {
            Original::JsonReader reader{text}; error = deserializeJson(doc, reader, limit);
            result.offset = reader.offset; result.remainingWhitespace = reader.complete();
        } else if (mode == 3) {
            Original::RfJsonReader reader{text}; error = deserializeJson(doc, reader, limit);
            result.offset = reader.offset; result.remainingWhitespace = reader.onlyWhitespaceRemaining();
        } else {
            Current::JsonReader reader{text}; error = deserializeJson(doc, reader, limit);
            result.offset = reader.offset; result.remainingWhitespace = reader.complete();
            CHECK(reader.complete() == reader.onlyWhitespaceRemaining());
        }
        CHECK(std::string(text.c_str(), text.length()) == bytes);
        CHECK(std::equal(raw.begin(), raw.end(), bytes.begin(), [](uint8_t a, char b) {
            return a == static_cast<unsigned char>(b);
        }));
        result.error = error.c_str(); result.overflowed = doc.overflowed();
        serializeJson(doc, result.json); shapeOf(doc.as<JsonVariantConst>(), result.shape);
        std::fill(raw.begin(), raw.end(), '!'); raw.clear(); raw.shrink_to_fit();
        text = String(std::string(4096, '?'));
        std::string after;
        serializeJson(doc, after);
        CHECK(after == result.json);  // Includes owned strings in partial DOM.
        CHECK(doc["previous"].isNull());
    }
    CHECK(allocator.sizes.empty());
    result.allocations = allocator.trace;
    result.attempts = allocator.attempts;
    result.growthFailure = allocator.growthFailure;
    return result;
}
static ParseResult compare(const std::string& bytes, unsigned failAt = 0,
                           bool persistent = false, uint8_t nesting = ARDUINOJSON_DEFAULT_NESTING_LIMIT) {
    const auto bounded = parse(bytes, 0, failAt, persistent, nesting);
    CHECK(bounded == parse(bytes, 1, failAt, persistent, nesting));
    const auto consumed = parse(bytes, 2, failAt, persistent, nesting);
    CHECK(consumed == parse(bytes, 3, failAt, persistent, nesting));
    CHECK(consumed == parse(bytes, 4, failAt, persistent, nesting));
    return consumed;
}
static std::vector<std::string> corpus() {
    std::vector<std::string> values = {
        "", " \t\r\n", "[]", "{}", "[0,65535]", "null", "true", "42", "-1.25e3",
        R"({"owned-key":"owned value","zero":"ab\u0000cd","unicode":"\u4e2d\u6587"})",
        R"({"utf8":"中文","escaped":"\"\\\t\n","nested":[true,null,{"a":1}]})",
        R"({"first":"valid before error","second":)", R"(["prefix",{"partial":1},)",
        "{]", "[1,]", "[1][2]", "[1] true", "[1]garbage", "[1] \t\r\n",
        R"({"x":1}{"y":2})", "123tail", std::string(12, '[') + "0" + std::string(12, ']')
    };
    const char inside[] = "{\"name\":\"a\0b\"}";
    const char tail[] = "[1]\0garbage";
    values.emplace_back(inside, sizeof(inside) - 1);
    values.emplace_back(tail, sizeof(tail) - 1);
    for (unsigned byte = 0x80; byte <= 0xff; ++byte) {
        values.push_back("{\"byte\":\"" + std::string(1, static_cast<char>(byte)) + "\"}");
        values.push_back("[1]" + std::string(1, static_cast<char>(byte)));
    }
    return values;
}
static void readers() {
    std::string bytes;
    for (unsigned byte = 0; byte <= 255; ++byte) bytes += static_cast<char>(byte);
    bytes += " \t\r\n";
    const String text(bytes.data(), static_cast<unsigned int>(bytes.size()));
    for (size_t offset = 0; offset <= bytes.size(); ++offset) {
        for (size_t count : {size_t(0), size_t(1), size_t(17), bytes.size() + 8}) {
            Original::JsonReader old{text}; Original::RfJsonReader rf{text}; Current::JsonReader current{text};
            for (size_t i = 0; i < offset; ++i) {
                const int expected = static_cast<unsigned char>(bytes[i]);
                CHECK(old.read() == expected); CHECK(rf.read() == expected); CHECK(current.read() == expected);
            }
            std::array<char, 512> a, b, c;
            a.fill('!'); b.fill('!'); c.fill('!');
            const size_t expected = std::min(count, bytes.size() - offset);
            CHECK(old.readBytes(a.data(), count) == expected);
            CHECK(rf.readBytes(b.data(), count) == expected);
            CHECK(current.readBytes(c.data(), count) == expected);
            CHECK(a == b && b == c);
            CHECK(old.offset == rf.offset && rf.offset == current.offset);
            CHECK(old.complete() == rf.onlyWhitespaceRemaining());
            CHECK(old.complete() == current.complete());
            while (old.offset < bytes.size()) {
                const int value = old.read(); CHECK(rf.read() == value); CHECK(current.read() == value);
            }
            CHECK(old.read() == -1 && rf.read() == -1 && current.read() == -1);
            CHECK(current.read() == -1 && current.offset == bytes.size());
        }
    }
}
static void parser() {
    for (const auto& bytes : corpus()) {
        for (size_t length = 0; length <= bytes.size(); ++length) compare(bytes.substr(0, length));
    }
    const std::string twelve = std::string(12, '[') + "0" + std::string(12, ']');
    CHECK(compare(twelve).error == "TooDeep");
    CHECK(compare(twelve, 0, false, 12).error == "Ok");
    CHECK(compare(twelve, 0, false, 11).error == "TooDeep");
    CHECK(compare("[1][2]").remainingWhitespace == false);
    CHECK(compare("[1] \t\r\n").remainingWhitespace);
    CHECK(compare("").error == "EmptyInput");
}
static std::string allocationPayload() {
    std::string bytes = "[";
    for (unsigned i = 0; i < 14; ++i) {
        if (i) bytes += ',';
        bytes += "{\"name\":\"item-" + std::to_string(i) + "-" + std::string(1024, char('a' + i)) +
                 "\",\"value\":" + std::to_string(i) + "}";
    }
    return bytes + "]";
}
static void oom() {
    const std::string bytes = allocationPayload();
    const auto success = compare(bytes);
    CHECK(success.error == "Ok" && success.attempts > 16);
    unsigned noMemory = 0, growth = 0, partial = 0;
    for (bool persistent : {false, true}) {
        for (unsigned failure = 1; failure <= success.attempts; ++failure) {
            const auto result = compare(bytes, failure, persistent);
            if (result.error == "NoMemory") {
                ++noMemory; growth += result.growthFailure;
                if (result.json != "null" && result.json != "[]") ++partial;
            }
        }
    }
    CHECK(noMemory && growth && partial);
}
static void mqtt() {
    CHECK(!Current::mqttPayloadIsStructuredJson(nullptr, 0));
    CHECK(!Current::mqttPayloadIsStructuredJson(nullptr, 4));
    auto cases = corpus();
    cases.push_back("010300010001D5CA"); cases.push_back("010300010001D5CB");
    cases.push_back("0Ab36E");
    cases.emplace_back("\x01\x03\x00\x01\x00\x01\xd5\xca", 8);
    for (const auto& bytes : cases) {
        const String text(bytes.data(), static_cast<unsigned int>(bytes.size()));
        for (size_t length = 0; length <= bytes.size(); ++length) {
            const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
            CHECK(Current::mqttPayloadIsStructuredJson(data, length) ==
                  Original::mqttPayloadIsStructuredJson(data, length));
        }
        for (uint8_t transfer : {uint8_t(0), uint8_t(1)}) {
            for (bool eligible : {false, true}) CHECK(Current::route(text, transfer, eligible) ==
                                                       Original::route(text, transfer, eligible));
        }
    }
    CHECK(Current::route("[1][2]", 0, false).accepted);  // Existing first-root probe policy.
    CHECK(!Current::route("010300010001D5CB", 1, true).accepted);
    CHECK(!Current::route("010300010001D5CA", 0, true).accepted);
    CHECK(!Current::route("010300010001D5CA", 1, false).accepted);
    CHECK(Current::route("010300010001D5CA", 1, true).payload ==
          "[{\"id\":\"modbus_raw_send\",\"value\":\"010300010001D5CA\"}]");
    CHECK(Current::route("0Ab36E", 1, true).payload ==
          "[{\"id\":\"modbus_raw_send\",\"value\":\"304162333645\"}]");
    uint8_t frame[256]{}; size_t length = 0;
    CHECK(Current::decodeModbusRtuPayload(reinterpret_cast<const uint8_t*>("0Ab36E"), 6,
          frame, sizeof(frame), length) == Current::MqttModbusPayloadEncoding::BINARY);
    CHECK(length == 6 && std::memcmp(frame, "0Ab36E", 6) == 0);
    CHECK(Current::decodeModbusRtuPayload(reinterpret_cast<const uint8_t*>("0Ab36E"), 6,
          frame, 5, length) == Current::MqttModbusPayloadEncoding::INVALID && length == 0);
}
static bool sameKeys(const std::vector<RfKeyEntry>& a, const std::vector<RfKeyEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].code != b[i].code ||
            a[i].bitLength != b[i].bitLength || a[i].pulseWidth != b[i].pulseWidth) return false;
    return true;
}
static void entries() {
    auto cases = corpus();
    cases.push_back(R"([{"name":"key","code":"0x123456","bits":24,"pulse":350}])");
    cases.push_back(R"([{"name":"key","code":"1"},{"name":"key","code":"2"}])");
    cases.push_back(R"([{"name":"good","code":"1"},{"name":"bad!","code":"2"}])");
    cases.push_back(R"([{"name":"ab\u0000cd","code":"1"}])");
    cases.push_back(R"([{"name":"key","code":"1","bits":11}])");
    for (const auto& bytes : cases) {
        String text(bytes.data(), static_cast<unsigned int>(bytes.size()));
        JsonDocument a, b;
        CHECK(Current::registerArray(text, a) == Original::registerArray(text, b));
        std::string encodedA, encodedB; serializeJson(a, encodedA); serializeJson(b, encodedB);
        CHECK(encodedA == encodedB && a.overflowed() == b.overflowed());
        std::vector<RfKeyEntry> first{{"previous", 99, 24, 350}}, second = first;
        CHECK(Current::parseRfKeys(text, first) == Original::parseRfKeys(text, second));
        CHECK(sameKeys(first, second));
    }
    JsonDocument doc;
    CHECK(Current::registerArray("[1] \t\r\n", doc));
    CHECK(!Current::registerArray("[1][2]", doc));
    CHECK(!Current::registerArray("[1]junk", doc));
    const char hidden[] = "[1]\0 ";
    CHECK(!Current::registerArray(String(hidden, sizeof(hidden) - 1), doc));
    CHECK(!Current::registerArray("{}", doc));
    CHECK(!Current::registerArray("", doc));
    std::vector<RfKeyEntry> keys{{"previous", 99, 24, 350}};
    CHECK(Current::parseRfKeys("", keys) && keys.empty());  // RF's distinct empty behavior.
    CHECK(!Current::parseRfKeys("[][]", keys));
    CHECK(!Current::parseRfKeys(String(hidden, sizeof(hidden) - 1), keys));
    CHECK(Current::parseRfKeys("[] \r\n\t", keys));
}

struct EntryResult {
    bool accepted = false, overflowed = false, growthFailure = false;
    std::string partial;
    std::vector<std::string> allocations;
    std::vector<RfKeyEntry> keys;
    unsigned attempts = 0;
    bool operator==(const EntryResult& other) const {
        return accepted == other.accepted && overflowed == other.overflowed && partial == other.partial &&
               allocations == other.allocations && sameKeys(keys, other.keys) && attempts == other.attempts &&
               growthFailure == other.growthFailure;
    }
};
static EntryResult entry(const std::string& bytes, unsigned kind, bool candidate,
                         unsigned failAt = 0, bool persistent = false) {
    TraceAllocator allocator;
    allocator.failAt = failAt; allocator.persistent = persistent;
    EntryResult result;
    String text(bytes.data(), static_cast<unsigned int>(bytes.size()));
    if (kind == 0) {
        JsonDocument doc(&allocator);
        result.accepted = candidate ? Current::registerArray(text, doc) : Original::registerArray(text, doc);
        result.overflowed = doc.overflowed(); serializeJson(doc, result.partial);
    } else if (kind == 1) {
        CurrentFault::allocator = &allocator; OriginalFault::allocator = &allocator;
        const auto* data = reinterpret_cast<const uint8_t*>(text.c_str());
        result.accepted = candidate ? CurrentFault::mqttPayloadIsStructuredJson(data, text.length()) :
                                      OriginalFault::mqttPayloadIsStructuredJson(data, text.length());
        CurrentFault::allocator = nullptr; OriginalFault::allocator = nullptr;
    } else {
        CurrentFault::allocator = &allocator; OriginalFault::allocator = &allocator;
        result.keys.push_back({"previous", 99, 24, 350});
        result.accepted = candidate ? CurrentFault::parseRfKeys(text, result.keys) :
                                      OriginalFault::parseRfKeys(text, result.keys);
        CurrentFault::allocator = nullptr; OriginalFault::allocator = nullptr;
    }
    CHECK(allocator.sizes.empty());
    result.allocations = allocator.trace; result.attempts = allocator.attempts;
    result.growthFailure = allocator.growthFailure;
    return result;
}
static void entryOom() {
    std::string keys = "[";
    for (unsigned i = 0; i < 40; ++i) {
        if (i) keys += ',';
        // Unknown field remains permitted but exercises actual parser growth.
        keys += "{\"name\":\"key" + std::to_string(i) + "\",\"code\":\"0x123456\",\"note\":\"" +
                std::string(260, char('a' + i % 26)) + "\"}";
    }
    keys += ']';
    unsigned rejected = 0, growth = 0;
    for (unsigned kind = 0; kind < 3; ++kind) {
        const std::string bytes = kind == 2 ? keys : allocationPayload();
        const auto success = entry(bytes, kind, true);
        CHECK(success.accepted);
        CHECK(success == entry(bytes, kind, false));
        for (bool persistent : {false, true}) {
            for (unsigned failure = 1; failure <= success.attempts; ++failure) {
                const auto current = entry(bytes, kind, true, failure, persistent);
                CHECK(current == entry(bytes, kind, false, failure, persistent));
                rejected += !current.accepted; growth += current.growthFailure;
            }
        }
    }
    CHECK(rejected && growth);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string mode = argv[1];
    if (mode == "readers") readers();
    else if (mode == "parser") parser();
    else if (mode == "oom") oom();
    else if (mode == "mqtt") mqtt();
    else if (mode == "entries") entries();
    else if (mode == "entry-oom") entryOom();
    else return 2;
}
