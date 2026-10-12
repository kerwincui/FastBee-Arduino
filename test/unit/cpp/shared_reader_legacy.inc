namespace Original {
inline bool registerArray(const String& text, JsonDocument& values) {
    JsonReader reader{text};
    return !deserializeJson(values, reader) && reader.complete() &&
        !values.overflowed() && values.is<JsonArray>();
}
bool validRfName(const String& name) {
    if (name.isEmpty() || name.length() > 32) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ') continue;
        return false;
    }
    return true;
}

bool parseRfKeyCode(const String& code, uint32_t& value) {
    size_t at = code.startsWith("0x") || code.startsWith("0X") ? 2 : 0;
    if (at == code.length() || code.length() - at > 8) return false;
    value = 0;
    for (; at < code.length(); ++at) {
        const char c = code[at];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

bool parseRfKeys(const String& raw, std::vector<RfKeyEntry>& keys) {
    keys.clear(); if (raw.isEmpty()) return true;
    JsonDocument doc; RfJsonReader reader{raw};
    if (deserializeJson(doc, reader) || !reader.onlyWhitespaceRemaining() ||
        doc.overflowed() || !doc.is<JsonArray>() || doc.size() > 40) return false;
    for (JsonVariantConst item : doc.as<JsonArrayConst>()) {
        if (!item.is<JsonObjectConst>() || !item["name"].is<const char*>() || !item["code"].is<const char*>()) return false;
        const JsonString name = item["name"].as<JsonString>(), code = item["code"].as<JsonString>();
        if (name.size() != std::strlen(name.c_str()) || code.size() != std::strlen(code.c_str())) return false;
        if ((!item["bits"].isNull() && !item["bits"].is<uint8_t>()) ||
            (!item["pulse"].isNull() && !item["pulse"].is<uint16_t>())) return false;
        const String codeText(code.c_str());
        RfKeyEntry entry{String(name.c_str()), 0, item["bits"] | uint8_t(24), item["pulse"] | uint16_t(350)};
        if (entry.name.length() != name.size() || !validRfName(entry.name) ||
            codeText.length() != code.size() || !parseRfKeyCode(codeText, entry.code) || entry.bitLength < 12 || entry.bitLength > 32 ||
            entry.pulseWidth < 80 || entry.pulseWidth > 2000 ||
            (entry.bitLength < 32 && (entry.code >> entry.bitLength))) return false;
        for (const auto& old : keys) if (old.name == entry.name) return false;
        keys.push_back(std::move(entry));
    }
    return true;
}
}

namespace ConfigOriginal {
class ConfigValueReader {
public:
    explicit ConfigValueReader(File& file) : file_(file) {}
    int read() {
        if (done_) return -1;
        const int next = file_.peek();
        if (next < 0) return -1;
        if (started_ && depth_ == 0 && !quote_ &&
            (next == ',' || next == '}' || next == ']' || isspace(next))) return -1;
        const char c = static_cast<char>(file_.read());
        if (!started_) started_ = true;
        if (quote_) {
            if (escaped_) escaped_ = false;
            else if (c == '\\') escaped_ = true;
            else if (c == quote_) { quote_ = 0; if (depth_ == 0) done_ = true; }
        } else if (c == '"' || c == '\'') quote_ = c;
        else if (c == '{' || c == '[') ++depth_;
        else if (c == '}' || c == ']') { if (--depth_ <= 0) done_ = true; }
        return static_cast<unsigned char>(c);
    }
    size_t readBytes(char* buffer, size_t count) {
        size_t copied = 0;
        while (copied < count) { int c = read(); if (c < 0) break; buffer[copied++] = static_cast<char>(c); }
        return copied;
    }
private:
    File& file_;
    int depth_ = 0;
    char quote_ = 0;
    bool started_ = false, escaped_ = false, done_ = false;
};
static bool configError(String& error, const char* code) { error = code; return false; }
static bool readConfigValue(File& file, JsonDocument& doc, String& error) {
    ConfigValueReader input(file);
    const auto parsed = deserializeJson(doc, input);
    if (!parsed) return true;
    return configError(error, parsed == DeserializationError::NoMemory ? "CONFIG_MEMORY_PRESSURE" : "CONFIG_JSON");
}
}
