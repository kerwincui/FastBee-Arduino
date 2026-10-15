// SEC-P0-02 hardening (09-15 audit): server-side network field validation.
// Production function bodies are extracted from src/utils/NetworkUtils.cpp by
// the driving test (network_field_validators_production.inc) — no mirrored logic.
#include <Arduino.h>
#include <IPAddress.h>
#include <cassert>
#include <cstdio>
#include <string>

class NetworkUtils {
public:
    static bool isValidIP(const String& ip);
    static bool isValidSSID(const String& ssid);
    static bool isValidHostname(const String& hostname);
    static bool isValidOptionalIP(const String& ip);
};

#include "network_field_validators_production.inc"

static String repeated(char c, size_t count) { return String(std::string(count, c)); }

int main() {
    // isValidSSID：802.11 SSID 为 0-32 字节任意值；空值保留“自动生成/未配置”语义。
    assert(NetworkUtils::isValidSSID(""));
    assert(NetworkUtils::isValidSSID("fastbee-ap"));
    assert(NetworkUtils::isValidSSID("my wifi <home>"));
    assert(NetworkUtils::isValidSSID(repeated('x', 32)));
    assert(!NetworkUtils::isValidSSID(repeated('x', 33)));
    assert(!NetworkUtils::isValidSSID(repeated('y', 64)));

    // isValidHostname：空值回退默认域名；RFC 1123 标签（mDNS hostname）。
    assert(NetworkUtils::isValidHostname(""));
    assert(NetworkUtils::isValidHostname("fastbee"));
    assert(NetworkUtils::isValidHostname("fb-01-A"));
    assert(NetworkUtils::isValidHostname(repeated('a', 63)));
    assert(!NetworkUtils::isValidHostname(repeated('a', 64)));
    assert(!NetworkUtils::isValidHostname("-fastbee"));
    assert(!NetworkUtils::isValidHostname("fastbee-"));
    assert(!NetworkUtils::isValidHostname("fb.local"));
    assert(!NetworkUtils::isValidHostname("fb_01"));
    assert(!NetworkUtils::isValidHostname("<script>alert(1)</script>"));
    assert(!NetworkUtils::isValidHostname("fb host"));

    // isValidOptionalIP：空值保留 DHCP/默认语义；非空必须是合法 IPv4。
    assert(NetworkUtils::isValidOptionalIP(""));
    assert(NetworkUtils::isValidOptionalIP("192.168.4.1"));
    assert(NetworkUtils::isValidOptionalIP("8.8.8.8"));
    assert(NetworkUtils::isValidOptionalIP("255.255.255.0"));
    assert(!NetworkUtils::isValidOptionalIP("999.1.1.1"));
    assert(!NetworkUtils::isValidOptionalIP("1.2.3"));
    assert(!NetworkUtils::isValidOptionalIP("<img src=x>"));
    // 尾随垃圾（"1.2.3.4;x"）由真实 IPAddress::fromString（ESP32 严格解析）拒绝；
    // mock 的 sscanf 解析对后缀宽容，不在本夹具断言。

    std::puts("network field validation passed");
    return 0;
}
