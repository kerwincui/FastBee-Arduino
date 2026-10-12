bool verifyJsonFileBefore(const String& path, JsonDocument* out = nullptr) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) return false;
    JsonDocument parsed;
    DeserializationError error = deserializeJson(parsed, file);
    file.close();
    if (error) return false;
    if (out) *out = parsed;
    return true;
}
