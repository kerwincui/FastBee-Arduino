/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:33:18
 */

#include "utils/StringUtils.h"
#include <Arduino.h>
#include <sstream>
#include <MD5Builder.h>

std::vector<String> StringUtils::split(const String& str, char delimiter) {
    std::vector<String> result;
    if (str.isEmpty()) return result;
    
    int start = 0;
    int end = str.indexOf(delimiter);
    
    while (end != -1) {
        result.push_back(str.substring(start, end));
        start = end + 1;
        end = str.indexOf(delimiter, start);
    }
    
    result.push_back(str.substring(start));
    return result;
}

std::vector<String> StringUtils::split(const String& str, const String& delimiter) {
    std::vector<String> result;
    if (str.isEmpty() || delimiter.isEmpty()) {
        result.push_back(str);
        return result;
    }
    
    int start = 0;
    int end = str.indexOf(delimiter);
    
    while (end != -1) {
        result.push_back(str.substring(start, end));
        start = end + delimiter.length();
        end = str.indexOf(delimiter, start);
    }
    
    result.push_back(str.substring(start));
    return result;
}

String StringUtils::join(const std::vector<String>& strings, const String& delimiter) {
    // 预计算总长度以减少内存重分配
    size_t totalLen = 0;
    for (size_t i = 0; i < strings.size(); i++) {
        totalLen += strings[i].length();
        if (i > 0) totalLen += delimiter.length();
    }
    String result;
    if (totalLen > 0) result.reserve(totalLen);
    
    for (size_t i = 0; i < strings.size(); i++) {
        if (i > 0) {
            result += delimiter;
        }
        result += strings[i];
    }
    return result;
}

String StringUtils::trim(const String& str) {
    return trimRight(trimLeft(str));
}

String StringUtils::trimLeft(const String& str) {
    if (str.isEmpty()) return str;
    
    int start = 0;
    while (start < str.length() && isspace(str[start])) {
        start++;
    }
    
    return str.substring(start);
}

String StringUtils::trimRight(const String& str) {
    if (str.isEmpty()) return str;
    
    int end = str.length() - 1;
    while (end >= 0 && isspace(str[end])) {
        end--;
    }
    
    return str.substring(0, end + 1);
}

String StringUtils::toLower(const String& str) {
    String result = str;
    for (size_t i = 0; i < result.length(); i++) {
        result[i] = tolower(result[i]);
    }
    return result;
}

String StringUtils::toUpper(const String& str) {
    String result = str;
    for (size_t i = 0; i < result.length(); i++) {
        result[i] = toupper(result[i]);
    }
    return result;
}

bool StringUtils::startsWith(const String& str, const String& prefix, bool caseSensitive) {
    if (prefix.length() > str.length()) return false;
    
    if (caseSensitive) {
        return str.substring(0, prefix.length()) == prefix;
    } else {
        return toLower(str.substring(0, prefix.length())) == toLower(prefix);
    }
}

bool StringUtils::endsWith(const String& str, const String& suffix, bool caseSensitive) {
    if (suffix.length() > str.length()) return false;
    
    if (caseSensitive) {
        return str.substring(str.length() - suffix.length()) == suffix;
    } else {
        return toLower(str.substring(str.length() - suffix.length())) == toLower(suffix);
    }
}

bool StringUtils::contains(const String& str, const String& substring, bool caseSensitive) {
    if (caseSensitive) {
        return str.indexOf(substring) != -1;
    } else {
        return toLower(str).indexOf(toLower(substring)) != -1;
    }
}

String StringUtils::replace(const String& str, const String& from, const String& to, bool caseSensitive) {
    if (from.isEmpty()) return str;
    int pos = caseSensitive ? str.indexOf(from) : toLower(str).indexOf(toLower(from));
    if (pos == -1) return str;
    return str.substring(0, pos) + to + str.substring(pos + from.length());
}

String StringUtils::replaceAll(const String& str, const String& from, const String& to, bool caseSensitive) {
    if (from.isEmpty()) return str;
    String result = str;
    String searchFrom = caseSensitive ? from : toLower(from);
    int pos = 0;
    while (true) {
        String searchIn = caseSensitive ? result : toLower(result);
        int found = searchIn.indexOf(searchFrom, pos);
        if (found == -1) break;
        result = result.substring(0, found) + to + result.substring(found + from.length());
        pos = found + to.length();
    }
    return result;
}

String StringUtils::format(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return String(buffer);
}

String StringUtils::urlEncode(const String& str) {
    String encoded;
    // 最坏情况: 每个字符变成 3 个字符 (%XX)
    encoded.reserve(str.length() * 3);
    const char* hex = "0123456789ABCDEF";
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            encoded += '%';
            encoded += hex[(c >> 4) & 0x0F];
            encoded += hex[c & 0x0F];
        }
    }
    
    return encoded;
}

String StringUtils::urlDecode(const String& str) {
    String decoded;
    decoded.reserve(str.length());  // 解码后长度不超过原长度
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (c == '%' && i + 2 < str.length()) {
            char hex[3] = {str[i + 1], str[i + 2], '\0'};
            decoded += (char)strtoul(hex, nullptr, 16);
            i += 2;
        } else if (c == '+') {
            decoded += ' ';
        } else {
            decoded += c;
        }
    }
    
    return decoded;
}

String StringUtils::htmlEncode(const String& str) {
    String encoded;
    // 最坏情况: 每个字符变成 6 个字符 (&quot;)
    encoded.reserve(str.length() * 6);
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        switch (c) {
            case '&': encoded += "&amp;"; break;
            case '<': encoded += "&lt;"; break;
            case '>': encoded += "&gt;"; break;
            case '"': encoded += "&quot;"; break;
            case '\'': encoded += "&#39;"; break;
            default: encoded += c; break;
        }
    }
    
    return encoded;
}

String StringUtils::htmlDecode(const String& str) {
    String decoded = str;
    decoded.replace("&amp;", "&");
    decoded.replace("&lt;", "<");
    decoded.replace("&gt;", ">");
    decoded.replace("&quot;", "\"");
    decoded.replace("&#39;", "'");
    return decoded;
}

String StringUtils::jsonEscape(const String& str) {
    String escaped;
    // 最坏情况: 每个字符变成 6 个字符 (\uXXXX)
    escaped.reserve(str.length() * 6);
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c >= 0 && c <= 0x1F) {
                    // 控制字符，使用Unicode转义
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    escaped += buffer;
                } else {
                    escaped += c;
                }
                break;
        }
    }
    
    return escaped;
}

bool StringUtils::isEmpty(const String& str) {
    return str.isEmpty() || trim(str).isEmpty();
}

bool StringUtils::isNumeric(const String& str) {
    if (str.isEmpty()) return false;
    
    bool hasDecimal = false;
    bool hasDigit = false;
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (isdigit(c)) {
            hasDigit = true;
        } else if (c == '.' && !hasDecimal) {
            hasDecimal = true;
        } else if (i == 0 && (c == '-' || c == '+')) {
            // 允许正负号开头
        } else {
            return false;
        }
    }
    
    return hasDigit;
}

bool StringUtils::isInteger(const String& str) {
    if (str.isEmpty()) return false;
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (!isdigit(c) && !(i == 0 && (c == '-' || c == '+'))) {
            return false;
        }
    }
    
    return true;
}

bool StringUtils::isFloat(const String& str) {
    if (str.isEmpty()) return false;
    
    bool hasDecimal = false;
    bool hasDigit = false;
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (isdigit(c)) {
            hasDigit = true;
        } else if (c == '.' && !hasDecimal) {
            hasDecimal = true;
        } else if (i == 0 && (c == '-' || c == '+')) {
            // 允许正负号开头
        } else {
            return false;
        }
    }
    
    return hasDigit && hasDecimal;
}

int StringUtils::toInt(const String& str, int defaultValue) {
    if (!isInteger(str)) return defaultValue;
    return str.toInt();
}

float StringUtils::toFloat(const String& str, float defaultValue) {
    if (!isNumeric(str)) return defaultValue;
    return str.toFloat();
}

bool StringUtils::toBool(const String& str, bool defaultValue) {
    String lowerStr = toLower(trim(str));
    
    if (lowerStr == "true" || lowerStr == "1" || lowerStr == "yes" || lowerStr == "on") {
        return true;
    } else if (lowerStr == "false" || lowerStr == "0" || lowerStr == "no" || lowerStr == "off") {
        return false;
    }
    
    return defaultValue;
}

String StringUtils::pad(const String& str, size_t length, char padChar, bool left) {
    if (str.length() >= length) return str;
    
    String padding = repeat(String(padChar), length - str.length());
    return left ? padding + str : str + padding;
}

String StringUtils::repeat(const String& str, size_t times) {
    String result;
    size_t totalLen = str.length() * times;
    if (totalLen > 0) result.reserve(totalLen);
    for (size_t i = 0; i < times; i++) {
        result += str;
    }
    return result;
}

String StringUtils::reverse(const String& str) {
    String result = str;
    size_t len = result.length();
    for (size_t i = 0; i < len / 2; i++) {
        char temp = result[i];
        result[i] = result[len - 1 - i];
        result[len - 1 - i] = temp;
    }
    return result;
}

String StringUtils::substring(const String& str, int start, int length) {
    if (start < 0) start = str.length() + start;
    if (length <= 0) length = str.length() - start;
    
    return str.substring(start, start + length);
}

String StringUtils::md5(const String& str) {
    MD5Builder md5;
    md5.begin();
    md5.add(str);
    md5.calculate();
    return md5.toString();
}

String StringUtils::sha256(const String& str) {
    // 仅用于快速哈希，非密码学安全；安全场景请使用 CryptoUtils::hash()
    uint32_t h = 0;
    for (size_t i = 0; i < str.length(); i++) {
        h = (h * 31) + str[i];
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016lx", (unsigned long)h);
    return String(buf);
}

String StringUtils::base64Encode(const String& str) {
    // 简化实现
    const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = str.length();
    // Base64 输出长度 = (inputLen + 2) / 3 * 4
    String result;
    result.reserve((len + 2) / 3 * 4 + 1);
    
    size_t i = 0;
    
    while (i < len) {
        uint32_t octet_a = i < len ? (unsigned char)str[i++] : 0;
        uint32_t octet_b = i < len ? (unsigned char)str[i++] : 0;
        uint32_t octet_c = i < len ? (unsigned char)str[i++] : 0;
        
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        
        result += base64_chars[(triple >> 3 * 6) & 0x3F];
        result += base64_chars[(triple >> 2 * 6) & 0x3F];
        result += base64_chars[(triple >> 1 * 6) & 0x3F];
        result += base64_chars[(triple >> 0 * 6) & 0x3F];
    }
    
    // 添加填充
    switch (len % 3) {
        case 1: result.setCharAt(result.length() - 2, '='); // 继续到下一个case
        case 2: result.setCharAt(result.length() - 1, '='); break;
    }
    
    return result;
}

String StringUtils::base64Decode(const String& str) {
    // 仅为接口占位，完整实现请使用 CryptoUtils::base64Decode()
    return "";
}

String StringUtils::randomString(size_t length, const String& charset) {
    String result;
    result.reserve(length);
    size_t charsetLen = charset.length();
    
    for (size_t i = 0; i < length; i++) {
        result += charset[random(0, charsetLen)];
    }
    
    return result;
}

int StringUtils::compareIgnoreCase(const String& str1, const String& str2) {
    return toLower(str1).compareTo(toLower(str2));
}

String StringUtils::removeWhitespace(const String& str) {
    String result;
    result.reserve(str.length());  // 最多等于原长度
    for (size_t i = 0; i < str.length(); i++) {
        if (!isspace(str[i])) {
            result += str[i];
        }
    }
    return result;
}

size_t StringUtils::charCount(const String& str) {
    // 简化实现，假设都是单字节字符
    return str.length();
}

size_t StringUtils::byteCount(const String& str) {
    return str.length();
}

String StringUtils::toCamelCase(const String& str, bool capitalizeFirst) {
    std::vector<String> parts = split(str, " _");
    String result;
    result.reserve(str.length());  // 结果不会超过原长度
    
    for (size_t i = 0; i < parts.size(); i++) {
        String part = trim(parts[i]);
        if (part.isEmpty()) continue;
        
        if (i == 0 && !capitalizeFirst) {
            result += toLower(part);
        } else {
            result += (char)toupper(part[0]);
            if (part.length() > 1) {
                result += toLower(part.substring(1));
            }
        }
    }
    
    return result;
}

String StringUtils::toSnakeCase(const String& str) {
    String result;
    // 最坏情况: 每个大写字母前加下划线，结果长度翻倍
    result.reserve(str.length() * 2);
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (isupper(c)) {
            if (i > 0) result += '_';
            result += (char)tolower(c);
        } else {
            result += c;
        }
    }
    
    return result;
}


String StringUtils::buildJsonResponse(int status, const String& msg, const String& data) {
    // 对msg中的JSON特殊字符进行转义
    String escapedMsg = msg;
    escapedMsg.replace("\\", "\\\\"); // 反斜杠
    escapedMsg.replace("\"", "\\\""); // 双引号
    // 可选：替换其他控制字符，如换行符
    escapedMsg.replace("\n", "\\n");
    escapedMsg.replace("\r", "\\r");
    
    String json = "{";
    json += "\"status\":" + String(status) + ",";
    json += "\"msg\":\"" + escapedMsg + "\",";
    json += "\"data\":" + data;
    json += "}";
    return json;
}