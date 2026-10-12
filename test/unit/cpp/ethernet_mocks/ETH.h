#pragma once
#include <SPI.h>
#include <IPAddress.h>
#include <functional>
#ifndef ETH_SPI_SUPPORTS_CUSTOM
#define ETH_SPI_SUPPORTS_CUSTOM 1
#endif
constexpr int ETH_PHY_W5500 = 1;
struct MockEthernet {
    int failureStage=0, stopFailure=0, begins=0, ends=0, configs=0;
    bool driver=false, network=false, failConfig=false, link=false, address=false;
    SPIClass* lastSpi=nullptr;
    std::function<void()> onBegin, onEnd;
    bool begin(int, int, int, int, int, SPIClass& spi) {
        ++begins; lastSpi=&spi;
        if (onBegin) onBegin();
        if (failureStage == 1) return false;
        driver=true;
        if (failureStage == 2) return false;
        network=true;
        return failureStage != 3;
    }
    void end() {
        ++ends; if (onEnd) onEnd();
        if (stopFailure == 1) return;
        driver=false;
        if (stopFailure == 2) return;
        network=false; link=false; address=false;
    }
    void* handle() const { return driver ? reinterpret_cast<void*>(1) : nullptr; }
    void* netif() const { return network ? reinterpret_cast<void*>(2) : nullptr; }
    bool config(IPAddress, IPAddress, IPAddress, IPAddress, IPAddress) { ++configs; return !failConfig; }
    bool linkUp() const { return link; }
    bool hasIP() const { return address; }
    IPAddress localIP() const { IPAddress value; value.fromString("192.0.2.2"); return value; }
    String macAddress() const { return "02:00:00:00:00:01"; }
};
inline MockEthernet ETH;
