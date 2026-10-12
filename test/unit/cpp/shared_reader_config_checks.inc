// Byte-level File seam: EOF and one short-read failure remain observable.
// No filesystem, serial or device operations are used.
struct File {
    std::string bytes;
    size_t position = 0, eofAt, failReadAt;
    bool failedRead = false;
    std::vector<std::string> io;
    explicit File(const std::string& input, size_t cap = size_t(-1), size_t fail = size_t(-1))
        : bytes(input), eofAt(std::min(cap, input.size())), failReadAt(fail) {}
    int peek() {
        const int value = position < eofAt ? static_cast<unsigned char>(bytes[position]) : -1;
        io.push_back("peek:" + std::to_string(position) + ":" + std::to_string(value));
        return value;
    }
    int read() {
        const size_t before = position;
        int value = -1;
        if (!failedRead && position == failReadAt) failedRead = true;
        else if (position < eofAt) value = static_cast<unsigned char>(bytes[position++]);
        io.push_back("read:" + std::to_string(before) + ":" + std::to_string(value));
        return value;
    }
};
// @CONFIG_CHECKS@
struct ConfigResult {
    bool accepted = false, overflowed = false, growthFailure = false;
    size_t position = 0;
    int next = -1;
    unsigned attempts = 0;
    std::string error, json;
    std::vector<std::string> shape, allocationTrace, io;
    bool operator==(const ConfigResult& b) const {
        return accepted == b.accepted && overflowed == b.overflowed &&
            growthFailure == b.growthFailure && position == b.position && next == b.next &&
            attempts == b.attempts && error == b.error && json == b.json &&
            shape == b.shape && allocationTrace == b.allocationTrace && io == b.io;
    }
};
static ConfigResult configParse(const std::string& bytes, bool candidate,
        unsigned failAt = 0, bool persistent = false, size_t eofAt = size_t(-1),
        size_t readFailure = size_t(-1)) {
    TraceAllocator allocator;
    ConfigResult result;
    {
        JsonDocument doc(&allocator);
        doc["previous"] = std::string("previous owned value must be cleared");
        CHECK(!doc.overflowed());
        allocator.trace.clear(); allocator.attempts = 0;
        allocator.failAt = failAt; allocator.persistent = persistent;
        File file(bytes, eofAt, readFailure);
        String error("previous-error");
        result.accepted = candidate ? ConfigCurrent::readConfigValue(file, doc, error) :
                                      ConfigOriginal::readConfigValue(file, doc, error);
        result.error = std::string(error.c_str(), error.length());
        result.overflowed = doc.overflowed();
        result.position = file.position; result.next = file.peek();
        result.io = file.io;
        serializeJson(doc, result.json); shapeOf(doc.as<JsonVariantConst>(), result.shape);
        CHECK(doc["previous"].isNull());
        file.bytes.assign(bytes.size() + 4096, '?');
        std::string after; serializeJson(doc, after);
        CHECK(after == result.json);
    }
    CHECK(allocator.sizes.empty());
    result.attempts = allocator.attempts;
    result.growthFailure = allocator.growthFailure;
    result.allocationTrace = allocator.trace;
    return result;
}
static ConfigResult compareConfig(const std::string& bytes, unsigned failAt = 0,
        bool persistent = false, size_t eofAt = size_t(-1), size_t readFailure = size_t(-1)) {
    const auto value = configParse(bytes, true, failAt, persistent, eofAt, readFailure);
    CHECK(value == configParse(bytes, false, failAt, persistent, eofAt, readFailure));
    return value;
}
static void configSequence(bool candidate) {
    File file("42,{\"x\":1},[2],true");
    const char* expected[] = {"42", "{\"x\":1}", "[2]", "true"};
    JsonDocument doc;
    for (unsigned i = 0; i < 4; ++i) {
        String error("untouched");
        const bool ok = candidate ? ConfigCurrent::readConfigValue(file, doc, error) :
                                    ConfigOriginal::readConfigValue(file, doc, error);
        CHECK(ok && error == "untouched");
        std::string encoded; serializeJson(doc, encoded);
        CHECK(encoded == expected[i]);
        if (i != 3) CHECK(file.read() == ',');
        else CHECK(file.peek() == -1);
    }
}
static void configCases() {
    for (const auto& bytes : corpus()) {
        for (size_t count = 0; count <= bytes.size(); ++count) {
            compareConfig(bytes, 0, false, count);
            if (count < bytes.size()) compareConfig(bytes, 0, false, size_t(-1), count);
        }
    }
    for (const auto& bytes : {"42,", "42}", "42]", "42 ", "-1.25e3,", "true,", "false}",
            "null]", "\"text\",", "'text',", "[1],", "{\"x\":1},",
            "{\"escaped\":\"a\\\"b\"},", "[{\"x\":[1,2]}],following"}) {
        compareConfig(bytes);
    }
    for (const auto& bytes : {"42,", "[1],", "{\"x\":1},", "\"x\","}) {
        const auto value = compareConfig(bytes);
        CHECK(value.accepted && value.next == ',');
        CHECK(value.error == "previous-error");
    }
    configSequence(false); configSequence(true);
}
static void configOom() {
    const std::string bytes = allocationPayload() + ",following";
    const auto success = compareConfig(bytes);
    CHECK(success.accepted && success.next == ',' && success.attempts > 16);
    unsigned pressure = 0, growth = 0, partial = 0;
    for (bool persistent : {false, true}) {
        for (unsigned failAt = 1; failAt <= success.attempts; ++failAt) {
            const auto result = compareConfig(bytes, failAt, persistent);
            if (!result.accepted) {
                CHECK(result.error == "CONFIG_MEMORY_PRESSURE");
                ++pressure; growth += result.growthFailure;
                if (result.json != "null" && result.json != "[]") ++partial;
            }
        }
    }
    CHECK(pressure && growth && partial);
}
