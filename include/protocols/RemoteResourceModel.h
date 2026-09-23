#pragma once

#include <ArduinoJson.h>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

// Shared by firmware and native tests. Paths are data selectors, never expressions.
namespace FastBee { namespace RemoteResourceModel {
constexpr unsigned MAX_CHANNELS = 0;
constexpr unsigned MAX_WEATHER = 1;
constexpr unsigned MAX_BODY = 8192;

inline bool identifier(const char* s) {
    if (!s || !*s || strlen(s) > 40) return false;
    for (; *s; ++s) if (!(isalnum(static_cast<unsigned char>(*s)) || *s == '_' || *s == '-')) return false;
    return true;
}

inline bool pathValid(const char* s) {
    if (!s || strlen(s) > 128) return false;
    unsigned segments = 0;
    bool empty = true;
    for (; *s; ++s) {
        if (*s == '.') { if (empty || ++segments > 11) return false; empty = true; }
        else {
            if (!(isalnum(static_cast<unsigned char>(*s)) || *s == '_' || *s == '-')) return false;
            empty = false;
        }
    }
    return !empty || segments == 0;
}

inline bool queryNameValid(const char* s) {
    if (!s || !*s) return true;
    if (strlen(s) > 64) return false;
    for (; *s; ++s) {
        if (!(isalnum(static_cast<unsigned char>(*s)) || *s == '_' || *s == '-' || *s == '.')) return false;
    }
    return true;
}

inline JsonVariantConst field(JsonVariantConst root, const char* path) {
    if (!path || !*path) return root;
    if (!pathValid(path)) return JsonVariantConst();
    std::string p(path);
    size_t start = 0;
    while (start < p.size()) {
        size_t end = p.find('.', start);
        if (end == std::string::npos) end = p.size();
        std::string key = p.substr(start, end - start);
        if (root.is<JsonArrayConst>()) {
            char* tail = nullptr;
            unsigned long idx = strtoul(key.c_str(), &tail, 10);
            if (!tail || *tail || key.empty() || idx >= root.size()) return JsonVariantConst();
            root = root[static_cast<size_t>(idx)];
        } else if (root.is<JsonObjectConst>()) root = root[key.c_str()];
        else return JsonVariantConst();
        start = end + 1;
    }
    return root;
}

inline bool number(JsonVariantConst v, double& result) {
    if (v.isNull() || v.is<bool>() || v.is<JsonObjectConst>() || v.is<JsonArrayConst>()) return false;
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s || !*s) return false;
        char* end = nullptr;
        result = strtod(s, &end);
        if (end == s || !end) return false;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
        return !*end && std::isfinite(result);
    }
    if (!v.is<double>()) return false;
    result = v.as<double>();
    return std::isfinite(result);
}

inline bool urlValid(const char* s) {
    if (!s || strlen(s) > 512) return false;
    size_t offset = strncmp(s,"https://",8) == 0 ? 8 : (strncmp(s,"http://",7) == 0 ? 7 : 0);
    if (!offset || !s[offset] || s[offset] == '/' || s[offset] == '?' || s[offset] == '#') return false;
    for (const char* p = s; *p; ++p) if (static_cast<unsigned char>(*p) <= 32 || *p == '#' || *p == '\\') return false;
    const char* end = strpbrk(s + offset,"/?");
    if (!end) end = s + strlen(s);
    for (const char* p = s + offset; p < end; ++p) if (*p == '@') return false;
    return true;
}

inline bool topicValid(const char* s, bool optional = false) {
    if (!s || !*s) return optional;
    if (strlen(s) > 160 || strstr(s,"/http/upgrade/") || s[0] == '$') return false;
    for (; *s; ++s) if (static_cast<unsigned char>(*s) < 32 || *s == '+' || *s == '#' || *s == '{' || *s == '}') return false;
    return true;
}

inline bool topicMatches(const char* filter, const char* topic) {
    if (!filter || !topic || !*filter) return false;
    if (topic[0] == '$' && (filter[0] == '#' || filter[0] == '+')) return false;
    const char* f = filter;
    const char* t = topic;
    for (;;) {
        if (f[0] == '#' && f[1] == '\0') return true;
        const char* fe = strchr(f, '/');
        const char* te = strchr(t, '/');
        const size_t fn = fe ? static_cast<size_t>(fe - f) : strlen(f);
        const size_t tn = te ? static_cast<size_t>(te - t) : strlen(t);
        if (!(fn == 1 && f[0] == '+') && (fn != tn || strncmp(f, t, fn))) return false;
        if (!te) return !fe || strcmp(fe, "/#") == 0;
        if (!fe) return false;
        f = fe + 1;
        t = te + 1;
    }
}

inline bool oneOf(const char* value, const char* a, const char* b, const char* c = nullptr) {
    return value && (!strcmp(value,a) || !strcmp(value,b) || (c && !strcmp(value,c)));
}

inline std::string sourceRevision(JsonObjectConst source) {
    uint32_t hash = 2166136261U;
    for (const char* key : {"type", "stateTopic", "valuePath", "unit", "onValue", "offValue",
                            "provider", "url", "location", "apiKeyParam", "fields", "codeMap"}) {
        std::string value;
        serializeJson(source[key], value);
        for (unsigned char c : value) { hash ^= c; hash *= 16777619U; }
        hash ^= 0xff;
        hash *= 16777619U;
    }
    std::string result(8, '0');
    for (int i = 7; i >= 0; --i) { result[i] = "0123456789abcdef"[hash & 15U]; hash >>= 4U; }
    return result;
}

inline bool boundedText(JsonVariantConst v, size_t size, bool required = false) {
    if (v.isNull()) return !required;
    if (!v.is<const char*>()) return false;
    const char* s = v.as<const char*>();
    return strlen(s) <= size && (!required || *s);
}

inline bool seconds(JsonVariantConst v, unsigned lo, unsigned hi) {
    return v.isNull() || (v.is<unsigned>() && v.as<unsigned>() >= lo && v.as<unsigned>() <= hi);
}

inline const char* condition(const char* code) {
    if (!code || !*code) return "unknown";
    char* end = nullptr;
    long n = strtol(code,&end,10);
    if (*end) return "unknown";
    if (n == 0 || n == 2) return "sunny";
    if (n == 1 || n == 3) return "clear-night";
    if (n >= 4 && n <= 8) return "cloudy";
    if (n == 9) return "overcast";
    if (n == 11 || n == 12) return "thunder";
    if (n == 10 || (n >= 13 && n <= 19)) return "rain";
    if (n >= 20 && n <= 25) return "snow";
    if (n >= 26 && n <= 31) return "fog";
    if (n >= 32 && n <= 36) return "wind";
    if (n == 37) return "cold";
    if (n == 38) return "hot";
    return "unknown";
}

inline bool conditionValid(const char* s) {
    const char* names[] = {"sunny","clear-night","cloudy","overcast","thunder","rain","snow","fog","wind","cold","hot","unknown"};
    for (auto name : names) if (s && !strcmp(s,name)) return true;
    return false;
}

// The only active product source is Seniverse current weather. Legacy fields are
// accepted only by migration, never by active configuration validation.
inline void fixedWeather(JsonObject out, JsonObjectConst old) {
    out["id"] = identifier(old["id"] | "") ? old["id"].as<const char*>() : "local";
    out["label"] = "心知天气"; out["provider"] = "seniverse";
    out["enabled"] = old["enabled"] | false;
    out["url"] = "https://api.seniverse.com/v3/weather/now.json";
    out["apiKey"] = old["apiKey"] | ""; out["location"] = old["location"] | "beijing";
    const uint32_t interval = old["intervalSeconds"] | 1800U;
    out["intervalSeconds"] = interval == 900 || interval == 3600 ? interval : 1800;
    out["staleAfterSeconds"] = std::max<uint32_t>(3600, (out["intervalSeconds"] | 1800U) * 2);
}

inline bool validate(JsonVariantConst config, std::string& error);

inline void migrateWeather(JsonVariantConst legacy, JsonObject active, JsonObject report) {
    active["remoteChannels"].to<JsonArray>();
    JsonArray weather = active["weatherSources"].to<JsonArray>();
    JsonArrayConst old = legacy["weatherSources"].as<JsonArrayConst>();
    JsonArray retired = report["retiredChannels"].to<JsonArray>();
    for (JsonObjectConst channel : legacy["remoteChannels"].as<JsonArrayConst>()) {
        JsonObject item = retired.add<JsonObject>();
        item["id"] = channel["id"]; item["label"] = channel["label"];
    }
    const bool single = old.size() == 1 && old[0]["provider"] == "seniverse";
    report["required"] = old.size() && !single;
    if (single) {
        fixedWeather(weather.add<JsonObject>(), old[0]);
        std::string error;
        if (validate(active,error)) return;
        // Preserve an invalid legacy source for explicit repair instead of saving
        // an unusable active source or silently replacing its identity.
        report["required"] = true;
        weather.clear();
    }
    {
        fixedWeather(weather.add<JsonObject>(), JsonObjectConst());
        JsonArray candidates = report["candidates"].to<JsonArray>();
        for (JsonObjectConst source : old) {
            JsonObject item = candidates.add<JsonObject>();
            item["id"] = source["id"]; item["label"] = source["label"];
            item["location"] = source["location"];
            item["compatible"] = source["provider"] == "seniverse";
        }
    }
}

inline bool validate(JsonVariantConst config, std::string& error) {
    auto fail = [&](const char* message) { error = message; return false; };
    if (!config.is<JsonObjectConst>()) return fail("Weather settings must be an object");
    if (!config["remoteChannels"].is<JsonArrayConst>() || config["remoteChannels"].size())
        return fail("Remote channels are retired");
    if (!config["weatherSources"].is<JsonArrayConst>() || config["weatherSources"].size() > MAX_WEATHER)
        return fail("Only one Seniverse weather source is supported");
    for (JsonObjectConst w : config["weatherSources"].as<JsonArrayConst>()) {
        if (!identifier(w["id"] | "") || w["provider"] != "seniverse" || !w["enabled"].is<bool>() || !boundedText(w["label"],64,true))
            return fail("Invalid Seniverse source");
        if (w["url"] != "https://api.seniverse.com/v3/weather/now.json" || !boundedText(w["apiKey"],256) || !boundedText(w["location"],80,true))
            return fail("Invalid weather location or credential");
        for (const char* retired : {"authHeader","apiKeyParam","fields","codeMap","forecastUrl","forecastIntervalSeconds","forecastEnabled"})
            if (!w[retired].isNull()) return fail("Custom mapping, authentication and forecast are retired");
        const uint32_t interval = w["intervalSeconds"] | 1800U;
        if ((!w["intervalSeconds"].isNull() && !w["intervalSeconds"].is<uint32_t>()) ||
            (interval != 900 && interval != 1800 && interval != 3600) || !seconds(w["staleAfterSeconds"],300,604800))
            return fail("Weather interval is out of bounds");
        const char* key = w["apiKey"] | "";
        for (const char* p = key; *p; ++p) if (static_cast<unsigned char>(*p) < 32 || static_cast<unsigned char>(*p) == 127) return fail("Invalid credential");
        if ((w["enabled"] | false) && !*key) return fail("Seniverse API key is required");
    }
    if (measureJson(config) > MAX_BODY) return fail("Weather settings are too large");
    error.clear(); return true;
}

// Pure preparation shared by firmware and host tests; never changes the saved document.
inline bool prepareScreenWeather(JsonVariantConst submitted, JsonObjectConst current,
                                 JsonVariantConst legacy, bool migrationRequired,
                                 uint32_t revision, JsonObject next, std::string& error) {
    auto fail = [&](const char* message) { error=message; return false; };
    if (!submitted.is<JsonObjectConst>()) return fail("Invalid weather settings");
    if (!submitted["expectedRevision"].is<uint32_t>() || submitted["expectedRevision"].as<uint32_t>() != revision)
        return fail("Weather revision conflict; refresh configuration");
    for (JsonPairConst entry : submitted.as<JsonObjectConst>()) {
        const char* key=entry.key().c_str();
        bool allowed=false;
        for (const char* candidate : {"expectedRevision","enabled","location","intervalSeconds","apiKey","clearApiKey","legacySourceId"})
            if (!strcmp(key,candidate)) { allowed=true; break; }
        if (!allowed) return fail("Unsupported weather setting");
    }
    if (!submitted["enabled"].is<bool>() || !boundedText(submitted["location"],80,true) ||
        !boundedText(submitted["apiKey"],256) || !submitted["intervalSeconds"].is<uint32_t>() ||
        (!submitted["clearApiKey"].isNull() && !submitted["clearApiKey"].is<bool>())) return fail("Invalid weather settings");
    next["remoteChannels"].to<JsonArray>();
    JsonObject weather=next["weatherSources"].to<JsonArray>().add<JsonObject>();
    fixedWeather(weather,current);
    if (migrationRequired) {
        const char* selected=submitted["legacySourceId"] | "";
        if (!*selected) return fail("Choose an existing Seniverse source or create a new source");
        if (!strcmp(selected,"new")) fixedWeather(weather,JsonObjectConst());
        else {
            bool found=false;
            for (JsonObjectConst source : legacy["weatherSources"].as<JsonArrayConst>())
                if (!strcmp(selected,source["id"] | "") && source["provider"] == "seniverse") {
                    fixedWeather(weather,source); found=true; break;
                }
            if (!found) return fail("Selected weather source is unavailable");
        }
    }
    weather["enabled"]=submitted["enabled"]; weather["location"]=submitted["location"];
    weather["intervalSeconds"]=submitted["intervalSeconds"];
    weather["staleAfterSeconds"]=std::max<uint32_t>(3600,submitted["intervalSeconds"].as<uint32_t>()*2);
    const char* key=submitted["apiKey"] | "";
    if (*key) weather["apiKey"]=key;
    if (submitted["clearApiKey"] == true) weather["apiKey"]="";
    return validate(next,error);
}

inline void copyText(JsonObject out, const char* key, JsonVariantConst value, size_t max = 80) {
    if (!value.is<const char*>()) return;
    const char* s = value.as<const char*>();
    if (s && strlen(s) <= max) out[key] = s;
}

// Copy only the normalized fields that the weather UI is allowed to expose.
// Never pass provider responses or source configuration directly to a route.
inline void copyWeatherPreview(JsonVariantConst normalized, JsonObject out) {
    double value = 0;
    if (number(normalized["temperature"], value)) {
        out["temperature"] = value;
        out["unit"] = "°C";
    }
    copyText(out, "city", normalized["city"], 80);
    copyText(out, "description", normalized["description"], 80);
    const char* weatherCondition = normalized["condition"] | "";
    if (*weatherCondition) out["condition"] = conditionValid(weatherCondition) ? weatherCondition : "unknown";
    if (number(normalized["humidity"], value) && value >= 0 && value <= 100) out["humidity"] = value;

}

inline bool parseWeather(JsonObjectConst source, JsonVariantConst body, JsonObject out, std::string& error, bool forecast = false) {
    if (forecast || source["provider"] != "seniverse") { error="Only Seniverse current weather is supported"; return false; }
    if (!body.is<JsonObjectConst>() || !body["status_code"].isNull() || !body["error"].isNull()) { error="Provider returned an error"; return false; }
    JsonVariantConst root = body["results"][0];
    if (root.isNull()) { error="Missing weather result"; return false; }
    double temp=0;
    if (!number(root["now"]["temperature"],temp)) { error="Missing or invalid temperature"; return false; }
    out["temperature"]=temp; out["unit"]="°C";
    copyText(out,"city",root["location"]["name"]);
    copyText(out,"description",root["now"]["text"]);
    copyText(out,"providerUpdatedAt",root["last_update"],64);
    std::string code;
    JsonVariantConst raw = root["now"]["code"];
    if (raw.is<const char*>()) code = raw.as<const char*>();
    else if (raw.is<int>()) code = std::to_string(raw.as<int>());
    out["condition"]=condition(code.c_str());
    out["isNight"]=!strcmp(out["condition"] | "","clear-night") || code=="6" || code=="8";
    // Humidity is optional in paid responses; never manufacture it for the free tier.
    double humidity=0;
    if (number(root["now"]["humidity"],humidity) && humidity >= 0 && humidity <= 100) out["humidity"]=humidity;
    error.clear(); return true;
}
}} // namespace FastBee::RemoteResourceModel
