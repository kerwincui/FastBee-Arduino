#include "network/EthernetAdapter.h"
#include "core/SpiBusInitializer.h"
#include "core/FeatureInstanceRegistry.h"
#include <freertos/semphr.h>
#include <cassert>
#include <cstring>
#include <ETH.h>
#include <WiFi.h>

int main(int argc, char** argv) {
    assert(argc == 2);
#if !FASTBEE_ENABLE_ETHERNET
    (void)argv; assert(SPI.begins == 0 && ETH.begins == 0); return 0;
#else
    const std::string test = argv[1];
    auto& bus = FastBee::SpiBusInitializer::instance();
    String error; WiFiConfig config;
    int peer = 0, outsider = 0;
#if !ETH_SPI_SUPPORTS_CUSTOM || CONFIG_DISABLE_HAL_LOCKS
    (void)test; (void)bus; (void)peer; (void)outsider;
    EthernetAdapter adapter;
    assert(!adapter.begin(config));
    assert(adapter.lastError().startsWith(CONFIG_DISABLE_HAL_LOCKS ? "ETH_LOCKS_DISABLED" : "ETH_CORE_UNSUPPORTED"));
    assert(!adapter.isConnected() && SPI.begins == 0 && ETH.begins == 0 && WiFi.registrations == 0);
    return 0;
#else
    if (test == "observation") {
        using namespace FastBee;
        auto& registry = FeatureInstanceRegistry::instance();
        assert(!registry.observe("ethernet").observed());
        auto* adapter = new EthernetAdapter;
        assert(registry.observe("ethernet").state == FeatureInstanceState::NotInitialized);
        assert(spiMutexCreates == 0 && ETH.begins == 0 && SPI.begins == 0);
        assert(adapter->begin(config));
        const int begins = ETH.begins, ends = ETH.ends;
        for (int i = 0; i < 1000; ++i) {
            assert(registry.observe("ethernet").initializedCount == 1);
            assert(spiMutexLastRequest == 0);
        }
        assert(ETH.begins == begins && ETH.ends == ends);
        spiMutexFailTake = true;
        assert(registry.observe("ethernet").state == FeatureInstanceState::Busy);
        spiMutexFailTake = false; ETH.stopFailure = 1;
        assert(!adapter->stop(error));
        assert(registry.observe("ethernet").state == FeatureInstanceState::CleanupPending);
        delete adapter;
        assert(registry.observe("ethernet").state == FeatureInstanceState::CleanupPending);
        ETH.stopFailure = 0; EthernetAdapter replacement;
        assert(replacement.begin(config) && replacement.stop(error));
        assert(registry.observe("ethernet").state == FeatureInstanceState::NotInitialized);
    } else if (test == "normal" || test == "peer-first" || test == "network-first") {
        const int sck = CHIP_ETHERNET_DEDICATED_SPI ? 18 : 12;
        const int miso = CHIP_ETHERNET_DEDICATED_SPI ? 19 : 13;
        const int mosi = CHIP_ETHERNET_DEDICATED_SPI ? 23 : 14;
        if (test == "peer-first") assert(bus.acquire(&peer,sck,miso,mosi,5,error));
        EthernetAdapter adapter;
        assert(adapter.begin(config));
        assert(ETH.lastSpi != nullptr && ETH.lastSpi->active);
        assert((ETH.lastSpi != &SPI) == bool(CHIP_ETHERNET_DEDICATED_SPI));
        assert(adapter.begin(config) && ETH.begins == 1 && WiFi.registrations == 1);
        if (test != "peer-first") assert(bus.acquire(&peer,sck,miso,mosi,5,error));
        assert(!bus.acquire(&outsider,sck,miso,mosi,config.ethernet.csPin,error));
        assert(!bus.acquire(&outsider,sck,miso,mosi,config.ethernet.rstPin,error));
        assert(!bus.acquire(&outsider,sck,miso,mosi,config.ethernet.intPin,error));
        ETH.link=true; ETH.address=true; assert(adapter.isConnected());
        assert(adapter.localIP().toString() == "192.0.2.2");
        { EthernetAdapter other; assert(!other.begin(config)); assert(other.lastError().startsWith("ETH_IN_USE")); }
        assert(adapter.isConnected());
        assert(bus.release(&peer,error));
        if (CHIP_ETHERNET_DEDICATED_SPI) {
            assert(bus.end(error) && ETH.lastSpi->active);
        } else {
            assert(!bus.end(error) && error.startsWith("SPI_IN_USE"));
        }
        assert(bus.acquire(&peer,sck,miso,mosi,5,error));
        const int globalEnds = SPI.ends;
        assert(adapter.stop(error)); assert(SPI.ends == globalEnds && bus.isAcquired(&peer));
        assert(WiFi.callbacks.empty() && !ETH.handle() && !ETH.netif());
        WiFi.deliverQueued();
        assert(!adapter.isConnected() && adapter.macAddress().isEmpty());
        assert(bus.release(&peer,error) && bus.end(error));
    } else if (test == "pins") {
        for (int variant=0; variant<4; ++variant) {
            WiFiConfig bad=config;
            if (variant==0) bad.ethernet.csPin=bad.ethernet.spiMiso;
            if (variant==1) bad.ethernet.rstPin=30;
            if (variant==2) bad.ethernet.spiMosi=35;
            if (variant==3) bad.ethernet.spiMiso=-1;
            EthernetAdapter adapter; assert(!adapter.begin(bad));
        }
        assert(ETH.begins == 0 && SPI.begins == 0 && WiFi.registrations == 0);
    } else if (test == "conflict-first") {
        assert(bus.acquire(&peer,18,19,23,config.ethernet.rstPin,error));
        EthernetAdapter adapter; assert(!adapter.begin(config));
        assert(bus.isAcquired(&peer) && ETH.begins==0 && WiFi.registrations==0 && SPI.ends==0);
    } else if (test == "failed-begin") {
        for (int stage=1; stage<=3; ++stage) {
            ETH.failureStage=stage;
            EthernetAdapter adapter; assert(!adapter.begin(config));
            assert(adapter.lastError().startsWith("ETH_BEGIN_FAILED"));
            assert(!ETH.handle() && !ETH.netif() && WiFi.callbacks.empty());
            assert(!bus.isChipSelectAcquired(config.ethernet.csPin));
        }
        ETH.failureStage=0; EthernetAdapter adapter; assert(adapter.begin(config));
        assert(adapter.stop(error));
    } else if (test == "failed-stop" || test == "destructor") {
        auto* adapter=new EthernetAdapter(); assert(adapter->begin(config));
        ETH.stopFailure=1; const int beforeEnds=SPI.ends, beforeDeletes=spiObjectDestroys;
        assert(!adapter->stop(error) && error.startsWith("ETH_STOP_PENDING"));
        assert(bus.isChipSelectAcquired(config.ethernet.csPin));
        assert(SPI.ends==beforeEnds && spiObjectDestroys==beforeDeletes);
        if (test == "destructor") {
            delete adapter; adapter=nullptr; WiFi.deliverQueued();
            EthernetAdapter replacement; assert(!replacement.begin(config));
            assert(replacement.lastError().startsWith("ETH_STOP_PENDING"));
            ETH.stopFailure=0; assert(replacement.begin(config)); assert(replacement.stop(error));
        } else {
            ETH.stopFailure=2; assert(!adapter->stop(error));
            assert(bus.isChipSelectAcquired(config.ethernet.csPin) && spiObjectDestroys==beforeDeletes);
            ETH.stopFailure=0; assert(adapter->stop(error)); delete adapter;
        }
        assert(!bus.isChipSelectAcquired(config.ethernet.csPin));
    } else if (test == "busy") {
        EthernetAdapter adapter; spiMutexFailTake=true;
        assert(!adapter.begin(config) && ETH.begins==0 && SPI.begins==0);
        spiMutexFailTake=false; assert(adapter.begin(config));
        spiMutexFailTake=true; assert(!adapter.stop(error));
        assert(ETH.ends==0 && !adapter.isConnected());
        spiMutexFailTake=false; assert(adapter.stop(error));
    } else if (test == "config") {
        EthernetAdapter adapter; assert(adapter.begin(config));
        WiFiConfig changed=config; changed.ethernet.csPin=6;
        assert(!adapter.begin(changed) && ETH.begins==1);
        changed=config; changed.ipConfigType=IPConfigType::STATIC;
        changed.staticIP="192.0.2.2"; changed.gateway="192.0.2.1"; changed.subnet="255.255.255.0";
        assert(!adapter.begin(changed) && adapter.lastError().startsWith("ETH_CONFIG_CHANGED"));
        assert(adapter.stop(error)); ETH.failConfig=true;
        assert(!adapter.begin(changed) && adapter.lastError().startsWith("ETH_IP_CONFIG_FAILED"));
        assert(!ETH.handle() && !ETH.netif() && WiFi.callbacks.empty());
        ETH.failConfig=false; assert(adapter.begin(changed) && ETH.configs==2);
        assert(adapter.stop(error)); changed.staticIP="invalid";
        const int before=ETH.begins; assert(!adapter.begin(changed) && ETH.begins==before);
    } else if (test == "unmanaged") {
        ETH.driver=true; EthernetAdapter adapter; assert(!adapter.begin(config));
        assert(ETH.ends==0 && SPI.begins==0 && ETH.driver);
        ETH.driver=false; SPI.begin(18,19,23,5);
        assert(!adapter.begin(config) && SPI.active && SPI.ends==0);
    } else if (test == "events") {
        WiFi.failRegister=true; EthernetAdapter adapter; assert(!adapter.begin(config));
        assert(ETH.begins==0 && !bus.isChipSelectAcquired(config.ethernet.csPin));
        WiFi.failRegister=false; assert(adapter.begin(config)); assert(adapter.stop(error));
        WiFi.deliverQueued(); assert(!adapter.isConnected());
    } else if (test == "spi-failure") {
        spiFailNextBegin=true; EthernetAdapter adapter; assert(!adapter.begin(config));
        assert(ETH.begins==0 && WiFi.registrations==0);
        assert(!bus.isChipSelectAcquired(config.ethernet.csPin));
        assert(adapter.begin(config)); assert(adapter.stop(error));
    } else if (test == "lease-contract") {
        assert(!bus.acquireNetwork(nullptr,false,12,13,14,15,16,17,error));
        assert(bus.acquireNetwork(&peer,CHIP_ETHERNET_DEDICATED_SPI,12,13,14,15,16,17,error));
        assert(bus.acquireNetwork(&peer,CHIP_ETHERNET_DEDICATED_SPI,12,13,14,15,16,17,error));
        assert(!bus.acquireNetwork(&outsider,CHIP_ETHERNET_DEDICATED_SPI,12,13,14,15,16,17,error));
        assert(!bus.acquireNetwork(&peer,CHIP_ETHERNET_DEDICATED_SPI,12,13,14,6,16,17,error));
        assert(!bus.releaseNetwork(&outsider,error));
        assert(bus.isChipSelectAcquired(15) && bus.isChipSelectAcquired(16) && bus.isChipSelectAcquired(17));
        if (CHIP_ETHERNET_DEDICATED_SPI) {
            assert(!bus.acquire(&outsider,12,19,23,5,error));
            assert(!bus.acquire(&outsider,18,13,23,5,error));
            assert(!bus.acquire(&outsider,18,19,14,5,error));
        } else {
            assert(!bus.acquire(&outsider,13,12,14,5,error));
            SPI.active=false;
            assert(!bus.acquireNetwork(&peer,false,12,13,14,15,16,17,error));
            assert(error.startsWith("SPI_BUS_LOST"));
        }
        spiMutexFailTake=true; assert(!bus.releaseNetwork(&peer,error));
        spiMutexFailTake=false; assert(bus.releaseNetwork(&peer,error));
        assert(!bus.isChipSelectAcquired(15));
    } else if (test == "cycles") {
        for (int i=0;i<1000;++i) {
            EthernetAdapter adapter; assert(adapter.begin(config)); assert(adapter.stop(error));
            assert(WiFi.callbacks.empty()); WiFi.queued.clear();
        }
        assert(spiObjectConstructs-spiObjectDestroys==1);
        assert(spiMutexCreates-spiMutexDeletes==1);
        assert(ETH.begins==1000 && ETH.ends==1000);
    } else { assert(false); }
    return 0;
#endif
#endif
}
