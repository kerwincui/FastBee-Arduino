#include "core/PeriphExecExecutor.h"
#include "core/SdStatusAction.h"
#include "core/PeripheralManager.h"
#include "peripherals/SDCardManager.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cstring>

PeriphExecExecutor::PeriphExecExecutor()=default;
PeriphExecExecutor::~PeriphExecExecutor()=default;
static SDCardStatus card;
static unsigned snapshots=0;
SDCardManager& SDCardManager::getInstance() { static SDCardManager manager; return manager; }
SDCardStatus SDCardManager::getStatus() const { assert(rtcLockDepth>0); ++snapshots; return card; }

int main(int argc, char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    ExecAction action; action.actionType=38; action.targetPeriphId="sd_01"; action.useReceivedValue=false;
    card.statusAvailable=true; card.configured=true; card.mounted=true; card.capacityValid=true;
    card.ownerId="sd_01"; card.sckPin=5; card.mosiPin=6; card.misoPin=7; card.csPin=8;
    card.frequency=1000000; card.totalBytes=5000000000ULL; card.usedBytes=1234567890ULL;
    card.capacityAgeMs=70000; card.capacitySampledAtMs=100;
    auto& manager=PeripheralManager::getInstance();
    const char* error=nullptr;
    if(mode=="busy") {rtcMutexBusy=true;error="SD_STATUS_BUSY";}
    if(mode=="missing") {manager.found=false;error="SD_STATUS_TARGET_INVALID";}
    if(mode=="wrong_type") {manager.config.type=PeripheralType::GPIO_DIGITAL_OUTPUT;error="SD_STATUS_TARGET_INVALID";}
    if(mode=="disabled") {manager.config.enabled=false;error="SD_STATUS_NOT_READY";}
    if(mode=="suspended") {manager.state.memorySuspended=true;error="SD_STATUS_NOT_READY";}
    if(mode=="unavailable") {card.statusAvailable=false;error="SD_STATUS_UNAVAILABLE";}
    if(mode=="owner") {card.ownerId="another";error="SD_STATUS_OWNER_MISMATCH";}
    if(mode=="pins") {manager.config.pins[0]=9;error="SD_STATUS_CONFIG_CHANGED";}
    if(mode=="frequency") {manager.config.params.sdcard.frequency=2000000;error="SD_STATUS_CONFIG_CHANGED";}
    if(mode=="value") {action.actionValue="mount";error="SD_STATUS_ACTION_INVALID: select an SD target and no value";}
    if(mode=="received") {action.useReceivedValue=true;error="SD_STATUS_ACTION_INVALID: select an SD target and no value";}
    if(mode=="wrong_action") {action.actionType=37;error="SD_STATUS_ACTION_TYPE_INVALID";}
    if(mode=="unmounted") card.mounted=false;
    if(mode=="capacity_invalid") card.capacityValid=false;
    if(mode=="capacity_bounds") card.usedBytes=card.totalBytes+1;
    if(mode=="recovery") {card.recoverySuggested=true;card.ioFailures=4;card.recoveries=2;}
    if(mode=="long_age") {card.capacityAgeMs=(1ULL<<32)+70000;card.capacitySampledAtMs=(1ULL<<33)+100;}
    ActionExecResult result; result.success=true; result.actualValue="old-good-sample";
    PeriphExecExecutor executor;
    const bool ok=executor.executeSdStatusAction(action,result);
    assert(rtcLockDepth==0);
    if(error) {
        assert(!ok && !result.success && result.actualValue.isEmpty() && result.remark==error);
        if(mode=="busy" || mode=="missing" || mode=="wrong_type" || mode=="disabled" || mode=="suspended" ||
           mode=="value" || mode=="received" || mode=="wrong_action") assert(snapshots==0);
    } else {
        assert(ok && result.success && snapshots==1 && rtcLockTimeout==0);
        assert(result.targetPeriphId=="sd_01" && result.remark=="sd_passive_snapshot");
        JsonDocument doc; assert(!deserializeJson(doc,result.actualValue));
        assert(doc["schema"].as<int>()==1 && doc["quality"]=="lifecycle_snapshot");
        assert(doc["driverError"].isNull() && doc["ownerId"].isNull());
        assert(doc["recoverySuggested"].as<bool>()==card.recoverySuggested);
        assert(doc["ioFailures"].as<uint32_t>()==card.ioFailures && doc["recoveries"].as<uint32_t>()==card.recoveries);
        if(mode=="unmounted" || mode=="capacity_invalid" || mode=="capacity_bounds") {
            assert(doc["freeBytes"].isNull() && doc["totalBytes"].isNull() && doc["capacityAgeMs"].isNull());
            assert(doc["capacityQuality"]=="unavailable");
        } else {
            assert(doc["totalBytes"].as<uint64_t>()==5000000000ULL);
            assert(doc["freeBytes"].as<uint64_t>()==3765432110ULL);
            assert(doc["capacityAgeMs"].as<uint64_t>()==card.capacityAgeMs);
            assert(doc["capacitySampledAtMs"].as<uint64_t>()==card.capacitySampledAtMs);
            assert(doc["capacityQuality"]=="last_operation_snapshot");
        }
    }
}
