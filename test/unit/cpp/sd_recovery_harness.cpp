#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include "network/SDCardStatusJson.h"
#include <SD.h>
#include <freertos/semphr.h>
#include <cassert>
#include <string>

int main(int argc,char** argv) {
    assert(argc==2);const std::string mode(argv[1]);
    auto& storage=SDCardManager::getInstance();auto& bus=FastBee::SpiBusInitializer::instance();
    String error,content="old";const uint8_t bytes[]={1,2,3};
    if(mode=="cold") {
        assert(!storage.recover("sd",error) && error.startsWith("SD_OWNER_MISMATCH"));
        assert(SD.begins==0 && SD.ends==0 && SPI.begins==0);return 0;
    }
    assert(storage.configureAndMount("sd",18,23,19,5,1000000,error));
    if(mode=="unmounted_reads" || mode=="unmounted_writes") {
        assert(storage.unmount("sd",&error,false));
        const int begins=SD.begins,opens=SD.opens,ends=SD.ends;
        if(mode=="unmounted_reads") {
            std::vector<SDCardEntry> entries(1);entries[0].path="old";
            assert(!storage.list("/fastbee",entries,error,"sd") && error.startsWith("SD_NOT_MOUNTED"));
            assert(entries.size()==1 && entries[0].path=="old");
            assert(!storage.readFile("test.txt",content,error,100,"sd") && content=="old");
        } else {
            assert(!storage.writeFile("test.txt",bytes,3,false,error,"sd"));
            assert(!storage.removeFile("test.txt",error,"sd"));
            assert(SD.deletes==0 && sdWriteCalls==0);
        }
        assert(error.startsWith("SD_NOT_MOUNTED") && SD.begins==begins && SD.ends==ends && SD.opens==opens);
        assert(!storage.getStatus().mounted && !bus.isAcquired(&storage));
        assert(storage.mount(error,"sd"));
    } else if(mode=="owner" || mode=="busy") {
        const int begins=SD.begins,ends=SD.ends;
        if(mode=="owner") {
            error="stale";assert(!storage.recover("",error) && error.startsWith("SD_OWNER_MISMATCH"));
            assert(!storage.recover("another",error) && error.startsWith("SD_OWNER_MISMATCH"));
        } else {
            spiMutexFailTake=true;assert(!storage.recover("sd",error) && error.startsWith("SD_BUSY"));
            spiMutexFailTake=false;
        }
        assert(SD.begins==begins && SD.ends==ends && storage.getStatus().recoveryAttempts==0);
    } else if(mode=="write_failure") {
        sdWriteLimit=1;assert(!storage.writeFile("test.txt",bytes,3,false,error,"sd"));
        const auto failed=storage.getStatus();
        assert(failed.recoverySuggested && failed.ioFailures==1 && failed.lastFailureCode=="SD_WRITE_TRUNCATED");
        assert(!failed.capacityValid);
        assert(storage.recover("sd",error));
        const auto restored=storage.getStatus();
        assert(restored.mounted && !restored.recoverySuggested && restored.capacityValid && restored.lastError.isEmpty());
        assert(restored.ioFailures==1 && restored.lastFailureCode==failed.lastFailureCode);
        assert(restored.recoveryAttempts==1 && restored.recoveries==1 && sdWriteCalls==1);
    } else if(mode=="read_history") {
        sdReadFailAt=1;assert(!storage.readFile("test.txt",content,error,100,"sd"));
        sdReadFailAt=-1;assert(storage.readFile("test.txt",content,error,100,"sd"));
        auto status=storage.getStatus();
        assert(status.lastError.isEmpty() && status.recoverySuggested && status.ioFailures==1 && !status.capacityValid);
        assert(storage.unmount("sd",&error,false) && storage.mount(error,"sd"));
        status=storage.getStatus();assert(status.ioFailures==1 && !status.recoverySuggested && status.recoveries==0);
    } else if(mode=="mount_failure") {
        SD.mountOk=false;assert(!storage.recover("sd",error));
        auto status=storage.getStatus();
        assert(!status.mounted && status.configured && status.recoverySuggested && !status.capacityValid);
        assert(status.recoveryAttempts==1 && status.recoveries==0 && status.ioFailures==1);
        assert(status.lastFailureCode=="SD_MOUNT_FAILED" && !bus.isAcquired(&storage));
        const int begins=SD.begins;
        assert(!storage.readFile("test.txt",content,error,100,"sd") && SD.begins==begins);
        SD.mountOk=true;assert(storage.recover("sd",error));
        status=storage.getStatus();assert(status.recoveryAttempts==2 && status.recoveries==1 && status.ioFailures==1);
    } else if(mode=="shared_bus") {
        int peer;assert(bus.acquire(&peer,18,19,23,6,error));const int ends=SPI.ends;
        assert(storage.recover("sd",error));
        assert(SPI.ends==ends && bus.isAcquired(&peer) && bus.isAcquired(&storage));
        assert(bus.release(&peer,error));
    } else if(mode=="bus_lost" || mode=="lost_peer") {
        int peer;if(mode=="lost_peer")assert(bus.acquire(&peer,18,19,23,6,error));
        SPI.end();assert(!storage.readFile("test.txt",content,error,100,"sd"));
        assert(error.startsWith("SD_SPI_LEASE_LOST") && storage.getStatus().recoverySuggested);
        if(mode=="lost_peer") {
            const int begins=SD.begins;
            assert(!storage.recover("sd",error));
            assert(SD.begins==begins && bus.isAcquired(&peer) && !bus.isAcquired(&storage));
            assert(bus.release(&peer,error));
        }
        assert(storage.recover("sd",error) && storage.getStatus().mounted);
    } else if(mode=="delete_unconfirmed" || mode=="delete_failure") {
        if(mode=="delete_unconfirmed")SD.openOk=false;else SD.deleteOk=false;
        assert(!storage.removeFile("test.txt",error,"sd"));
        assert(SD.deletes==(mode=="delete_unconfirmed"?0:1));
        assert(storage.getStatus().ioFailures==1 && storage.getStatus().recoverySuggested);
        assert(storage.recover("sd",error));
        assert(SD.deletes==(mode=="delete_unconfirmed"?0:1)); // No replay.
    } else if(mode=="json_history") {
        sdWriteLimit=1;assert(!storage.writeFile("test.txt",bytes,3,false,error,"sd"));
        assert(storage.recover("sd",error));
        JsonDocument doc;auto data=doc.to<JsonObject>();
        FastBee::writeSdCardStatusJson(data,storage.getStatus(),"sd");
        assert(data["recoverySuggested"]==false && data["ioFailures"]==1 && data["recoveries"]==1);
        assert(data["lastFailureCode"]=="SD_WRITE_TRUNCATED");
        FastBee::writeSdCardStatusJson(data,storage.getStatus(),"other");
        assert(data["ioFailures"].isNull() && data["lastFailureCode"].isNull() && data["recoveries"].isNull());
        FastBee::writeSdCardStatusJson(data,SDCardStatus{},"sd");
        assert(data["recoverySuggested"].isNull());
    } else assert(false);
    assert(storage.unmount("sd",&error));
    assert(storage.getStatus().ioFailures==0 && storage.getStatus().recoveryAttempts==0);
    assert(bus.end(error));
}
