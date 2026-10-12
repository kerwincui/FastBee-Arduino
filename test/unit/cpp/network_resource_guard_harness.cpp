#include <cassert>
#include <cstdlib>
#include <new>
#include "network/NetworkResourceConfig.h"
#include "core/PeripheralManager.h"
static bool failResultAllocation=false;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
 if(failResultAllocation && size==sizeof(FastBee::PreflightResult)) return nullptr;
 return (std::malloc)(size);
}
int main() {
 using namespace FastBee;
 WiFiConfig config; String error="stale";
 auto& manager=PeripheralManager::getInstance();
 assert(validateNetworkResourceConfig(config,error) && error.isEmpty() && manager.reads==0);
 config.networkType=NetworkType::NET_ETHERNET;
 config.ethernet={11,13,12,10,9,14};
 assert(validateNetworkResourceConfig(config,error));
 PeripheralConfig p; p.id="relay"; p.type=PeripheralType::GPIO_DIGITAL_OUTPUT;
 p.enabled=true; p.pinCount=1; p.pins[0]=9; manager.items={p};
 assert(!validateNetworkResourceConfig(config,error));
 assert(error=="NETWORK_RESOURCE_GPIO_CONFLICT: relay");
 manager.items[0].enabled=false;
 assert(validateNetworkResourceConfig(config,error) && error.isEmpty());
 failResultAllocation=true;
 assert(!validateNetworkResourceConfig(config,error));
 assert(error.startsWith("NETWORK_RESOURCE_MEMORY"));
 failResultAllocation=false;
 config.networkType=NetworkType::NET_4G; config.cellular={11,13,9};
 manager.items.clear();
 assert(validateNetworkResourceConfig(config,error) && error.isEmpty());
 p.pins[0]=9; manager.items={p};
 assert(!validateNetworkResourceConfig(config,error));
 assert(error=="NETWORK_RESOURCE_GPIO_CONFLICT: relay");
 manager.items[0].enabled=false;
 assert(validateNetworkResourceConfig(config,error));
 config.cellular.txPin=config.cellular.rxPin;
 assert(!validateNetworkResourceConfig(config,error));
 assert(error.startsWith("NETWORK_RESOURCE_PIN_DUPLICATED"));
 config.cellular.txPin=11;
 failResultAllocation=true;
 assert(!validateNetworkResourceConfig(config,error));
 assert(error.startsWith("NETWORK_RESOURCE_MEMORY"));
 failResultAllocation=false;
 config.networkType=NetworkType::NET_ETHERNET;
 manager.items.clear();
 for(unsigned i=0;i<FastBee::PREFLIGHT_MAX_CLAIMS+1;++i) {p.id=String(i);manager.items.push_back(p);}
 assert(!validateNetworkResourceConfig(config,error));
 assert(error.startsWith("NETWORK_RESOURCE_CAPACITY"));
}
