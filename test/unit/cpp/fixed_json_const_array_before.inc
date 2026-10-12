// Frozen pre-candidate implementation; independent of the current helper.
namespace FixedJsonConstArrayBefore {
inline JsonVariant field(JsonObject object, const char* key) {
    auto member = object[JsonString(key, true)];
    JsonVariant result = member.as<JsonVariant>();
    return result.isUnbound() ? member.to<JsonVariant>() : result;
}
inline JsonVariant field(JsonDocument& document, const char* key) {
    auto member = document[JsonString(key, true)];
    JsonVariant result = member.as<JsonVariant>();
    return result.isUnbound() ? member.to<JsonVariant>() : result;
}
template<size_t N, typename Value>
bool put(JsonObject object, const char (&key)[N], Value&& value) {
    return field(object, key).set(std::forward<Value>(value));
}
template<size_t N, typename Value>
bool put(JsonDocument& document, const char (&key)[N], Value&& value) {
    return field(document, key).set(std::forward<Value>(value));
}
}
