#pragma once
#include <Arduino.h>
enum class NetworkType : uint8_t { NET_WIFI=0, NET_ETHERNET=1, NET_4G=2 };
struct EthernetConfig {
 int8_t spiMosi=11, spiMiso=13, spiSck=12, csPin=47, rstPin=48, intPin=14;
};
struct CellularConfig { int8_t txPin=39, rxPin=40, pwrPin=38; };
struct WiFiConfig { NetworkType networkType=NetworkType::NET_WIFI; EthernetConfig ethernet; CellularConfig cellular; };
