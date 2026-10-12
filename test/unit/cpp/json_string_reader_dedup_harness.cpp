// Compare ArduinoJson's actual String readers with the exact bounded input used
// by production. Host library semantics only; no device or firmware execution.
#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

static_assert(ARDUINOJSON_VERSION_MAJOR == 7 && ARDUINOJSON_VERSION_MINOR == 4 &&
              ARDUINOJSON_VERSION_REVISION == 2, "Review reader semantics after an ArduinoJson upgrade");
static_assert(std::is_base_of<ArduinoJson::detail::BoundedReader<const char*>,
              ArduinoJson::detail::Reader<String>>::value, "Actual mutable String reader changed");
static_assert(std::is_base_of<ArduinoJson::detail::BoundedReader<const char*>,
              ArduinoJson::detail::Reader<const String>>::value, "Actual const String reader changed");

struct TraceAllocator final : ArduinoJson::Allocator {
    std::vector<std::string> trace;
    std::map<void*, size_t> sizes;
    unsigned attempt = 0, failAt = 0;
    void* allocate(size_t bytes) override {
        trace.push_back("allocate:" + std::to_string(bytes));
        if (++attempt == failAt) return nullptr;
        void* pointer = std::malloc(bytes);
        if (pointer) sizes[pointer] = bytes;
        return pointer;
    }
    void* reallocate(void* pointer, size_t bytes) override {
        trace.push_back("reallocate:" + std::to_string(bytes));
        // ArduinoJson StringBuilder::save requires shrinking reallocations to
        // succeed. Retain the existing block for this no-growth operation;
        // inject failures in allocations and growth, where NoMemory is defined.
        // The original r13/r14 fixture violated this allocator contract and its
        // access-violation evidence is retained separately.
        if (pointer && bytes <= sizes.at(pointer)) {
            sizes[pointer] = bytes;
            return pointer;
        }
        if (++attempt == failAt) return nullptr;
        void* replacement = std::realloc(pointer, bytes);
        if (replacement) {
            sizes.erase(pointer);
            sizes[replacement] = bytes;
        }
        return replacement;
    }
    void deallocate(void* pointer) override {
        trace.push_back(pointer ? "deallocate" : "deallocate-null");
        sizes.erase(pointer);
        std::free(pointer);
    }
};

struct Result {
    std::string error, json;
    bool overflowed;
    std::vector<std::string> allocations;
    bool operator==(const Result& other) const {
        return error == other.error && json == other.json &&
               overflowed == other.overflowed && allocations == other.allocations;
    }
};

Result parse(const std::string& bytes, unsigned mode, unsigned failAt = 0,
             uint8_t nesting = ARDUINOJSON_DEFAULT_NESTING_LIMIT) {
    if (failAt) std::cerr << "allocation-case=" << failAt << " reader=" << mode << " start\n";
    TraceAllocator allocator;
    JsonDocument document(&allocator);
    // Parsing must clear the previous document equally, even on allocation
    // failure or malformed input. Do not start all comparisons with empty DOMs.
    document["previous"] = std::string("owned previous value long enough for allocation");
    assert(!document.overflowed());
    allocator.trace.clear();
    allocator.attempt = 0;
    allocator.failAt = failAt;

    String input(bytes.data(), static_cast<unsigned int>(bytes.size()));
    const std::string before(input.c_str(), input.length());
    DeserializationError error;
    if (mode == 0) {
        error = deserializeJson(document, input, DeserializationOption::NestingLimit(nesting));
    } else if (mode == 1) {
        const String& constant = input;
        error = deserializeJson(document, constant, DeserializationOption::NestingLimit(nesting));
    } else {
        error = deserializeJson(document, input.c_str(), input.length(),
                                DeserializationOption::NestingLimit(nesting));
    }
    assert(std::string(input.c_str(), input.length()) == before);
    if (failAt) std::cerr << "parsed=" << error.c_str() << " attempts=" << allocator.attempt << "\n";
    std::string encoded;
    serializeJson(document, encoded);
    const auto trace = allocator.trace;
    const bool overflowed = document.overflowed();
    // Parsed keys and values must remain owned after the request body/source
    // String is replaced/freed, including any partial DOM from a failed parse.
    input = String(std::string(4096, '!'));
    std::string afterReplacement;
    serializeJson(document, afterReplacement);
    assert(afterReplacement == encoded);
    assert(document["previous"].isNull());
    return {error.c_str(), encoded, overflowed, trace};
}

Result compare(const std::string& bytes, unsigned failAt = 0,
               uint8_t nesting = ARDUINOJSON_DEFAULT_NESTING_LIMIT) {
    const auto mutableString = parse(bytes, 0, failAt, nesting);
    const auto constString = parse(bytes, 1, failAt, nesting);
    const auto bounded = parse(bytes, 2, failAt, nesting);
    assert(mutableString == constString);
    assert(mutableString == bounded);
    return bounded;
}

void syntax() {
    const std::vector<std::string> inputs = {
        "", " \r\n\t", "{}", "[]", "true", "null", "42", "-7", "1.25",
        R"({"name":"owned","i":18446744073709551615,"f":1.25,"b":false,"n":null})",
        R"({"escape":"ab\u0000cd","utf8":"\u4e2d\u6587","quote":"\"\\","a":[1,true,null]})",
        R"({"broken":)", R"({"x":"unterminated)", R"([1,2,)", "{]", "not-json",
        R"({"one":1}{"two":2})", "[1][2]", "123tail", R"({"one":1} trailing)"
    };
    for (const auto& bytes : inputs) compare(bytes);
    const char insideString[] = "{\"name\":\"a\0b\"}";
    const char afterRoot[] = "{\"name\":\"owned\"}\0garbage";
    compare(std::string(insideString, sizeof(insideString) - 1));
    compare(std::string(afterRoot, sizeof(afterRoot) - 1));
    assert(compare(R"({"name":"owned"})").error == "Ok");
    assert(compare("").error == "EmptyInput");
    assert(compare(R"({"broken":)").error == "IncompleteInput");
}

void nestingAndLength() {
    const std::string twelve = std::string(12, '[') + "0" + std::string(12, ']');
    assert(compare(twelve, 0, 12).error == "Ok");
    assert(compare(twelve, 0, 11).error == "TooDeep");
    assert(compare(twelve).error == "TooDeep");
    const std::string thirteen = "[" + twelve + "]";
    assert(compare(thirteen, 0, 12).error == "TooDeep");

    const char exact[] = "{\"a\":123}unread-suffix";
    assert(compare(std::string(exact, 9)).error == "Ok");
    assert(compare(std::string(exact, 8)).error == "IncompleteInput");
    const char bytes[] = {'a', '\0', static_cast<char>(0xff), 'z'};
    String input(bytes, sizeof(bytes));
    auto stringReader = ArduinoJson::detail::makeReader(input);
    auto boundedReader = ArduinoJson::detail::makeReader(input.c_str(), input.length());
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        const int expected = static_cast<unsigned char>(bytes[i]);
        assert(stringReader.read() == expected);
        assert(boundedReader.read() == expected);
    }
    assert(stringReader.read() == -1);
    assert(boundedReader.read() == -1);
    char fromString[8]{}, fromBounded[8]{};
    auto first = ArduinoJson::detail::makeReader(input);
    auto second = ArduinoJson::detail::makeReader(input.c_str(), input.length());
    assert(first.readBytes(fromString, sizeof(fromString)) == sizeof(bytes));
    assert(second.readBytes(fromBounded, sizeof(fromBounded)) == sizeof(bytes));
    assert(std::memcmp(fromString, fromBounded, sizeof(fromString)) == 0);
}

void ownership() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        String input(R"({"name":"source-owned-value","zero":"ab\u0000cd"})");
        JsonDocument document;
        DeserializationError error;
        if (mode == 0) error = deserializeJson(document, input);
        else if (mode == 1) {
            const String& constant = input;
            error = deserializeJson(document, constant);
        } else error = deserializeJson(document, input.c_str(), input.length());
        assert(!error && !document.overflowed());
        for (JsonPairConst pair : document.as<JsonObjectConst>()) {
            assert(!pair.key().isStatic());
            assert(!pair.value().as<JsonString>().isStatic());
        }
        input = "modified and replaced";
        assert(document["name"] == "source-owned-value");
        const JsonString zero = document["zero"].as<JsonString>();
        const char expected[] = {'a', 'b', '\0', 'c', 'd'};
        assert(zero.size() == sizeof(expected));
        assert(std::memcmp(zero.c_str(), expected, sizeof(expected)) == 0);
    }
}

void allocationFailures() {
    std::string bytes = "{\"items\":[";
    for (unsigned i = 0; i < 40; ++i) {
        if (i) bytes += ',';
        bytes += "{\"name\":\"item-" + std::to_string(i) + "-" +
                 std::string(90, static_cast<char>('a' + i % 26)) + "\",\"value\":" +
                 std::to_string(i) + "}";
    }
    bytes += "]}";
    assert(compare(bytes).error == "Ok");
    unsigned noMemory = 0;
    for (unsigned failAt = 1; failAt <= 16; ++failAt) {
        const auto result = compare(bytes, failAt);
        if (result.error == "NoMemory") ++noMemory;
    }
    assert(noMemory == 16);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode(argv[1]);
    if (mode == "syntax") syntax();
    else if (mode == "length-nesting") nestingAndLength();
    else if (mode == "ownership") ownership();
    else if (mode == "allocation-failures") allocationFailures();
    else return 2;
}
