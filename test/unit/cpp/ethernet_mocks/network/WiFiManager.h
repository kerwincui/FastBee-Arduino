#pragma once
#include <Arduino.h>
#include <WiFi.h>
enum class IPConfigType { DHCP, STATIC };
struct EthernetConfig {
    int8_t spiSck=12, spiMiso=13, spiMosi=14, csPin=15, rstPin=16, intPin=17;
};
struct WiFiConfig {
    EthernetConfig ethernet;
    IPConfigType ipConfigType=IPConfigType::DHCP;
    String staticIP, gateway, subnet, dns1, dns2;
};
