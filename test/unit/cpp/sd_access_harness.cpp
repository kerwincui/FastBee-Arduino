#include "core/SdAccess.h"
#include <cassert>
#include <string>
static SDCardStatus card;
static unsigned snapshots=0;
SDCardManager& SDCardManager::getInstance() { static SDCardManager manager; return manager; }
SDCardStatus SDCardManager::getStatus() const { assert(rtcLockDepth>0);++snapshots;return card; }
int main(int argc,char** argv) {
    assert(argc==2);const std::string mode(argv[1]);
    auto& pm=PeripheralManager::getInstance();
    card.statusAvailable=card.configured=card.mounted=true;card.ownerId="sd_01";
    card.sckPin=5;card.mosiPin=6;card.misoPin=7;card.csPin=8;card.frequency=1000000;
    const char* error=nullptr;
    if(mode=="busy"){rtcMutexBusy=true;error="SD_CONFIG_BUSY";}
    if(mode=="missing"){pm.found=false;error="SD_TARGET_INVALID";}
    if(mode=="wrong_type"){pm.config.type=PeripheralType::GPIO_DIGITAL_OUTPUT;error="SD_TARGET_INVALID";}
    if(mode=="changed"){++pm.version;error="SD_CONFIG_CHANGED";}
    if(mode=="disabled"){pm.config.enabled=false;error="SD_NOT_READY";}
    if(mode=="suspended"){pm.state.memorySuspended=true;error="SD_NOT_READY";}
    if(mode=="runtime_missing"){pm.stateFound=false;error="SD_NOT_READY";}
    if(mode=="snapshot_busy"){card.statusAvailable=false;error="SD_STATUS_UNAVAILABLE";}
    if(mode=="owner"){card.ownerId="sd_other";error="SD_OWNER_MISMATCH";}
    if(mode=="pins"){card.csPin=10;error="SD_CONFIG_CHANGED";}
    if(mode=="frequency"){card.frequency=2000000;error="SD_CONFIG_CHANGED";}
    if(mode=="unmounted")card.mounted=false;
    if(mode=="lost_bus" || mode=="lost_owner" || mode=="lost_pins") {
        card.statusAvailable=false;card.configurationAvailable=true;card.mounted=false;
        if(mode=="lost_owner"){card.ownerId="sd_other";error="SD_OWNER_MISMATCH";}
        if(mode=="lost_pins"){card.csPin=10;error="SD_CONFIG_CHANGED";}
    }
    {
        FastBee::SdAccess access("sd_01",7);
        assert(rtcLockTimeout==0);
        if(error){assert(!access.driver() && std::string(access.error())==error);}
        else {assert(access.driver() && rtcLockDepth==1 && access.status().mounted==card.mounted);}
        if(mode=="busy" || mode=="missing" || mode=="wrong_type" || mode=="changed" ||
           mode=="disabled" || mode=="suspended" || mode=="runtime_missing")assert(snapshots==0);
    }
    assert(rtcLockDepth==0);
}
