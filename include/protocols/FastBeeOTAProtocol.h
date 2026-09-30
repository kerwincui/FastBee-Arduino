#ifndef FASTBEE_OTA_PROTOCOL_H
#define FASTBEE_OTA_PROTOCOL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>

namespace FastBeeOtaProtocol {

constexpr uint8_t STATUS_UPGRADING = 2;
constexpr uint8_t STATUS_SUCCESS = 3;
constexpr uint8_t STATUS_FAILED = 4;

struct Command {
    int32_t taskId = 0;
    String url;
    String version;
};

inline bool hasUnsafeUrlCharacter(const String& value) {
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(value.charAt(i));
        if (c <= 0x20U || c == 0x7FU || c == '\\') return true;
    }
    return false;
}

inline bool isHttpsUrl(const String& value) {
    return value.startsWith("https://");
}

inline bool isValidFirmwareUrl(const String& value) {
    const size_t authorityStart = isHttpsUrl(value) ? 8U :
        (value.startsWith("http://") ? 7U : 0U);
    if (!authorityStart || value.length() <= authorityStart || value.length() > 768U ||
        hasUnsafeUrlCharacter(value) || value.indexOf('#') >= 0) {
        return false;
    }
    size_t authorityEnd = value.length();
    for (size_t i = authorityStart; i < value.length(); ++i) {
        const char c = value.charAt(i);
        if (c == '/' || c == '?' || c == '#') {
            authorityEnd = i;
            break;
        }
    }
    const String authority = value.substring(authorityStart, authorityEnd);
    if (authority.isEmpty() || authority.indexOf('@') >= 0) return false;
    String host = authority;
    String port;
    if (authority.startsWith("[")) {
        const int close = authority.indexOf(']');
        if (close <= 1 || authority.indexOf(':') < 0) return false;
        host = authority.substring(1, close);
        const String suffix = authority.substring(close + 1);
        if (!suffix.isEmpty()) {
            if (!suffix.startsWith(":") || suffix.length() == 1U) return false;
            port = suffix.substring(1);
        }
    } else {
        const int colon = authority.indexOf(':');
        if (colon >= 0) {
            host = authority.substring(0, colon);
            port = authority.substring(colon + 1);
            if (port.isEmpty()) return false;
        }
        if (host.isEmpty()) return false;
        for (size_t i = 0; i < host.length(); ++i) {
            const char c = host.charAt(i);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        }
    }
    if (!port.isEmpty()) {
        uint32_t number = 0;
        for (size_t i = 0; i < port.length(); ++i) {
            const char c = port.charAt(i);
            if (c < '0' || c > '9') return false;
            number = number * 10U + static_cast<uint32_t>(c - '0');
            if (number > 65535U) return false;
        }
        if (!number) return false;
    }
    return true;
}

inline bool isValidHttpsUrl(const String& value) {
    return isHttpsUrl(value) && isValidFirmwareUrl(value);
}

inline bool validateBaseUrl(const String& value, String& error) {
    error = "";
    if (value.isEmpty()) return true;
    if (value.length() > 512U || !isValidFirmwareUrl(value) ||
        value.indexOf('?') >= 0 || value.indexOf('#') >= 0) {
        error = "OTA base URL must use http:// or https:// with a valid host, no credentials, query or fragment (max 512 characters)";
        return false;
    }
    return true;
}

inline bool isInteger(JsonVariantConst value) {
    return value.is<long long>() || value.is<unsigned long long>() ||
           value.is<long>() || value.is<unsigned long>() ||
           value.is<int>() || value.is<unsigned int>();
}

inline bool isNumber(JsonVariantConst value) {
    return isInteger(value) || value.is<float>() || value.is<double>();
}

inline String scalarToString(JsonVariantConst value) {
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        return String(text ? text : "");
    }

    if (!isNumber(value)) return String();

    char buffer[32] = {0};
    size_t written = serializeJson(value, buffer, sizeof(buffer));
    if (written == 0 || written >= sizeof(buffer)) return String();
    buffer[written] = '\0';
    return String(buffer);
}

inline bool parseCommand(const String& payload, Command& command, String& error) {
    command = Command();
    error = "";

    if (payload.isEmpty() || payload.length() > 768U) {
        error = "Invalid OTA command size";
        return false;
    }

    JsonDocument doc;
    DeserializationError jsonError =
        deserializeJson(doc, payload.c_str(), payload.length());
    if (jsonError || !doc.is<JsonObject>()) {
        error = "OTA command must be a JSON object";
        return false;
    }

    JsonObjectConst object = doc.as<JsonObjectConst>();
    JsonVariantConst taskValue = object["taskId"];
    if (!isInteger(taskValue)) {
        error = "OTA taskId must be an integer";
        return false;
    }
    long long taskId = taskValue.as<long long>();
    if (taskId <= 0 || taskId > INT32_MAX) {
        error = "OTA taskId is out of range";
        return false;
    }

    JsonVariantConst statusValue = object["status"];
    if (!isInteger(statusValue) || statusValue.as<int>() != 1) {
        error = "OTA command status must be 1";
        return false;
    }

    if (!object["url"].is<const char*>()) {
        error = "OTA URL is missing";
        return false;
    }
    String url = object["url"].as<const char*>();
    url.trim();
    if (url.isEmpty() || url.length() > 640U ||
        url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0) {
        error = "OTA URL is invalid";
        return false;
    }

    JsonVariantConst versionValue = object["version"];
    String version = scalarToString(versionValue);
    version.trim();
    if (version.isEmpty() || version.length() > 31U) {
        error = "OTA version is missing or invalid";
        return false;
    }

    command.taskId = static_cast<int32_t>(taskId);
    command.url = url;
    command.version = version;
    return true;
}

inline bool resolveFirmwareUrl(const String& requestedUrl,
                               const String& configuredBaseUrl,
                               const String& mqttHost,
                               String& resolvedUrl,
                               String& error) {
    resolvedUrl = "";
    error = "";

    String path = requestedUrl;
    path.trim();
    if (path.startsWith("https://") || path.startsWith("http://")) {
        if (!isValidFirmwareUrl(path)) {
            error = "OTA HTTP/HTTPS URL is invalid";
            return false;
        }
        resolvedUrl = path;
        return true;
    }
    if (path.isEmpty() || path.startsWith("//") || path.indexOf("://") >= 0 ||
        path.indexOf('#') >= 0 || hasUnsafeUrlCharacter(path)) {
        error = "OTA URL must be HTTP/HTTPS or a relative path";
        return false;
    }

    String base = configuredBaseUrl;
    base.trim();
    if (base.isEmpty()) {
        String host = mqttHost;
        host.trim();
        if (host.startsWith("mqtts://")) {
            host = host.substring(8);
        } else if (host.startsWith("mqtt://")) {
            host = host.substring(7);
        }
        int slash = host.indexOf('/');
        if (slash >= 0) host = host.substring(0, slash);
        if (host.startsWith("[")) {
            const int closingBracket = host.indexOf(']');
            if (closingBracket > 0) host = host.substring(0, closingBracket + 1);
        } else {
            int portSeparator = -1;
            for (size_t i = 0; i < host.length(); ++i) {
                if (host.charAt(i) == ':') portSeparator = static_cast<int>(i);
            }
            if (portSeparator > 0) host = host.substring(0, portSeparator);
        }
        if (host.isEmpty() || host.indexOf('@') >= 0 ||
            hasUnsafeUrlCharacter(host)) {
            error = "OTA HTTPS base URL is not configured";
            return false;
        }
        base = "https://" + host;
    }

    if (!validateBaseUrl(base, error)) {
        return false;
    }
    while (base.endsWith("/") && base.length() > 8U) {
        base = base.substring(0, base.length() - 1);
    }
    if (!path.startsWith("/")) path = "/" + path;

    resolvedUrl = base + path;
    if (resolvedUrl.length() > 768U) {
        resolvedUrl = "";
        error = "Resolved OTA URL is too long";
        return false;
    }
    return true;
}

inline String buildReply(int32_t taskId,
                         const String& version,
                         uint8_t status,
                         uint8_t progress = 0) {
    JsonDocument doc;
    doc["taskId"] = taskId;
    if (status == STATUS_UPGRADING) {
        doc["progress"] = progress;
    }
    doc["version"] = version;
    doc["status"] = status;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

}  // namespace FastBeeOtaProtocol

#endif
